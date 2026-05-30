#!/usr/bin/env python3
"""Integration test client for the GameServer protocol."""

import os
import socket
import struct
import sys
from pathlib import Path

import capnp

HOST = "127.0.0.1"
PORT = 11000

SCHEMA_PATH = (
    Path(__file__).resolve().parents[2] / "shared" / "protocol" / "schema" / "packet.capnp"
)

_vcpkg_root = Path(os.environ.get("VCPKG_ROOT", "C:/vcpkg"))
_vcpkg_capnp_tools = _vcpkg_root / "installed" / "x64-windows-static" / "tools" / "capnproto"
_vcpkg_capnp_include = _vcpkg_root / "installed" / "x64-windows-static" / "include"
if _vcpkg_capnp_tools.exists():
    os.environ["PATH"] = str(_vcpkg_capnp_tools) + os.pathsep + os.environ.get("PATH", "")

_capnp_pkg_dir = os.path.dirname(os.path.abspath(capnp.__file__))
_capnp_import_root = os.path.dirname(_capnp_pkg_dir)

_capnp_imports = [str(SCHEMA_PATH.parent), _capnp_import_root]
if _vcpkg_capnp_include.exists():
    _capnp_imports.append(str(_vcpkg_capnp_include))

packet_capnp = capnp.load(str(SCHEMA_PATH), imports=_capnp_imports)


def recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("connection closed before full frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class Client:
    """One server connection that stays open across multiple protocol steps."""

    def __init__(self, host=HOST, port=PORT):
        self.sock = socket.create_connection((host, port), timeout=5)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def send_packet(self, packet_msg):
        payload = b"\x00" + packet_msg.to_bytes()
        self.sock.sendall(struct.pack(">I", len(payload)) + payload)

    def recv_packet(self):
        header = recv_exact(self.sock, 4)
        (length,) = struct.unpack(">I", header)
        payload = recv_exact(self.sock, length)
        if not payload or payload[0] != 0:
            raise ValueError(f"unexpected codec: {payload[0] if payload else 'empty'}")
        return packet_capnp.Packet.from_bytes(payload[1:])

    def handshake(self, version=1):
        request = packet_capnp.Packet.new_message()
        handshake = request.init("handshakeRequest")
        handshake.protocolVersion = version
        handshake.clientBuild = "test-client-0.2"
        self.send_packet(request)
        with self.recv_packet() as response:
            assert response.which() == "handshakeResponse"
            return str(response.handshakeResponse.result), response.handshakeResponse.message

    def login(self, username, password):
        request = packet_capnp.Packet.new_message()
        login = request.init("loginRequest")
        login.username = username
        login.password = password
        self.send_packet(request)
        with self.recv_packet() as response:
            assert response.which() == "loginResponse"
            return (
                str(response.loginResponse.result),
                response.loginResponse.message,
                response.loginResponse.accountId,
            )

    def list_characters(self):
        request = packet_capnp.Packet.new_message()
        request.init("characterListRequest")
        self.send_packet(request)
        with self.recv_packet() as response:
            assert response.which() == "characterListResponse"
            characters = []
            for character in response.characterListResponse.characters:
                characters.append(
                    {
                        "id": character.id,
                        "slot": character.slot,
                        "name": character.name,
                        "level": character.level,
                        "classId": character.classId,
                    }
                )
            return (
                str(response.characterListResponse.result),
                response.characterListResponse.message,
                characters,
            )


def test_handshake_ok():
    with Client() as client:
        result, message = client.handshake(1)
        print(f"valid-handshake: result={result}, message={message}")
        assert result == "ok", f"expected ok, got {result}"
    print("PASS valid-handshake")


def test_handshake_bad_version():
    with Client() as client:
        result, message = client.handshake(999)
        print(f"bad-version: result={result}, message={message}")
        assert result == "protocolVersionMismatch", (
            f"expected protocolVersionMismatch, got {result}"
        )
    print("PASS bad-version")


def test_full_login_flow():
    with Client() as client:
        handshake_result, _ = client.handshake(1)
        assert handshake_result == "ok", f"handshake failed: {handshake_result}"

        login_result, _, account_id = client.login("testuser", "admin")
        assert login_result == "ok", f"login failed: {login_result}"
        assert account_id == 1, f"unexpected account_id: {account_id}"

        list_result, _, characters = client.list_characters()
        assert list_result == "ok", f"character list failed: {list_result}"
        assert len(characters) == 2, f"expected 2 characters, got {len(characters)}"
        print(f"  characters: {[character['name'] for character in characters]}")
    print("PASS full-login-flow")


def test_bad_password():
    with Client() as client:
        handshake_result, _ = client.handshake(1)
        assert handshake_result == "ok"

        login_result, _, _ = client.login("testuser", "wrongpass")
        assert login_result == "invalidCredentials", (
            f"expected invalidCredentials, got {login_result}"
        )
    print("PASS bad-password")


def test_character_list_without_login():
    with Client() as client:
        handshake_result, _ = client.handshake(1)
        assert handshake_result == "ok"

        try:
            client.list_characters()
            raise AssertionError("server did not disconnect")
        except (ConnectionError, ConnectionResetError, OSError):
            pass
    print("PASS char-list-without-login")


def run(name, fn):
    try:
        fn()
        return True
    except Exception as exc:
        print(f"FAIL {name}: {type(exc).__name__}: {exc}")
        return False


def main():
    ok = True
    ok &= run("valid-handshake", test_handshake_ok)
    ok &= run("bad-version", test_handshake_bad_version)
    ok &= run("full-login-flow", test_full_login_flow)
    ok &= run("bad-password", test_bad_password)
    ok &= run("char-list-without-login", test_character_list_without_login)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
