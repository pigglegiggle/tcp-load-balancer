"""Black-box tests against real proxy/backend/client processes and sockets."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

from support import Backend, Balancer, exchange, free_port, wait_for

BINARY = Path(sys.argv[1]).resolve()
checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)


def settled(lb):
    metrics = wait_for(lambda: (m if (m := lb.metrics())["active"] == 0 else None))
    check(
        metrics["accepted"] == metrics["completed"] + metrics["failed"] + metrics["rejected"],
        "connection conservation",
    )
    check(all(b["active"] == 0 for b in metrics["backends"]), "backend leases released")
    check(
        metrics["peak_tunnel_buffered"] <= 2 * metrics["buffer_capacity_per_direction"],
        "bounded tunnel buffers",
    )
    return metrics


def test_binary_and_fragmentation():
    payload = bytes(range(256)) * 8192
    with Backend() as backend, Balancer(BINARY, [backend], "--buffer-bytes", "4096") as lb:
        response = exchange(("127.0.0.1", lb.port), payload, 1703)
        check(
            hashlib.sha256(response).digest() == hashlib.sha256(payload).digest(),
            "binary 2 MiB roundtrip",
        )
        check(
            exchange(("127.0.0.1", lb.port), b"fragment" * 2000, 3) == b"fragment" * 2000,
            "fragmented writes",
        )
        check(exchange(("127.0.0.1", lb.port), b"") == b"", "empty stream")
        metrics = settled(lb)
        check(
            metrics["client_bytes_read"] == metrics["backend_bytes_written"],
            "client-to-backend byte accounting",
        )
        check(
            metrics["backend_bytes_read"] == metrics["client_bytes_written"],
            "backend-to-client byte accounting",
        )
    check(lb.process.returncode == 0 and lb.final is not None, "clean process exit")


def test_half_close():
    payload = os.urandom(1024 * 1024)
    with (
        Backend(mode="after-eof") as backend,
        Balancer(BINARY, [backend], "--buffer-bytes", "2048") as lb,
    ):
        check(
            exchange(("127.0.0.1", lb.port), payload) == payload[::-1],
            "response after client SHUT_WR",
        )
        settled(lb)
    with Backend(mode="greeting") as backend, Balancer(BINARY, [backend]) as lb:
        with lb.connect() as client:
            check(client.recv(5) == b"HELLO", "backend speaks first")
            check(client.recv(1) == b"", "backend half-close reaches client")
            client.sendall(b"client can still send after receiving EOF")
            client.shutdown(socket.SHUT_WR)
        metrics = settled(lb)
        check(metrics["backend_bytes_written"] > 0, "reverse direction survives backend half-close")


def test_distribution():
    backends = [Backend(str(i), "identity") for i in range(3)]
    try:
        with Balancer(BINARY, backends, "--policy", "round-robin") as lb:
            wait_for(lambda: lb.metrics()["healthy_backends"] == 3)
            counts = {str(i): 0 for i in range(3)}
            for _ in range(30):
                response = exchange(("127.0.0.1", lb.port), b"request")
                name, data = response.split(b":", 1)
                counts[name.decode()] += 1
                check(data == b"request", "identity payload")
            check(list(counts.values()) == [10, 10, 10], "round robin distribution")
            settled(lb)
        with Balancer(BINARY, backends[:2], "--policy", "least-connections") as lb:
            wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
            with lb.connect() as held:
                held.sendall(b"held")
                identity = held.recv(64).split(b":", 1)[0]
                wait_for(lambda: lb.metrics()["active"] == 1)
                for _ in range(6):
                    response = exchange(("127.0.0.1", lb.port), b"short")
                    check(
                        not response.startswith(identity + b":"),
                        "least-connections avoids held backend",
                    )
                    wait_for(lambda: lb.metrics()["active"] == 1)
            settled(lb)
    finally:
        for backend in backends:
            backend.close()


def test_concurrent_clients():
    with Backend() as a, Backend() as b, Balancer(BINARY, [a, b], "--buffer-bytes", "8192") as lb:
        payloads = [bytes([i]) * (16384 + i * 31) for i in range(24)]
        with ThreadPoolExecutor(max_workers=12) as pool:
            futures = [pool.submit(exchange, ("127.0.0.1", lb.port), p, 511) for p in payloads]
            for payload, future in zip(payloads, futures):
                check(future.result(timeout=10) == payload, "concurrent stream isolation")
        check(settled(lb)["completed"] == 24, "concurrent completion count")


def test_health_and_retry():
    a, b = Backend("A", "identity"), Backend("B", "identity")
    try:
        with Balancer(
            BINARY, [a, b], "--policy", "round-robin", "--rise", "2", "--fall", "2"
        ) as lb:
            wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
            port = b.port
            b.close()
            wait_for(lambda: lb.metrics()["healthy_backends"] == 1)
            for _ in range(5):
                check(
                    exchange(("127.0.0.1", lb.port), b"alive").startswith(b"A:"),
                    "dead backend excluded",
                )
            b = Backend("B", "identity", port=port)
            wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
            results = [exchange(("127.0.0.1", lb.port), b"recovered") for _ in range(6)]
            check(any(r.startswith(b"B:") for r in results), "recovered backend reenters pool")
            settled(lb)
        with Balancer(
            BINARY, [a, b], "--policy", "round-robin", "--health-interval-ms", "60000"
        ) as lb:
            wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
            a.close()
            check(
                exchange(("127.0.0.1", lb.port), b"not replayed").startswith(b"B:"),
                "retry failed connect on live backend",
            )
            metrics = settled(lb)
            check(
                metrics["connect_failures"] >= 1 and metrics["connect_retries"] >= 1,
                "pre-connect retry recorded",
            )
    finally:
        a.close()
        b.close()


def test_limits_and_idle():
    with (
        Backend() as backend,
        Balancer(BINARY, [backend], "--max-connections", "1", "--idle-timeout-ms", "200") as lb,
    ):
        with lb.connect() as held:
            held.sendall(b"hold")
            check(held.recv(4) == b"hold", "held client connected")
            with lb.connect() as rejected:
                check(rejected.recv(1) == b"", "connection cap rejects excess client")
            wait_for(lambda: lb.metrics()["idle_timeouts"] == 1)
            check(held.recv(1) == b"", "idle connection closed")
        metrics = settled(lb)
        check(metrics["rejected_limit"] == 1, "limit rejection counter")
    with Balancer(BINARY, [f"127.0.0.1:{free_port()}"], ready=False) as lb:
        check(lb.request("/ready")[0] == 503, "no backend is not ready")
        with lb.connect() as client:
            check(client.recv(1) == b"", "no healthy backend closes promptly")
        check(settled(lb)["rejected_no_backend"] == 1, "no-backend rejection counter")


def test_no_replay_after_delivery():
    with (
        Backend(mode="reset") as failed,
        Backend() as other,
        Balancer(BINARY, [failed, other], "--policy", "round-robin") as lb,
    ):
        wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
        with lb.connect() as client:
            client.sendall(b"one application operation")
            try:
                check(client.recv(64) == b"", "reset stream terminates")
            except ConnectionResetError:
                check(True, "reset stream terminates")
        wait_for(lambda: failed.messages == 1)
        metrics = settled(lb)
        check(other.messages == 0, "application bytes are not replayed to another backend")
        check(metrics["connect_retries"] == 0, "no retry after successful connect")


def test_backpressure_and_reset():
    with (
        Backend(delay=0.002) as backend,
        Balancer(BINARY, [backend], "--buffer-bytes", "1024") as lb,
    ):
        payload = os.urandom(3 * 1024 * 1024)
        check(
            exchange(("127.0.0.1", lb.port), payload) == payload, "slow backend retains every byte"
        )
        metrics = settled(lb)
        check(metrics["backpressure_pauses"] > 0, "full buffers pause reading")
        for _ in range(12):
            try:
                with lb.connect() as client:
                    client.sendall(b"abort" * 100)
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            except OSError:
                pass  # An intentionally reset peer may fail before its send completes.
        wait_for(lambda: lb.metrics()["active"] == 0)
        check(
            exchange(("127.0.0.1", lb.port), b"still alive") == b"still alive",
            "resets do not kill reactor",
        )
        settled(lb)


def test_graceful_shutdown():
    with Backend() as backend, Balancer(BINARY, [backend], "--drain-timeout-ms", "1500") as lb:
        with lb.connect() as client:
            client.sendall(b"before")
            check(client.recv(6) == b"before", "before drain")
            lb.process.terminate()
            wait_for(lambda: lb.metrics()["draining"])
            check(lb.request("/ready")[0] == 503, "draining is not ready")
            try:
                unexpected = socket.create_connection(("127.0.0.1", lb.port), timeout=0.3)
                unexpected.close()
                check(False, "draining stopped listener")
            except OSError:
                check(True, "draining stopped listener")
            client.sendall(b"after")
            check(client.recv(5) == b"after", "existing stream continues during drain")
            client.shutdown(socket.SHUT_WR)
            check(client.recv(1) == b"", "drain preserves half-close")
        lb.process.wait(timeout=3)
    check(
        lb.final["forced_shutdowns"] == 0 and lb.final["completed"] == 1, "graceful drain counters"
    )
    with Backend() as backend, Balancer(BINARY, [backend], "--drain-timeout-ms", "80") as lb:
        with lb.connect() as client:
            client.sendall(b"held")
            check(client.recv(4) == b"held", "forced drain held client")
            lb.process.terminate()
            lb.process.wait(timeout=2)
    check(
        lb.final["forced_shutdowns"] == 1 and lb.final["active"] == 0, "bounded shutdown deadline"
    )


def test_quiet_connection_polling():
    with (
        Backend() as backend,
        Balancer(
            BINARY, [backend], "--health-interval-ms", "60000", "--idle-timeout-ms", "2000"
        ) as lb,
    ):
        with lb.connect() as client:
            client.sendall(b"quiet")
            check(client.recv(5) == b"quiet", "quiet tunnel established")
            before = lb.metrics()["poll_calls"]
            time.sleep(0.3)
            after = lb.metrics()["poll_calls"]
            check(after - before < 10, "quiet connection avoids fixed 20 ms wakeups")
            client.shutdown(socket.SHUT_WR)
            check(client.recv(1) == b"", "quiet connection still completes")
        settled(lb)


def test_admin_descriptor_exhaustion():
    with Backend() as backend, Balancer(BINARY, [backend], fd_limit=32) as lb:
        observers = []
        try:
            observer = socket.create_connection(("127.0.0.1", lb.admin_port), timeout=2)
            observers.append(observer)
            observer.sendall(b"GET /metrics HTTP/1.1\r\n")
            time.sleep(
                0.03
            )  # Admit this observer before consuming the remaining child descriptors.
            for _ in range(40):
                observers.append(socket.create_connection(("127.0.0.1", lb.admin_port), timeout=2))
            time.sleep(0.15)
            observer.sendall(b"Host: localhost\r\n\r\n")
            response = http.client.HTTPResponse(observer)
            response.begin()
            metrics = json.loads(response.read())
            check(metrics["poll_calls"] < 200, "admin descriptor exhaustion does not busy-spin")
            check(metrics["admin_accept_errors"] > 0, "admin accept exhaustion is observable")
        finally:
            for observer in observers:
                observer.close()
        check(
            wait_for(
                lambda: exchange(("127.0.0.1", lb.port), b"after fd pressure")
                == b"after fd pressure"
            ),
            "data plane recovers after descriptor pressure",
        )
        settled(lb)


def test_admin_and_config():
    with Backend() as backend, Balancer(BINARY, [backend]) as lb:
        check(lb.request("/unknown")[0] == 404, "unknown admin route")
        check(lb.request("/metrics", "POST")[0] == 405, "admin GET only")
        with socket.create_connection(("127.0.0.1", lb.admin_port), timeout=2) as client:
            client.sendall(b"GET /metrics HTTP/1.1\r\nX: " + b"x" * 8300)
            check(b"431" in client.recv(256), "bounded admin request")
        with socket.create_connection(("127.0.0.1", lb.admin_port), timeout=2) as client:
            client.sendall(b"bad\r\n\r\n")
            check(b"400" in client.recv(256), "malformed request")
    for options in (
        ["--backend", "localhost:80"],
        ["--backend", "127.0.0.1:0"],
        ["--backend", "127.0.0.1:1", "--admin", "0.0.0.0:0"],
        ["--backend", "127.0.0.1:1", "--buffer-bytes", "-1"],
        ["--unknown", "value"],
    ):
        result = subprocess.run([str(BINARY), *options], capture_output=True, timeout=2)
        check(result.returncode == 1 and b"error:" in result.stderr, "invalid CLI rejected")


if __name__ == "__main__":
    for test in [
        test_binary_and_fragmentation,
        test_half_close,
        test_distribution,
        test_concurrent_clients,
        test_health_and_retry,
        test_limits_and_idle,
        test_no_replay_after_delivery,
        test_backpressure_and_reset,
        test_graceful_shutdown,
        test_quiet_connection_polling,
        test_admin_descriptor_exhaustion,
        test_admin_and_config,
    ]:
        started = time.monotonic()
        test()
        print(f"PASS {test.__name__} ({time.monotonic() - started:.2f}s)", flush=True)
    print(f"{checks} integration checks passed", flush=True)
