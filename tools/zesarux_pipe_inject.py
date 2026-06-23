#!/usr/bin/env python3
"""
Send stdin bytes into the emulator build through ZEsarUX ZRCP.

The target-side default conn backend exposes a tiny mailbox:
    conn_zrcp_inject_data[CONN_ZRCP_INJECT_MAX]
    conn_zrcp_inject_len

This script writes data first and the length byte last. conn_poll() then moves
the chunk into RX and clears the length byte back to zero.
"""

import argparse
import os
import re
import socket
import sys
import time


DEFAULT_MAP = os.path.join("build", "term.map")
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10001
DEFAULT_CHUNK = 32
PROMPT = b"command> "


class ZrcpError(RuntimeError):
    pass


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
    raise ZrcpError("symbol not found in map: %s" % name)


def recv_prompt(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    deadline = time.monotonic() + timeout

    while not buf.endswith(PROMPT):
        if time.monotonic() > deadline:
            raise ZrcpError("timeout waiting for ZRCP prompt")
        try:
            data = sock.recv(4096)
        except socket.timeout as exc:
            raise ZrcpError("timeout waiting for ZRCP prompt") from exc
        if not data:
            raise ZrcpError("ZRCP connection closed")
        buf += data
    return buf.decode("latin-1", "replace")


def command(sock, text, timeout):
    sock.sendall((text + "\n").encode("latin-1"))
    response = recv_prompt(sock, timeout)
    lowered = response.lower()
    if "unknown command" in lowered or "error" in lowered:
        raise ZrcpError("ZRCP command failed: %s: %s" % (text, response.strip()))
    return response


def first_hex_line(response):
    for line in response.splitlines():
        line = line.strip()
        if re.fullmatch(r"[0-9A-Fa-f]+", line) and len(line) >= 2:
            return line
    raise ZrcpError("no hex payload in ZRCP response: %s" % response.strip())


def read_byte(sock, addr, timeout):
    response = command(sock, "read-memory %d 1" % addr, timeout)
    return bytes.fromhex(first_hex_line(response))[0]


def write_bytes(sock, addr, data, timeout):
    if not data:
        return
    values = " ".join(str(b) for b in data)
    command(sock, "write-memory %d %s" % (addr, values), timeout)


def wait_empty(sock, len_addr, timeout):
    deadline = time.monotonic() + timeout

    while True:
        if read_byte(sock, len_addr, timeout) == 0:
            return
        if time.monotonic() > deadline:
            raise ZrcpError("timeout waiting for target mailbox to empty")
        time.sleep(0.02)


def inject_chunk(sock, len_addr, data_addr, chunk, timeout):
    wait_empty(sock, len_addr, timeout)
    write_bytes(sock, data_addr, chunk, timeout)
    write_bytes(sock, len_addr, bytes((len(chunk),)), timeout)
    wait_empty(sock, len_addr, timeout)


def resolve_addresses(args):
    len_addr = parse_int(args.len_addr) if args.len_addr else None
    data_addr = parse_int(args.data_addr) if args.data_addr else None

    if len_addr is not None and data_addr is not None:
        return len_addr, data_addr
    if not args.map:
        raise ZrcpError("provide --map or both --len-addr and --data-addr")
    if not os.path.exists(args.map):
        raise ZrcpError("map file not found: %s" % args.map)

    symbols = parse_map(args.map)
    if len_addr is None:
        len_addr = find_symbol(symbols, args.len_symbol)
    if data_addr is None:
        data_addr = find_symbol(symbols, args.data_symbol)
    return len_addr, data_addr


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Pipe stdin bytes into a running ZEsarUX TC2048 session via ZRCP."
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--map", default=DEFAULT_MAP)
    parser.add_argument("--len-symbol", default="conn_zrcp_inject_len")
    parser.add_argument("--data-symbol", default="conn_zrcp_inject_data")
    parser.add_argument("--len-addr")
    parser.add_argument("--data-addr")
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.chunk_size < 1 or args.chunk_size > 255:
        raise SystemExit("--chunk-size must be in range 1..255")

    try:
        len_addr, data_addr = resolve_addresses(args)
        stdin = getattr(sys.stdin, "buffer", sys.stdin)
        total = 0

        with socket.create_connection(
            (args.host, args.port), timeout=args.timeout
        ) as sock:
            recv_prompt(sock, args.timeout)
            if args.verbose:
                print(
                    "mailbox len=$%04X data=$%04X chunk=%d"
                    % (len_addr, data_addr, args.chunk_size),
                    file=sys.stderr,
                )
            while True:
                chunk = stdin.read(args.chunk_size)
                if not chunk:
                    break
                inject_chunk(sock, len_addr, data_addr, chunk, args.timeout)
                total += len(chunk)
                if args.verbose:
                    print("sent %d bytes" % total, file=sys.stderr)
        print(
            "sent %d bytes to ZEsarUX ZRCP %s:%d" % (total, args.host, args.port),
            file=sys.stderr,
        )
        return 0
    except (OSError, ZrcpError, ValueError) as exc:
        print("zesarux_pipe_inject.py: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
