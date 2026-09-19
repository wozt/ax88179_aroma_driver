#!/usr/bin/env python3

import socket
import time

HOST = "192.168.2.100"
CTRL_PORT = 19049
DATA_BASE = 19050

SOCKETS = 8
ROUNDS = 3
PER_SOCKET = 64
PAYLOAD = 1400

sock = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM
)

sock.bind((HOST, CTRL_PORT))

print(
    f"buffer exhaustion peer listening on "
    f"{HOST}:{CTRL_PORT}",
    flush=True
)

for expected_round in range(1, ROUNDS + 1):
    while True:
        data, addr = sock.recvfrom(2048)
        text = data.decode(errors="replace").strip()

        if text.startswith("READY"):
            break

    target_ip = addr[0]

    print(
        f"[round {expected_round}] READY from "
        f"{target_ip} control={addr[1]}",
        flush=True
    )

    packets = 0
    total_bytes = 0

    # Round-robin rather than filling socket 0 first.
    for seq in range(PER_SOCKET):
        for index in range(SOCKETS):
            header = (
                f"B{expected_round:02d}"
                f"{index:02d}"
                f"{seq:04d}"
            ).encode()

            payload = (
                header +
                bytes([index & 0xff]) *
                (PAYLOAD - len(header))
            )

            sock.sendto(
                payload,
                (target_ip, DATA_BASE + index)
            )

            packets += 1
            total_bytes += len(payload)

        # Keep this fast enough to exhaust queues, while avoiding
        # a pure NIC-ring microburst being the thing we measure.
        time.sleep(0.0005)

    print(
        f"[round {expected_round}] burst sent "
        f"packets={packets} bytes={total_bytes}",
        flush=True
    )

    while True:
        data, addr2 = sock.recvfrom(2048)
        text = data.decode(errors="replace").strip()

        if text == f"DRAINED round={expected_round}":
            break

    print(
        f"[round {expected_round}] Wii U drained; "
        f"sending recovery markers",
        flush=True
    )

    for copy in range(3):
        for index in range(SOCKETS):
            payload = (
                f"RECOVERY "
                f"round={expected_round} "
                f"sock={index} copy={copy}"
            ).encode()

            sock.sendto(
                payload,
                (target_ip, DATA_BASE + index)
            )

        time.sleep(0.01)

print("buffer exhaustion peer done", flush=True)
