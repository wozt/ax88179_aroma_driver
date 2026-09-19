#!/usr/bin/env python3

import socket
import time

HOST = "192.168.2.100"
PORT = 19041
TOTAL = 256 * 1024
CHUNK = 1460
TESTS = 6

listener = socket.socket(
    socket.AF_INET,
    socket.SOCK_STREAM
)

listener.setsockopt(
    socket.SOL_SOCKET,
    socket.SO_REUSEADDR,
    1
)

listener.bind((HOST, PORT))
listener.listen(4)

print(
    f"RX pressure peer listening on {HOST}:{PORT}",
    flush=True,
)

payload = bytes(
    (i * 31) & 0xff
    for i in range(CHUNK)
)

try:
    for test in range(1, TESTS + 1):
        conn, addr = listener.accept()

        conn.setsockopt(
            socket.IPPROTO_TCP,
            socket.TCP_NODELAY,
            1
        )

        print(
            f"[{test}/{TESTS}] connected {addr}",
            flush=True,
        )

        sent = 0
        start = time.monotonic()

        while sent < TOTAL:
            n = min(CHUNK, TOTAL - sent)
            conn.sendall(payload[:n])
            sent += n

        elapsed = time.monotonic() - start

        print(
            f"[{test}/{TESTS}] send completed "
            f"bytes={sent} time={elapsed:.3f}s",
            flush=True,
        )

        conn.shutdown(socket.SHUT_WR)

        conn.settimeout(10)

        try:
            reply = conn.recv(128)
        except (socket.timeout, ConnectionResetError):
            reply = b""

        print(
            f"[{test}/{TESTS}] reply={reply!r}",
            flush=True,
        )

        conn.close()

finally:
    listener.close()
