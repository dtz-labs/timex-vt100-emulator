"""PC-side runner for the audio link: pty, audio plumbing, master loop.

    python3 -m alink.main --aofile /tmp/zx-audio.raw --command /bin/zsh

The master owns every exchange (protocol v1). This module gives it a Channel
whose two halves use completely different plumbing, because the emulator
forces that:

  send()  renders the block to pulses and pushes PCM at ffmpeg, which plays it
          into the Loopback device ZEsarUX reads as External Audio Source
  recv()  follows ZEsarUX's --aofile dump, which is written synchronously with
          emulation and therefore never drops samples -- unlike its realtime
          CoreAudio output, whose FIFO overflows and eats milliseconds of tape
          data

On real hardware the asymmetry disappears: both directions are just audio.
See docs/audio-link-setup.md.
"""
import argparse
import os
import pty
import select
import shutil
import subprocess
import sys
import time

from . import frame, phy
from .master import Channel, Master


class AofileReader:
    """Follows a growing raw dump like `tail -f`, yielding decoded blocks.

    ZEsarUX appends to this file as it emulates. We keep our own read offset
    and hand every new byte to one long-lived PulseDecoder, so a frame split
    across two reads still decodes -- which is the normal case, since a frame
    takes ~0.43 s and we poll far faster than that.
    """

    def __init__(self, path):
        self.path = path
        self.decoder = phy.PulseDecoder()
        self.offset = 0
        self.pending = []

    def poll(self):
        """Read whatever has appeared and return any complete blocks."""
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        if size < self.offset:
            # The file was recreated -- ZEsarUX restarted. Start over, or we
            # would decode the tail of a dump that no longer exists.
            self.offset = 0
            self.decoder = phy.PulseDecoder()
        if size == self.offset:
            return []
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            chunk = f.read(size - self.offset)
        self.offset += len(chunk)
        return list(self.decoder.feed(b - 128 for b in chunk))


class FfmpegSender:
    """Plays PCM into a named CoreAudio output device via ffmpeg."""

    def __init__(self, device):
        self.device = device
        self.ffmpeg = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"

    def play(self, pcm):
        subprocess.run(
            [self.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
             "-f", "s16le", "-ar", str(phy.SAMPLE_RATE), "-ac", "1", "-i", "-",
             "-f", "audiotoolbox", self.device],
            input=pcm, check=False,
        )


class AudioChannel(Channel):
    """Half-duplex audio transport for the Master."""

    def __init__(self, sender, reader):
        self.sender = sender
        self.reader = reader
        self.queue = []

    def send(self, block):
        # Drain anything already captured before transmitting, so a stale
        # block cannot be mistaken for the response to this frame.
        self.reader.poll()
        self.sender.play(phy.pulses_to_pcm(phy.encode_block(block)))

    def recv(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.queue:
                return self.queue.pop(0)
            self.queue.extend(self.reader.poll())
            if self.queue:
                return self.queue.pop(0)
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.02)


def run_pty(master, command, cols, rows, term):
    """Bridge a pty to the master until the child exits or the link dies."""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = term
        os.environ["COLUMNS"] = str(cols)
        os.environ["LINES"] = str(rows)
        os.execvp(command[0], command)

    try:
        import fcntl
        import struct
        import termios
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
    except Exception:
        pass

    try:
        while True:
            ready, _, _ = select.select([fd], [], [], 0)
            if ready:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    break
                if not data:
                    break
                master.send(data)

            if not master.transact():
                if master.state == "DOWN":
                    print("alink: link down, retrying HELLO", file=sys.stderr)
                    time.sleep(master.t_hello_retry)
                continue

            if master.received:
                os.write(fd, bytes(master.received))
                master.received.clear()
    except KeyboardInterrupt:
        pass
    finally:
        master.close()
        os.close(fd)


def selftest():
    """Prove the runner's own plumbing without audio hardware.

    Feeds AofileReader a file the encoder wrote, and checks the blocks come
    back out. This is the part that breaks silently in the field -- a wrong
    offset, a recreated dump, a decoder that is not long-lived -- and none of
    it needs an emulator to test.
    """
    path = "/tmp/alink_selftest.raw"
    blocks = [frame.encode(frame.LINK, 0, 1, b"first"),
              frame.encode(frame.LINK, 1, 1, b"second")]
    pulses = []
    for b in blocks:
        pulses.extend(phy.encode_block(b))
        pulses.append(phy.PILOT_T * 8)
    data = phy.pulses_to_raw_u8(pulses)

    reader = AofileReader(path)
    with open(path, "wb") as f:
        f.write(data[:len(data) // 3])
    got = reader.poll()
    with open(path, "ab") as f:
        f.write(data[len(data) // 3:])
    got += reader.poll()

    os.unlink(path)
    if got != blocks:
        print(f"selftest FAILED: got {got!r}", file=sys.stderr)
        return 1
    print(f"selftest OK: {len(got)} blocks recovered across two reads")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--aofile", help="ZEsarUX --aofile dump to follow")
    ap.add_argument("--device", default="ZX Link",
                    help="CoreAudio output device name (default: ZX Link)")
    ap.add_argument("--command", default=os.environ.get("SHELL", "/bin/sh"))
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("--term", default=None,
                    help="TERM for the pty (default: by column count)")
    ap.add_argument("--selftest", action="store_true",
                    help="check the plumbing without audio or an emulator")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.aofile:
        ap.error("--aofile is required unless --selftest is given")

    term = args.term or ("zx-vt102" if args.cols <= 40 else "timex-vt102")
    channel = AudioChannel(FfmpegSender(args.device), AofileReader(args.aofile))
    master = Master(channel)
    run_pty(master, [args.command], args.cols, args.rows, term)
    return 0


if __name__ == "__main__":
    sys.exit(main())
