"""Phase 3 integration checks. Run with: make test-poll"""

import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

from test_blocking import receive_exact


SERVER = str(Path(__file__).resolve().with_name("poll_server"))


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

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [SERVER, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    def connect():
        return socket.create_connection(("127.0.0.1", port), timeout=10)

    def echo(payload):
        with connect() as client:
            client.sendall(payload)
            client.shutdown(socket.SHUT_WR)
            assert receive_exact(client, len(payload)) == payload
            assert client.recv(1) == b"", "expected EOF after echo"

    def fd_count():
        return len(list(Path(f"/proc/{process.pid}/fd").iterdir()))

    def wait_for_fds(expected):
        deadline = time.monotonic() + 3
        while fd_count() != expected:
            assert time.monotonic() < deadline, "descriptor count did not settle"
            time.sleep(0.01)

    try:
        deadline = time.monotonic() + 3
        while True:
            assert process.poll() is None
            try:
                with connect():
                    pass
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)

        baseline_fds = 4  # stdin, stdout, stderr, listener
        wait_for_fds(baseline_fds)
        echo(b"hello\x00binary\xff\n")

        with connect() as idle, connect() as slow:
            idle.sendall(b"idle")
            assert receive_exact(idle, 4) == b"idle"
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32 * 1024)
            payload = bytes(range(256)) * (8 * 4096)  # 8 MiB
            errors = []
            done = threading.Event()

            def send_large():
                try:
                    slow.sendall(payload)
                    slow.shutdown(socket.SHUT_WR)
                except OSError as error:
                    errors.append(error)
                finally:
                    done.set()

            sender = threading.Thread(target=send_large, daemon=True)
            sender.start()
            assert not done.wait(0.2), "large sender did not encounter backpressure"
            for _ in range(10):
                echo(b"active while another client does not read\x00")
            assert receive_exact(slow, len(payload)) == payload
            assert slow.recv(1) == b"", "half-close lost pending echo"
            sender.join(timeout=3)
            assert not sender.is_alive() and not errors, errors

            # Idle sockets should let poll sleep, rather than spin on POLLOUT.
            stat = Path(f"/proc/{process.pid}/stat")
            before = sum(map(int, stat.read_text().split()[13:15]))
            time.sleep(0.3)
            after = sum(map(int, stat.read_text().split()[13:15]))
            assert after - before <= 5, "server consumed CPU while clients were idle"

        for _ in range(100):
            echo(b"connect/disconnect")
        wait_for_fds(baseline_fds)
        with connect() as client:
            client.sendall(b"reset" * 16384)
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        echo(b"alive after reset")
        wait_for_fds(baseline_fds)

        clients = []
        try:
            for _ in range(128):
                client = connect()
                clients.append(client)
                client.sendall(b"x")
                assert receive_exact(client, 1) == b"x"
            with connect() as excess:
                try:
                    assert excess.recv(1) == b"", "excess client was not rejected"
                except ConnectionResetError:
                    pass
        finally:
            for client in clients:
                client.close()
        wait_for_fds(baseline_fds)
        echo(b"capacity recovered")
        print("PASS: startup, binary/8 MiB echo, idle + slow + active clients,")
        print("      half-close, idle CPU, fd cleanup, reset recovery, 128-client limit")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
