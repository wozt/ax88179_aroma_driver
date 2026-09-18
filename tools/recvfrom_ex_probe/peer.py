#!/usr/bin/env python3

import socket
import sys
import time

TARGET_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.124"

TARGET_PORT = 19030
SOURCE_PORT = 19031
DURATION = 30.0

TTLS = [17, 37, 64, 91, 127, 200]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", SOURCE_PORT))

print("PC recvfrom_ex peer ready", flush=True)
print(f"Target Wii U: {TARGET_IP}:{TARGET_PORT}", flush=True)
print(f"Source port: {SOURCE_PORT}", flush=True)
print(f"TTL cycle: {TTLS}", flush=True)
print("Launch AX recvfrom_ex Probe now.", flush=True)

deadline = time.monotonic() + DURATION
seq = 0

while time.monotonic() < deadline:
    for ttl in TTLS:
        if time.monotonic() >= deadline:
            break

        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, ttl)

        payload = f"TTL={ttl} SEQ={seq}".encode("ascii")

        sock.sendto(payload, (TARGET_IP, TARGET_PORT))

        if seq == 0:
            print(
                f"TX ttl={ttl:3d} -> {TARGET_IP}:{TARGET_PORT} "
                f"{payload!r}",
                flush=True,
            )

        time.sleep(0.08)

    seq += 1

sock.close()
print("PC helper finished", flush=True)
