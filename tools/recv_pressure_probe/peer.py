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

        # A pathological Wii U receive window must not be able to hang
        # or terminate the complete six-case characterization.
        conn.settimeout(20.0)

        print(
            f"[{test}/{TESTS}] connected {addr}",
            flush=True,
        )

        sent = 0
        send_error = None
        start = time.monotonic()

        try:
            while sent < TOTAL:
                want = min(CHUNK, TOTAL - sent)

                n = conn.send(payload[:want])

                if n <= 0:
                    raise ConnectionError(
                        f"send returned {n}"
                    )

                sent += n

        except (
            BrokenPipeError,
            ConnectionResetError,
            ConnectionAbortedError,
            TimeoutError,
            socket.timeout,
            OSError,
        ) as exc:
            send_error = (
                f"{type(exc).__name__}: {exc}"
            )

        elapsed = time.monotonic() - start

        print(
            f"[{test}/{TESTS}] send result "
            f"bytes={sent}/{TOTAL} "
            f"time={elapsed:.3f}s "
            f"error={send_error!r}",
            flush=True,
        )

        try:
            conn.shutdown(socket.SHUT_WR)
        except OSError:
            pass

        reply = b""

        try:
            conn.settimeout(3.0)
            reply = conn.recv(128)
        except (
            socket.timeout,
            BrokenPipeError,
            ConnectionResetError,
            OSError,
        ):
            pass

        print(
            f"[{test}/{TESTS}] reply={reply!r}",
            flush=True,
        )

        try:
            conn.close()
        except OSError:
            pass

finally:
    listener.close()
