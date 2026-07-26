#!/usr/bin/env python3
"""
Full-duplex stdio bridge for the ZEsarUX emulator build.

stdin  -> conn_zrcp_inject_data/len  -> terminal RX
stdout <- conn_zrcp_output_data/len  <- terminal TX
"""

import argparse
import errno
import fcntl
import os
import re
import select
import signal
import socket
import struct
import sys
import termios
import time


DEFAULT_MAP = os.path.join("build", "term.map")
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10001
DEFAULT_CHUNK = 16
PROMPT = b"command> "
NEWLINE_MODES = ("raw", "lf", "cr", "crlf")
BRIDGE_ENABLE = 0x01
BRIDGE_LOCAL_ECHO = 0x02
BRIDGE_LOCAL_ECHO_CRLF = 0x04


class BridgeError(RuntimeError):
    pass


class NewlineTranslator:
    def __init__(self, mode):
        self.mode = mode
        self.pending_cr = False

    def _newline(self):
        if self.mode == "lf":
            return b"\n"
        if self.mode == "cr":
            return b"\r"
        if self.mode == "crlf":
            return b"\r\n"
        return b""

    def translate(self, data, flush_pending=False):
        out = bytearray()
        i = 0

        if self.mode == "raw" or not data:
            return data

        if self.pending_cr:
            if data[0] == 0x0A:
                out.extend(self._newline())
                i = 1
            else:
                out.extend(self._newline())
            self.pending_cr = False

        while i < len(data):
            b = data[i]
            if b == 0x0D:
                if i + 1 == len(data):
                    self.pending_cr = True
                    i += 1
                elif data[i + 1] == 0x0A:
                    out.extend(self._newline())
                    i += 2
                else:
                    out.extend(self._newline())
                    i += 1
            elif b == 0x0A:
                out.extend(self._newline())
                i += 1
            else:
                out.append(b)
                i += 1
        if flush_pending and self.pending_cr:
            self.pending_cr = False
            out.extend(self._newline())
        return bytes(out)

    def flush(self):
        if self.mode == "raw" or not self.pending_cr:
            return b""
        self.pending_cr = False
        return self._newline()


def parse_int(text):
    if text.startswith("$"):
        return int(text[1:], 16)
    return int(text, 0)


def parse_map(path):
    symbols = {}
    pattern = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\$([0-9A-Fa-f]+)\b")

    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            match = pattern.match(line)
            if match:
                symbols[match.group(1)] = int(match.group(2), 16)
    return symbols


def find_symbol(symbols, name):
    for candidate in (name, "_" + name):
        if candidate in symbols:
            return symbols[candidate]
    raise BridgeError("symbol not found in map: %s" % name)


def recv_prompt(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    deadline = time.monotonic() + timeout

    while not buf.endswith(PROMPT):
        if time.monotonic() > deadline:
            raise BridgeError("timeout waiting for ZRCP prompt")
        try:
            data = sock.recv(4096)
        except socket.timeout as exc:
            raise BridgeError("timeout waiting for ZRCP prompt") from exc
        if not data:
            raise BridgeError("ZRCP connection closed")
        buf += data
    return buf.decode("latin-1", "replace")


def command(sock, text, timeout):
    sock.sendall((text + "\n").encode("latin-1"))
    response = recv_prompt(sock, timeout)
    lowered = response.lower()
    if "unknown command" in lowered or "error" in lowered:
        raise BridgeError("ZRCP command failed: %s: %s" % (text, response.strip()))
    return response


def first_hex_line(response):
    for line in response.splitlines():
        line = line.strip()
        if re.fullmatch(r"[0-9A-Fa-f]+", line) and len(line) >= 2:
            return line
    raise BridgeError("no hex payload in ZRCP response: %s" % response.strip())


def read_memory(sock, addr, size, timeout):
    if size == 0:
        return b""
    response = command(sock, "read-memory %d %d" % (addr, size), timeout)
    data = bytes.fromhex(first_hex_line(response))
    if len(data) < size:
        raise BridgeError("short read from ZRCP memory")
    return data[:size]


def read_byte(sock, addr, timeout):
    return read_memory(sock, addr, 1, timeout)[0]


def write_bytes(sock, addr, data, timeout):
    if not data:
        return
    values = " ".join(str(b) for b in data)
    command(sock, "write-memory %d %s" % (addr, values), timeout)


def os_write_all(fd, data):
    view = memoryview(data)
    while view:
        try:
            n = os.write(fd, view)
            view = view[n:]
        except BlockingIOError:
            select.select([], [fd], [])
        except InterruptedError:
            continue


def set_nonblocking(fd):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def set_cc(attrs, index, value):
    current = attrs[6][index]
    attrs[6][index] = bytes((value,)) if isinstance(current, bytes) else value


def configure_pty_slave(fd):
    attrs = termios.tcgetattr(fd)
    set_cc(attrs, termios.VERASE, 0x08)  # Timex/Spectrum backspace is Ctrl-H.
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def default_command():
    shell = os.environ.get("SHELL") or "/bin/sh"
    return [shell, "-l"]


def spawn_pty(command, rows, cols, term):
    master, slave = os.openpty()
    set_winsize(slave, rows, cols)
    configure_pty_slave(slave)
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


def resolve_addresses(args):
    symbols = None

    def sym(name):
        nonlocal symbols
        if symbols is None:
            if not args.map:
                raise BridgeError("provide --map or explicit mailbox addresses")
            if not os.path.exists(args.map):
                raise BridgeError("map file not found: %s" % args.map)
            symbols = parse_map(args.map)
        return find_symbol(symbols, name)

    return {
        "flags": parse_int(args.flags_addr)
        if args.flags_addr
        else sym(args.flags_symbol),
        "in_len": parse_int(args.in_len_addr)
        if args.in_len_addr
        else sym(args.in_len_symbol),
        "in_data": parse_int(args.in_data_addr)
        if args.in_data_addr
        else sym(args.in_data_symbol),
        "out_len": parse_int(args.out_len_addr)
        if args.out_len_addr
        else sym(args.out_len_symbol),
        "out_data": parse_int(args.out_data_addr)
        if args.out_data_addr
        else sym(args.out_data_symbol),
    }


class Bridge:
    def __init__(self, args, addrs):
        self.args = args
        self.addrs = addrs
        self.sock = None
        self.stdin_fd = sys.stdin.fileno()
        self.stdout_fd = sys.stdout.fileno()
        self.stdin_restore = None
        self.child_pid = None
        self.pty_fd = None
        self.last_output = time.monotonic()
        self.input_newline = NewlineTranslator(args.input_newline)
        self.output_newline = NewlineTranslator(args.output_newline)
        self.local_echo_newline = NewlineTranslator(args.local_echo_newline)

    def log(self, text):
        if self.args.verbose:
            print(text, file=sys.stderr)

    def connect(self):
        self.sock = socket.create_connection(
            (self.args.host, self.args.port), timeout=self.args.timeout
        )
        recv_prompt(self.sock, self.args.timeout)
        if not self.args.no_clear:
            write_bytes(self.sock, self.addrs["in_len"], b"\x00", self.args.timeout)
            write_bytes(self.sock, self.addrs["out_len"], b"\x00", self.args.timeout)
        flags = BRIDGE_ENABLE
        write_bytes(self.sock, self.addrs["flags"], bytes((flags,)), self.args.timeout)
        self.log(
            "mailbox flags=$%04X value=$%02X local_echo=%s in=$%04X/$%04X out=$%04X/$%04X"
            % (
                self.addrs["flags"],
                flags,
                "on" if self.args.local_echo else "off",
                self.addrs["in_len"],
                self.addrs["in_data"],
                self.addrs["out_len"],
                self.addrs["out_data"],
            )
        )

    def close(self):
        if self.sock is None:
            return
        if not self.args.keep_enabled:
            try:
                write_bytes(self.sock, self.addrs["flags"], b"\x00", self.args.timeout)
            except (OSError, BridgeError):
                pass
        self.sock.close()
        self.sock = None

    def start_command(self):
        if self.args.command is None:
            return
        command = self.args.command if self.args.command else default_command()
        if command and command[0] == "--":
            command = command[1:]
        if not command:
            command = default_command()
        self.child_pid, self.pty_fd = spawn_pty(
            command,
            self.args.rows,
            self.args.cols,
            self.args.term,
        )
        self.log(
            "pty cmd=%s rows=%d cols=%d TERM=%s"
            % (" ".join(command), self.args.rows, self.args.cols, self.args.term)
        )

    def stop_command(self):
        if self.child_pid is not None:
            try:
                os.kill(self.child_pid, signal.SIGHUP)
            except OSError:
                pass
            try:
                os.waitpid(self.child_pid, os.WNOHANG)
            except OSError:
                pass
            self.child_pid = None
        if self.pty_fd is not None:
            os.close(self.pty_fd)
            self.pty_fd = None

    def child_exited(self):
        if self.child_pid is None:
            return False
        try:
            done, _ = os.waitpid(self.child_pid, os.WNOHANG)
        except ChildProcessError:
            self.child_pid = None
            return True
        if done == self.child_pid:
            self.child_pid = None
            return True
        return False

    def setup_stdin(self):
        if self.args.stdin_mode != "immediate" or not os.isatty(self.stdin_fd):
            return
        attrs = termios.tcgetattr(self.stdin_fd)
        new_attrs = attrs[:]
        new_attrs[6] = attrs[6][:]
        new_attrs[3] &= ~(termios.ICANON | termios.ECHO)
        new_attrs[6][termios.VMIN] = 1
        new_attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.stdin_fd, termios.TCSADRAIN, new_attrs)
        self.stdin_restore = attrs
        self.log("stdin mode: immediate")

    def restore_stdin(self):
        if self.stdin_restore is None:
            return
        termios.tcsetattr(self.stdin_fd, termios.TCSADRAIN, self.stdin_restore)
        self.stdin_restore = None

    def wait_input_empty(self, pump_output):
        deadline = time.monotonic() + self.args.input_timeout

        while True:
            if pump_output:
                self.poll_output()
            if read_byte(self.sock, self.addrs["in_len"], self.args.timeout) == 0:
                return
            if time.monotonic() > deadline:
                raise BridgeError("timeout waiting for target input mailbox to empty")
            time.sleep(self.args.poll_interval)

    def send_input(self, data, pump_output=True):
        self.wait_input_empty(pump_output)
        write_bytes(self.sock, self.addrs["in_data"], data, self.args.timeout)
        write_bytes(
            self.sock, self.addrs["in_len"], bytes((len(data),)), self.args.timeout
        )
        if self.args.send_delay > 0:
            time.sleep(self.args.send_delay)
        self.wait_input_empty(pump_output)

    def send_input_stream(self, data, pump_output=True):
        pos = 0
        while pos < len(data):
            chunk = data[pos : pos + self.args.mailbox_size]
            self.send_input(chunk, pump_output)
            pos += len(chunk)

    def poll_output(self):
        n = read_byte(self.sock, self.addrs["out_len"], self.args.timeout)
        if n == 0:
            return False
        if n > self.args.mailbox_size:
            raise BridgeError("target output length %d exceeds mailbox size" % n)
        raw_data = read_memory(self.sock, self.addrs["out_data"], n, self.args.timeout)
        write_bytes(self.sock, self.addrs["out_len"], b"\x00", self.args.timeout)
        if self.pty_fd is not None:
            os_write_all(self.pty_fd, raw_data)
            self.last_output = time.monotonic()
            return True
        if self.args.local_echo:
            self.send_input_stream(
                self.local_echo_newline.translate(raw_data, flush_pending=True),
                pump_output=False,
            )
        data = self.output_newline.translate(raw_data, flush_pending=True)
        os_write_all(self.stdout_fd, data)
        self.last_output = time.monotonic()
        return True

    def flush_output(self):
        data = self.output_newline.flush()
        if data:
            os_write_all(self.stdout_fd, data)
            self.last_output = time.monotonic()

    def run(self):
        stdin_open = True
        stdin_eof_at = None

        self.connect()
        self.start_command()
        if self.pty_fd is None:
            self.setup_stdin()
        try:
            while True:
                while self.poll_output():
                    pass

                if self.pty_fd is not None:
                    readable, _, _ = select.select(
                        [self.pty_fd], [], [], self.args.poll_interval
                    )
                    if readable:
                        try:
                            data = os.read(self.pty_fd, self.args.chunk_size)
                        except OSError as exc:
                            if exc.errno == errno.EIO:
                                return
                            raise
                        if data:
                            self.send_input_stream(data)
                        else:
                            return
                    elif self.child_exited():
                        return
                elif stdin_open:
                    readable, _, _ = select.select(
                        [self.stdin_fd], [], [], self.args.poll_interval
                    )
                    if readable:
                        data = os.read(self.stdin_fd, self.args.chunk_size)
                        if data:
                            self.send_input_stream(self.input_newline.translate(data))
                        else:
                            self.send_input_stream(self.input_newline.flush())
                            stdin_open = False
                            stdin_eof_at = time.monotonic()
                            self.log("stdin EOF")
                else:
                    time.sleep(self.args.poll_interval)
                    if self.args.drain_after_eof >= 0:
                        idle = time.monotonic() - max(
                            stdin_eof_at or 0, self.last_output
                        )
                        if idle >= self.args.drain_after_eof:
                            return
        finally:
            self.restore_stdin()
            self.stop_command()
            self.flush_output()
            self.close()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Bridge local stdin/stdout to a running ZEsarUX terminal build via ZRCP."
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--map", default=DEFAULT_MAP)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument(
        "--input-timeout",
        type=float,
        default=60.0,
        help="Seconds to wait for the target to drain its input mailbox; the "
        "target may be busy (e.g. an app still booting) and not reading keys.",
    )
    parser.add_argument("--poll-interval", type=float, default=0.02)
    parser.add_argument("--send-delay", type=float, default=0.02)
    parser.add_argument("--drain-after-eof", type=float, default=2.0)
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK)
    parser.add_argument("--mailbox-size", type=int, default=32)
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--term", default="vt100")
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Use raw protocol defaults: no local echo, no newline translation.",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Use interactive defaults: immediate stdin, local echo, input CR, output LF.",
    )
    parser.add_argument("--stdin-mode", choices=("immediate", "line"), default=None)
    parser.add_argument("--input-newline", choices=NEWLINE_MODES, default=None)
    parser.add_argument("--output-newline", choices=NEWLINE_MODES, default=None)
    parser.add_argument("--local-echo", action="store_true")
    parser.add_argument("--local-echo-newline", choices=("raw", "crlf"), default="crlf")
    parser.add_argument("--no-clear", action="store_true")
    parser.add_argument("--keep-enabled", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")

    parser.add_argument("--flags-symbol", default="conn_zrcp_bridge_flags")
    parser.add_argument("--in-len-symbol", default="conn_zrcp_inject_len")
    parser.add_argument("--in-data-symbol", default="conn_zrcp_inject_data")
    parser.add_argument("--out-len-symbol", default="conn_zrcp_output_len")
    parser.add_argument("--out-data-symbol", default="conn_zrcp_output_data")

    parser.add_argument("--flags-addr")
    parser.add_argument("--in-len-addr")
    parser.add_argument("--in-data-addr")
    parser.add_argument("--out-len-addr")
    parser.add_argument("--out-data-addr")
    parser.add_argument(
        "--cmd",
        dest="command",
        nargs=argparse.REMAINDER,
        help="run a Unix command under a PTY; defaults to $SHELL -l when used without a command",
    )
    args = parser.parse_args(argv)

    if args.command is not None:
        args.raw = True

    if args.raw and args.interactive:
        raise SystemExit("--raw and --interactive are mutually exclusive")

    if args.interactive or not args.raw:
        if args.stdin_mode is None:
            args.stdin_mode = "immediate"
        if args.input_newline is None:
            args.input_newline = "cr"
        if args.output_newline is None:
            args.output_newline = "lf"
        args.local_echo = True
    else:
        if args.stdin_mode is None:
            args.stdin_mode = "immediate"
        if args.input_newline is None:
            args.input_newline = "raw"
        if args.output_newline is None:
            args.output_newline = "raw"

    if args.chunk_size < 1 or args.chunk_size > args.mailbox_size:
        raise SystemExit("--chunk-size must be in range 1..mailbox-size")
    if args.mailbox_size < 1 or args.mailbox_size > 255:
        raise SystemExit("--mailbox-size must be in range 1..255")

    try:
        addrs = resolve_addresses(args)
        Bridge(args, addrs).run()
        return 0
    except BrokenPipeError:
        return 1
    except KeyboardInterrupt:
        return 130
    except (OSError, BridgeError, ValueError) as exc:
        print("zesarux_stdio_bridge.py: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
