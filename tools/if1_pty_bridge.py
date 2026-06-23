#!/usr/bin/env python3
"""
Bridge a ZX Interface 1 RS-232 serial port to a local pseudo-terminal.

The Timex side speaks raw VT102-ish bytes. This program opens a serial device,
spawns a local shell/editor side in a PTY, sets the terminal to 64x24, and copies
bytes in both directions.
"""

import argparse
import errno
import fcntl
import os
import select
import signal
import struct
import sys
import termios


BAUDS = {
    300: termios.B300,
    600: termios.B600,
    1200: termios.B1200,
    2400: termios.B2400,
    4800: termios.B4800,
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Bridge ZX Interface 1 RS-232 to a local 64x24 VT102 PTY."
    )
    parser.add_argument("serial", help="serial device, e.g. /dev/cu.usbserial-0001")
    parser.add_argument("--baud", type=int, default=9600, choices=sorted(BAUDS))
    parser.add_argument("--cols", type=int, default=64)
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--term", default="vt100")
    parser.add_argument(
        "--cmd",
        nargs=argparse.REMAINDER,
        help="command to run in the PTY; defaults to $SHELL -l",
    )
    return parser.parse_args()


def set_nonblocking(fd):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def configure_serial(fd, baud):
    old = termios.tcgetattr(fd)
    attrs = termios.tcgetattr(fd)

    attrs[0] = 0  # iflag: raw input
    attrs[1] = 0  # oflag: raw output
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    attrs[2] &= ~termios.PARENB
    attrs[2] &= ~termios.CSTOPB
    if hasattr(termios, "CRTSCTS"):
        attrs[2] &= ~termios.CRTSCTS
    attrs[3] = 0  # lflag: no echo/canonical/signals
    attrs[4] = BAUDS[baud]
    attrs[5] = BAUDS[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0

    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return old


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def default_command():
    shell = os.environ.get("SHELL") or "/bin/sh"
    return [shell, "-l"]


def spawn_pty(command, rows, cols, term):
    master, slave = os.openpty()
    set_winsize(slave, rows, cols)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.close(master)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        if slave > 2:
            os.close(slave)

        env = os.environ.copy()
        env["TERM"] = term
        env["COLUMNS"] = str(cols)
        env["LINES"] = str(rows)
        os.execvpe(command[0], command, env)

    os.close(slave)
    set_nonblocking(master)
    return pid, master


def write_all(fd, data):
    view = memoryview(data)
    while view:
        try:
            n = os.write(fd, view)
            view = view[n:]
        except BlockingIOError:
            select.select([], [fd], [])
        except InterruptedError:
            continue


def bridge(serial_fd, pty_fd, child_pid):
    fds = [serial_fd, pty_fd]
    while True:
        try:
            readable, _, _ = select.select(fds, [], [], 0.5)
        except InterruptedError:
            continue

        try:
            done, _ = os.waitpid(child_pid, os.WNOHANG)
            if done == child_pid:
                return
        except ChildProcessError:
            return

        for fd in readable:
            try:
                data = os.read(fd, 1024)
            except OSError as exc:
                if exc.errno == errno.EIO and fd == pty_fd:
                    return
                raise
            if not data:
                return
            if fd == serial_fd:
                write_all(pty_fd, data)
            else:
                write_all(serial_fd, data)


def main():
    args = parse_args()
    command = args.cmd if args.cmd else default_command()
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise SystemExit("--cmd requires a command after it")

    serial_fd = os.open(args.serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    old_serial = None
    child_pid = None
    pty_fd = None

    def stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    try:
        old_serial = configure_serial(serial_fd, args.baud)
        child_pid, pty_fd = spawn_pty(command, args.rows, args.cols, args.term)
        print(
            f"IF1 bridge: {args.serial} @ {args.baud}, "
            f"{args.cols}x{args.rows}, TERM={args.term}, cmd={' '.join(command)}",
            file=sys.stderr,
            flush=True,
        )
        bridge(serial_fd, pty_fd, child_pid)
    except KeyboardInterrupt:
        pass
    finally:
        if child_pid is not None:
            try:
                os.kill(child_pid, signal.SIGHUP)
            except OSError:
                pass
        if old_serial is not None:
            termios.tcsetattr(serial_fd, termios.TCSANOW, old_serial)
        if pty_fd is not None:
            os.close(pty_fd)
        os.close(serial_fd)


if __name__ == "__main__":
    main()
