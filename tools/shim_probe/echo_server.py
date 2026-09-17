#!/usr/bin/env python3
"""Bounded local echo service; log peer IP to distinguish Ethernet from Wi-Fi."""
import selectors
import socket
import time
sel=selectors.DefaultSelector()
for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
    s=socket.socket(socket.AF_INET, kind)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('192.168.2.100', 18879))
    if kind == socket.SOCK_STREAM: s.listen(4)
    sel.register(s, selectors.EVENT_READ, 'listen' if kind == socket.SOCK_STREAM else 'udp')
print('Listening TCP/UDP 192.168.2.100:18879 for 30 minutes', flush=True)
end=time.monotonic()+1800
try:
    while time.monotonic()<end:
        for key,_ in sel.select(timeout=1):
            s=key.fileobj
            if key.data=='listen':
                conn,peer=s.accept(); conn.settimeout(4)
                print('TCP CONNECT',peer,flush=True)
                sel.register(conn,selectors.EVENT_READ,'tcp')
            elif key.data=='udp':
                data,peer=s.recvfrom(2048); s.sendto(data,peer)
                print('UDP ECHO',peer,len(data),flush=True)
            else:
                try:
                    data=s.recv(4096)
                    if data:
                        s.sendall(data); print('TCP ECHO',s.getpeername(),len(data),flush=True); continue
                except OSError as e: print('TCP ERROR',e,flush=True)
                sel.unregister(s); s.close()
finally:
    for key in list(sel.get_map().values()): key.fileobj.close()
    sel.close()
