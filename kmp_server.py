#!/usr/bin/env python3
"""KMP Chat Server — run alongside QEMU on host"""
import socket
import struct
import threading

KMP_MAGIC = 0x4B4D5000
KMP_LOGIN = 1
KMP_TEXT = 2
KMP_HEARTBEAT = 3
KMP_ACK = 4

clients = {}

def handle_client(conn, addr):
    print(f"[KMP] Client connected: {addr}")
    buf = b""
    while True:
        try:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while len(buf) >= 12:
                magic = struct.unpack(">I", buf[0:4])[0]
                if magic != KMP_MAGIC:
                    buf = buf[1:]
                    continue
                ptype = buf[4]
                sender = buf[5]
                plen = struct.unpack(">H", buf[6:8])[0]
                dest = struct.unpack(">I", buf[8:12])[0]
                if len(buf) < 12 + plen:
                    break
                payload = buf[12:12+plen]
                buf = buf[12+plen:]

                if ptype == KMP_LOGIN:
                    name = payload.decode("utf-8", errors="replace")
                    print(f"[KMP] Login: {name} (id={sender})")
                    clients[sender] = conn
                    # Send ACK
                    ack = struct.pack(">IBBH I", KMP_MAGIC, KMP_ACK, 0, 0, sender)
                    conn.send(ack)

                elif ptype == KMP_TEXT:
                    text = payload.decode("utf-8", errors="replace")
                    print(f"[KMP] Text from {sender}: {text}")
                    # Echo back as from server
                    resp = struct.pack(">IBBH I", KMP_MAGIC, KMP_TEXT, 0, len(text), sender) + text.encode()
                    conn.send(resp)
                    # Also send a reply
                    reply_text = "Echo: " + text
                    reply = struct.pack(">IBBH I", KMP_MAGIC, KMP_TEXT, 0, len(reply_text), sender) + reply_text.encode()
                    conn.send(reply)

                elif ptype == KMP_HEARTBEAT:
                    ack = struct.pack(">IBBH I", KMP_MAGIC, KMP_ACK, 0, 0, sender)
                    conn.send(ack)

        except Exception as e:
            print(f"[KMP] Error: {e}")
            break
    print(f"[KMP] Client disconnected: {addr}")
    conn.close()

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9999))
    s.listen(5)
    print("[KMP] Server listening on port 9999")
    while True:
        conn, addr = s.accept()
        t = threading.Thread(target=handle_client, args=(conn, addr))
        t.daemon = True
        t.start()

if __name__ == "__main__":
    main()
