#!/usr/bin/env python3

import socket
import sys
import time

HOST = "192.168.2.100"
PORT = 19040
STALL_SECONDS = 2.0
TESTS = 5
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

# Set before listen so the advertised receive window is deliberately small
# and inherited by accepted sockets.
listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)

listener.bind((HOST, PORT))
listener.listen(4)

print(
    f"Backpressure peer listening on {HOST}:{PORT}, "
    f"SO_RCVBUF={listener.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)}",
    flush=True,
)

try:
    for test in range(1, TESTS + 1):
        conn, addr = listener.accept()

        conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)

        actual = conn.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)

        print(
            f"[{test}/{TESTS}] accept {addr}, "
            f"SO_RCVBUF={actual}, stalling reads {STALL_SECONDS:.1f}s",
            flush=True,
        )

        # Deliberately advertise/consume no additional application data.
        time.sleep(STALL_SECONDS)

        total = 0

        while True:
            data = conn.recv(65536)

            if not data:
                break

            total += len(data)

        reply = f"DRAINED {total}\n".encode()
        conn.sendall(reply)

        print(
            f"[{test}/{TESTS}] drained={total}",
            flush=True,
        )

        conn.close()

finally:
    listener.close()
