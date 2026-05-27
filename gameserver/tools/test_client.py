#!/usr/bin/env python3

import socket
import struct


HOST = "127.0.0.1"
PORT = 11000
MESSAGE = b"Hello, server!"


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("connection closed before the full frame was received")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def main() -> None:
    with socket.create_connection((HOST, PORT), timeout=5) as sock:
        sock.sendall(struct.pack(">I", len(MESSAGE)) + MESSAGE)

        header = recv_exact(sock, 4)
        (length,) = struct.unpack(">I", header)
        payload = recv_exact(sock, length)

    print(f"sent: {MESSAGE.decode('utf-8')}")
    print(f"received: {payload.decode('utf-8')}")


if __name__ == "__main__":
    main()
