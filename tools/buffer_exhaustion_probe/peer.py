#!/usr/bin/env python3

import argparse
import socket
import time

HOST = "192.168.2.100"
CTRL_PORT = 19049
DATA_BASE = 19050

SOCKETS = 8
ROUNDS = 3
PER_SOCKET = 64
PAYLOAD = 1400

parser = argparse.ArgumentParser()
parser.add_argument(
    "--gaps-us",
    default="500,2000,5000",
    help="comma-separated delay after each round-robin group of 8 packets"
)
args = parser.parse_args()

gaps_us = [int(x) for x in args.gaps_us.split(",")]

if len(gaps_us) != ROUNDS:
    raise SystemExit(
        f"--gaps-us must contain exactly {ROUNDS} values"
    )

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

print(
    "pacing matrix: " +
    ", ".join(f"{x}us" for x in gaps_us),
    flush=True
)

for expected_round in range(1, ROUNDS + 1):
    gap_us = gaps_us[expected_round - 1]

    # Eight packets of 1400 bytes are emitted per pacing interval.
    nominal_mbps = (
        SOCKETS * PAYLOAD * 8
        / (gap_us / 1_000_000)
        / 1_000_000
    )

    while True:
        data, addr = sock.recvfrom(2048)
        text = data.decode(errors="replace").strip()

        if text.startswith("READY"):
            break

    target_ip = addr[0]

    print(
        f"[round {expected_round}] READY from "
        f"{target_ip} control={addr[1]} "
        f"gap={gap_us}us nominal={nominal_mbps:.1f}Mbit/s",
        flush=True
    )

    packets = 0
    total_bytes = 0
    started = time.monotonic()

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

        time.sleep(gap_us / 1_000_000)

    elapsed = time.monotonic() - started

    actual_mbps = (
        total_bytes * 8 / elapsed / 1_000_000
        if elapsed > 0 else 0
    )

    print(
        f"[round {expected_round}] burst sent "
        f"packets={packets} bytes={total_bytes} "
        f"time={elapsed:.3f}s "
        f"actual={actual_mbps:.1f}Mbit/s",
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
