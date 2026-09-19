#!/usr/bin/env python3
"""TCP/UDP stress echo peer for the AX88179 Wii U shim probe."""

import selectors
import socket
import time

HOST = "192.168.2.100"
PORT = 18879

sel = selectors.DefaultSelector()

tcp_listener = socket.socket(
    socket.AF_INET,
    socket.SOCK_STREAM,
)
tcp_listener.setsockopt(
    socket.SOL_SOCKET,
    socket.SO_REUSEADDR,
    1,
)
tcp_listener.bind((HOST, PORT))
tcp_listener.listen(16)
tcp_listener.setblocking(False)

udp = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM,
)
udp.setsockopt(
    socket.SOL_SOCKET,
    socket.SO_REUSEADDR,
    1,
)
udp.bind((HOST, PORT))
udp.setblocking(False)

sel.register(
    tcp_listener,
    selectors.EVENT_READ,
    "listen",
)
sel.register(
    udp,
    selectors.EVENT_READ,
    "udp",
)

tcp_connections = 0
tcp_packets = 0
tcp_bytes = 0
udp_packets = 0
udp_bytes = 0

seen_udp_peers = set()

print(
    f"AX stress echo listening TCP/UDP {HOST}:{PORT}",
    flush=True,
)

start = time.monotonic()
end = start + 1800
next_stats = start + 1

try:
    while time.monotonic() < end:
        for key, _ in sel.select(timeout=0.1):
            s = key.fileobj

            if key.data == "listen":
                conn, peer = s.accept()
                conn.setblocking(False)

                tcp_connections += 1

                print(
                    "TCP CONNECT",
                    peer,
                    flush=True,
                )

                sel.register(
                    conn,
                    selectors.EVENT_READ,
                    "tcp",
                )

            elif key.data == "udp":
                data, peer = s.recvfrom(4096)

                if peer not in seen_udp_peers:
                    seen_udp_peers.add(peer)
                    print(
                        "UDP PEER",
                        peer,
                        flush=True,
                    )

                if data:
                    s.sendto(data, peer)
                    udp_packets += 1
                    udp_bytes += len(data)

            else:
                try:
                    data = s.recv(8192)

                    if data:
                        s.sendall(data)

                        tcp_packets += 1
                        tcp_bytes += len(data)

                        continue

                except (
                    ConnectionResetError,
                    BrokenPipeError,
                    OSError,
                ) as e:
                    print(
                        "TCP CLOSE",
                        repr(e),
                        flush=True,
                    )

                try:
                    sel.unregister(s)
                except Exception:
                    pass

                s.close()

        now = time.monotonic()

        if now >= next_stats:
            print(
                "STATS"
                f" t={now-start:.1f}s"
                f" tcp_conn={tcp_connections}"
                f" tcp_packets={tcp_packets}"
                f" tcp_bytes={tcp_bytes}"
                f" udp_packets={udp_packets}"
                f" udp_bytes={udp_bytes}",
                flush=True,
            )

            next_stats = now + 1

finally:
    for key in list(
        sel.get_map().values()
    ):
        try:
            key.fileobj.close()
        except Exception:
            pass

    sel.close()
