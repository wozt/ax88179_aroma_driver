#!/usr/bin/env python3
"""Bounded AX88179 wire probe. Requires Linux CAP_NET_RAW (or sudo).

Run before wiiload. Only replies to the adapter's experimental test frames;
records just those frames and their responses, without capturing other traffic.
"""
import argparse
import select
import socket
import struct
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--interface', required=True)
p.add_argument('--mac', default='00:0e:c6:b0:41:dc')
p.add_argument('--seconds', type=int, default=90)
p.add_argument('--pcap', default='/tmp/ax88179-wire.pcap')
a = p.parse_args()
adapter = bytes.fromhex(a.mac.replace(':', ''))
local = bytes.fromhex(Path('/sys/class/net', a.interface, 'address').read_text().strip().replace(':', ''))
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
s.bind((a.interface, 0))
seen = set()
end = time.monotonic() + a.seconds
with open(a.pcap, 'wb') as f:
    f.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    f.flush()
    def record(b):
        now = time.time()
        f.write(struct.pack('<IIII', int(now), int((now % 1) * 1e6), len(b), len(b)) + b)
        f.flush()
    print(f'READY {a.interface} adapter={a.mac} pcap={a.pcap}', flush=True)
    while time.monotonic() < end:
        if not select.select([s], [], [], min(1, max(0, end-time.monotonic())))[0]:
            continue
        b = s.recv(65535)
        if len(b) < 23 or b[6:12] != adapter or b[12:22] != b'\x88\xb5AX179PNG':
            continue
        seq = b[22]
        valid = seq < 3 and len(b) == (60, 1514, 504)[seq]
        valid = valid and all(v == ((i + seq) & 255) for i, v in enumerate(b[23:], 23))
        record(b)
        print(f'WIRE RX seq={seq} len={len(b)} payload={"PASS" if valid else "FAIL"}', flush=True)
        if not valid:
            continue
        reply = adapter + local + b'\x88\xb5AX179PON' + b[22:]
        n = s.send(reply)
        record(reply)
        seen.add(seq)
        print(f'WIRE ECHO sent={n} crc32={__import__("zlib").crc32(reply):08x} verified_sequences={sorted(seen)}', flush=True)
    print(f'WIRE CAPTURE COMPLETE: {len(seen)}/3 sizes verified', flush=True)
s.close()
raise SystemExit(0 if len(seen) == 3 else 1)
