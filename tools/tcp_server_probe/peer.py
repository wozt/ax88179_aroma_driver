#!/usr/bin/env python3

import socket
import sys
import time

TARGET = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.124"
PORT = 19010
BULK_SIZE = 128 * 1024


def connect_retry(timeout=45.0):
    deadline = time.monotonic() + timeout
    last = None

    while time.monotonic() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)

        try:
            s.connect((TARGET, PORT))
            s.settimeout(10.0)
            return s
        except OSError as e:
            last = e
            s.close()
            time.sleep(0.25)

    raise RuntimeError(f"connect timeout to {TARGET}:{PORT}: {last}")


def recv_line(s):
    out = bytearray()

    while b"\n" not in out:
        data = s.recv(4096)

        if not data:
            break

        out += data

    return bytes(out)


def make_bulk():
    return bytes(
        ((i * 37 + 11) & 0xff)
        for i in range(BULK_SIZE)
    )


def fnv1a(data):
    h = 2166136261

    for b in data:
        h ^= b
        h = (h * 16777619) & 0xffffffff

    return h


print(f"TCP server probe peer -> {TARGET}:{PORT}", flush=True)
print("Waiting for Wii U listener...", flush=True)

# ------------------------------------------------------------
# Phase 1: two connections exist simultaneously before replies.
# ------------------------------------------------------------

a = connect_retry()
print("CLIENT1 connected", flush=True)

b = connect_retry()
print("CLIENT2 connected", flush=True)

a.sendall(b"CLIENT1\n")
b.sendall(b"CLIENT2\n")

ra = recv_line(a)
rb = recv_line(b)

print(f"CLIENT1 RX {ra!r}", flush=True)
print(f"CLIENT2 RX {rb!r}", flush=True)

if ra != b"ACK CLIENT1\n":
    raise RuntimeError(f"bad CLIENT1 reply: {ra!r}")

if rb != b"ACK CLIENT2\n":
    raise RuntimeError(f"bad CLIENT2 reply: {rb!r}")

a.close()
b.close()

print("BACKLOG/SHORT CONNECTIONS: PASS", flush=True)

# ------------------------------------------------------------
# Phase 2: bulk transfer + TCP half-close.
# ------------------------------------------------------------

bulk = make_bulk()
expected_hash = fnv1a(bulk)

s = connect_retry()

print(
    f"BULK connected: sending {len(bulk)} bytes "
    f"hash={expected_hash:08x}",
    flush=True,
)

s.sendall(bulk)

# Tell server there will be no more client->server bytes while keeping
# server->client direction alive.
s.shutdown(socket.SHUT_WR)

reply = recv_line(s)

print(f"BULK RX {reply!r}", flush=True)

expected = f"BULK-OK {BULK_SIZE} {expected_hash:08x}\n".encode()

if reply != expected:
    raise RuntimeError(
        f"bad bulk reply: expected {expected!r}, got {reply!r}"
    )

# Server performs SHUT_WR after the reply. We should therefore receive EOF.
tail = s.recv(1)

print(f"after server SHUT_WR recv={tail!r}", flush=True)

if tail != b"":
    raise RuntimeError("expected EOF after server SHUT_WR")

s.close()

print("BULK DATA + HALF-CLOSE: PASS", flush=True)
print("TCP SERVER PEER: PASS", flush=True)
