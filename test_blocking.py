"""Phase 2 integration checks. Run with: make test"""

import socket
import struct
import subprocess
import threading
import time
from pathlib import Path


SERVER = str(Path(__file__).resolve().with_name("blocking_server"))


def receive_exact(client, count):
    result = bytearray()
    while len(result) < count:
        chunk = client.recv(count - len(result))
        assert chunk, "premature EOF"
        result.extend(chunk)
    return bytes(result)


def main():
    for port in ("0", "65536", "-1", "abc", "9090x", ""):
        result = subprocess.run([SERVER, port], capture_output=True, timeout=3)
        assert result.returncode != 0 and b"Invalid port" in result.stderr

    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        port = occupied.getsockname()[1]
        result = subprocess.run([SERVER, str(port)], capture_output=True, timeout=3)
        assert result.returncode != 0 and b"bind" in result.stderr

    # Ask the OS for an unused port, then hand it to the server.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [SERVER, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    def connect():
        return socket.create_connection(("127.0.0.1", port), timeout=3)

    def echo(payload):
        with connect() as client:
            # Concurrent sending avoids a client-side deadlock on large echoes.
            errors = []

            def send():
                try:
                    client.sendall(payload)
                    client.shutdown(socket.SHUT_WR)
                except OSError as error:
                    errors.append(error)

            sender = threading.Thread(target=send)
            sender.start()
            try:
                assert receive_exact(client, len(payload)) == payload
                assert client.recv(1) == b"", "expected EOF after half-close"
            finally:
                sender.join(timeout=4)
            assert not sender.is_alive(), "sender stalled"
            assert not errors, errors

    try:
        deadline = time.monotonic() + 3
        while True:
            assert process.poll() is None, "server exited during startup"
            try:
                with connect():
                    pass  # Also checks an empty connection.
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)

        echo(b"hello\n")
        echo(b"\x00binary\xff\x00")
        echo(bytes(range(256)) * 4096)  # 1 MiB; spans many 4 KiB receives.

        with connect() as first:
            first.sendall(b"first")
            assert receive_exact(first, 5) == b"first"  # First client is accepted.
            with connect() as second:
                second.sendall(b"second")
                second.settimeout(0.2)
                try:
                    second.recv(6)
                except socket.timeout:
                    pass
                else:
                    raise AssertionError("second client was serviced before first EOF")
                first.shutdown(socket.SHUT_WR)
                assert first.recv(1) == b""
                second.settimeout(3)
                assert receive_exact(second, 6) == b"second"

        with connect() as client:
            client.sendall(b"reset" * 16384)
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        echo(b"still alive after reset")
        assert process.poll() is None
        print("PASS: startup errors, binary/large echo, half-close, empty connection,")
        print("      second-client delay, reset recovery")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
