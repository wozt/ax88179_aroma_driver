#!/usr/bin/env python3
"""
Simple echo server for AX88179 test.
Listens on TCP and UDP port 18879 and echoes back all received data.

Usage: python3 echo_server.py
"""

import socket
import sys
import threading

SERVER_HOST = "0.0.0.0"
SERVER_PORT = 18879

def tcp_server():
    """TCP echo server"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((SERVER_HOST, SERVER_PORT))
    sock.listen(5)
    print(f"[TCP] Listening on {SERVER_HOST}:{SERVER_PORT}")
    
    while True:
        try:
            client, addr = sock.accept()
            print(f"[TCP] Connection from {addr}")
            
            def handle_client(conn):
                try:
                    while True:
                        data = conn.recv(4096)
                        if not data:
                            break
                        conn.sendall(data)
                        print(f"[TCP] Echoed {len(data)} bytes from {addr}")
                except Exception as e:
                    print(f"[TCP] Error with {addr}: {e}")
                finally:
                    conn.close()
                    print(f"[TCP] Closed connection from {addr}")
            
            t = threading.Thread(target=handle_client, args=(client,), daemon=True)
            t.start()
        except Exception as e:
            print(f"[TCP] Accept error: {e}")

def udp_server():
    """UDP echo server"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((SERVER_HOST, SERVER_PORT))
    print(f"[UDP] Listening on {SERVER_HOST}:{SERVER_PORT}")
    
    while True:
        try:
            data, addr = sock.recvfrom(65535)
            sock.sendto(data, addr)
            print(f"[UDP] Echoed {len(data)} bytes from {addr}")
        except Exception as e:
            print(f"[UDP] Error: {e}")

if __name__ == "__main__":
    print(f"AX88179 Echo Server")
    print(f"Listening on {SERVER_HOST}:{SERVER_PORT}")
    print("Press Ctrl+C to stop")
    print()
    
    # Start both TCP and UDP servers
    tcp_thread = threading.Thread(target=tcp_server, daemon=True)
    udp_thread = threading.Thread(target=udp_server, daemon=True)
    
    tcp_thread.start()
    udp_thread.start()
    
    try:
        while True:
            import time
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        sys.exit(0)
