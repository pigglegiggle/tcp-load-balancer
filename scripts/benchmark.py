"""Verified loopback direct/proxy comparison, not a WAN capacity claim."""

from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from support import Backend, Balancer, exchange, wait_for


def percentile(values, p):
    return sorted(values)[math.ceil(len(values) * p) - 1]


def request_batch(addresses, clients, requests=100, size=256):
    barrier = threading.Barrier(clients)
    payload = bytes(range(256)) * (size // 256)

    def worker(i):
        samples = []
        with socket.create_connection(addresses[i % len(addresses)], timeout=8) as client:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            barrier.wait(timeout=8)
            for _ in range(requests):
                begin = time.perf_counter_ns()
                client.sendall(payload)
                data = bytearray()
                while len(data) < size:
                    part = client.recv(size - len(data))
                    if not part:
                        raise AssertionError("early EOF")
                    data.extend(part)
                assert data == payload
                samples.append((time.perf_counter_ns() - begin) / 1e6)
            client.shutdown(socket.SHUT_WR)
            assert client.recv(1) == b""
        return samples

    begin = time.perf_counter()
    with ThreadPoolExecutor(max_workers=clients) as pool:
        values = list(pool.map(worker, range(clients)))
    return {
        "wall_seconds": time.perf_counter() - begin,
        "latency_ms": [sample for client in values for sample in client],
    }


def bulk_batch(addresses, clients, size=4 * 1024 * 1024):
    payload = bytes(range(256)) * (size // 256)
    barrier = threading.Barrier(clients)

    def worker(i):
        barrier.wait(timeout=8)
        result = exchange(addresses[i % len(addresses)], payload)
        assert result == payload

    begin = time.perf_counter()
    with ThreadPoolExecutor(max_workers=clients) as pool:
        list(pool.map(worker, range(clients)))
    elapsed = time.perf_counter() - begin
    return {
        "wall_seconds": elapsed,
        "payload_bytes": clients * size,
        "verified_echo_payload_mib_s": clients * size / 1048576 / elapsed,
    }


def main():
    observations = []
    with Backend() as a, Backend() as b, Balancer(ROOT / "build/tcp-lb", [a, b]) as lb:
        wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
        direct = [("127.0.0.1", a.port), ("127.0.0.1", b.port)]
        proxy = [("127.0.0.1", lb.port)]
        request_batch(direct, 1, requests=10)
        request_batch(proxy, 1, requests=10)
        for clients in (1, 8, 32):
            row = {
                "kind": "request",
                "clients": clients,
                "request_bytes": 256,
                "requests_per_client": 100,
                "direct": [],
                "proxy": [],
            }
            for repetition in range(3):
                for method, addresses in (
                    (("direct", direct), ("proxy", proxy))
                    if repetition % 2 == 0
                    else (("proxy", proxy), ("direct", direct))
                ):
                    row[method].append(request_batch(addresses, clients))
            for method in ("direct", "proxy"):
                latencies = [v for batch in row[method] for v in batch["latency_ms"]]
                row[method + "_p50_ms"] = percentile(latencies, 0.5)
                row[method + "_p95_ms"] = percentile(latencies, 0.95)
            observations.append(row)
        for clients in (1, 4, 16):
            row = {
                "kind": "bulk",
                "clients": clients,
                "payload_bytes_per_client": 4 * 1024 * 1024,
                "direct": [],
                "proxy": [],
            }
            for repetition in range(3):
                for method, addresses in (
                    (("direct", direct), ("proxy", proxy))
                    if repetition % 2 == 0
                    else (("proxy", proxy), ("direct", direct))
                ):
                    row[method].append(bulk_batch(addresses, clients))
            for method in ("direct", "proxy"):
                row[method + "_median_mib_s"] = percentile(
                    [b["verified_echo_payload_mib_s"] for b in row[method]], 0.5
                )
            observations.append(row)
        metrics = wait_for(lambda: (m if (m := lb.metrics())["active"] == 0 else None))
        assert metrics["failed"] == 0 and metrics["rejected"] == 0
    result = {
        "recorded_at": datetime.datetime.now().astimezone().isoformat(),
        "platform": platform.platform(),
        "compiler": subprocess.check_output(["clang++", "--version"], text=True).splitlines()[0],
        "flags": "-std=c++17 -O2 -Wall -Wextra -Wpedantic",
        "results_verified": True,
        "workloads": observations,
        "proxy_metrics": metrics,
        "sha256": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [
                ROOT / "build/tcp-lb",
                *sorted((ROOT / "src").glob("*.cpp")),
                *sorted((ROOT / "include/lb").glob("*.hpp")),
                Path(__file__),
                ROOT / "tests/support.py",
            ]
        },
        "methodology": [
            "Real IPv4 TCP over loopback; two Python threaded echo backends and clients share the host with the C++ proxy.",
            "Direct clients alternate between the same two backends; proxy uses least-connections. This measures complete application paths, not isolated proxy CPU cost.",
            "Three measured batches per method/workload; order alternates after a small-request warm-up.",
            "Small-request latency is send-to-complete-echo wall time on persistent connections, including payload comparison and Python scheduling. No pipelining; at most one outstanding request per client.",
            "p50/p95 pool individual request samples across three batches at the stated concurrency; they are not medians of averages.",
            "Bulk throughput counts verified request payload once despite a full echo roundtrip; it is not total wire bandwidth. Batch wall time includes connect, threads, validation, half-close and cleanup.",
            "All response bytes are checked. Proxy failed/rejected counts must be zero. Warm-up traffic is included in final proxy metrics.",
            "No CPU affinity, thermal control, WAN, TLS performance, long-duration soak or per-process CPU/RSS profiling. Backend/client Python overhead can dominate.",
        ],
    }
    output = ROOT / "docs/benchmarks"
    output.mkdir(parents=True, exist_ok=True)
    (output / "latest.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = [
        "# Measured performance report",
        "",
        f"Recorded: {result['recorded_at']}",
        "",
        f"Platform: {result['platform']}. Compiler: {result['compiler']}, `-O2`.",
        "",
        "## Persistent connection requests",
        "",
        "256-byte request + verified echo, 100 requests/client/batch, three batches per method.",
        "",
        "| Clients | Direct p50 / p95 ms | Proxy p50 / p95 ms |",
        "|---:|---:|---:|",
    ]
    for row in observations:
        if row["kind"] == "request":
            lines.append(
                f"| {row['clients']} | {row['direct_p50_ms']:.3f} / {row['direct_p95_ms']:.3f} | {row['proxy_p50_ms']:.3f} / {row['proxy_p95_ms']:.3f} |"
            )
    lines += [
        "",
        "## Bulk verified echo",
        "",
        "4 MiB/client; median throughput of three measured batches. Payload is counted once, not twice for the echo.",
        "",
        "| Clients | Direct payload MiB/s | Proxy payload MiB/s |",
        "|---:|---:|---:|",
    ]
    for row in observations:
        if row["kind"] == "bulk":
            lines.append(
                f"| {row['clients']} | {row['direct_median_mib_s']:.2f} | {row['proxy_median_mib_s']:.2f} |"
            )
    lines += [
        "",
        "## Method and limits",
        "",
        *["- " + item for item in result["methodology"]],
        "",
        f"Observed maximum occupied application buffer per tunnel: {metrics['peak_tunnel_buffered']:,} bytes; configured limit: {2 * metrics['buffer_capacity_per_direction']:,} bytes. This is not process peak RAM; rings reserve capacity and kernel socket buffers add separate memory.",
        "",
        "These numbers describe this loopback setup, not production capacity or a claim to outperform direct TCP. Reproduce with `make benchmark`. [Raw samples, metrics and hashes](benchmarks/latest.json).",
        "",
    ]
    (ROOT / "docs/PERFORMANCE_REPORT.md").write_text("\n".join(lines))
    print(output / "latest.json")
    print(ROOT / "docs/PERFORMANCE_REPORT.md")


if __name__ == "__main__":
    main()
