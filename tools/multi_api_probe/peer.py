#!/usr/bin/env python3

import select
import socket
import sys
import threading
import time

TARGET_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.124"

RX_PORT = 19020
TX_PORT_A = 19021
TX_PORT_B = 19022

DURATION = 30.0

rx_packets = [
    b"RX-ONE",
    b"RX-TWO22",
    b"RX-THREE-333",
]


def sender():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Important:
    # keep sending for the ENTIRE test duration.
    # Do not stop when sendto_multi_ex succeeds.
    deadline = time.monotonic() + DURATION

    seq = 0

    while time.monotonic() < deadline:
        for payload in rx_packets:
            sock.sendto(payload, (TARGET_IP, RX_PORT))

        seq += 1

        if seq == 1:
            print(
                f"TX -> {TARGET_IP}:{RX_PORT}: "
                + ", ".join(repr(x) for x in rx_packets),
                flush=True,
            )

        time.sleep(0.25)

    sock.close()


def make_listener(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.setblocking(False)
    return sock


a = make_listener(TX_PORT_A)
b = make_listener(TX_PORT_B)

print("PC multi-API peer ready", flush=True)
print(f"Target Wii U: {TARGET_IP}", flush=True)
print(
    f"Listening on UDP {TX_PORT_A} + {TX_PORT_B}",
    flush=True,
)
print(
    f"Sending RX test datagrams to UDP {RX_PORT} for {DURATION:.0f}s",
    flush=True,
)
print("Launch AX Multi API Probe now.", flush=True)

thread = threading.Thread(target=sender, daemon=True)
thread.start()

deadline = time.monotonic() + DURATION

while time.monotonic() < deadline:
    ready, _, _ = select.select([a, b], [], [], 0.25)

    for sock in ready:
        data, addr = sock.recvfrom(65535)

        print(
            f"RX port={sock.getsockname()[1]} "
            f"from={addr} data={data!r}",
            flush=True,
        )

thread.join()

a.close()
b.close()

print("PC helper finished", flush=True)
