# Measured performance report

Recorded: 2026-11-01T02:27:37.871354+07:00

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O. Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101), `-O2`.

## Persistent connection requests

256-byte request + verified echo, 100 requests/client/batch, three batches per method.

| Clients | Direct p50 / p95 ms | Proxy p50 / p95 ms |
|---:|---:|---:|
| 1 | 0.027 / 0.062 | 0.057 / 0.113 |
| 8 | 0.355 / 0.654 | 0.354 / 0.651 |
| 32 | 1.482 / 3.082 | 1.648 / 3.197 |

## Bulk verified echo

4 MiB/client; median throughput of three measured batches. Payload is counted once, not twice for the echo.

| Clients | Direct payload MiB/s | Proxy payload MiB/s |
|---:|---:|---:|
| 1 | 1957.07 | 1688.57 |
| 4 | 1162.65 | 1124.51 |
| 16 | 912.32 | 903.74 |

## Method and limits

- Real IPv4 TCP over loopback; two Python threaded echo backends and clients share the host with the C++ proxy.
- Direct clients alternate between the same two backends; proxy uses least-connections. This measures complete application paths, not isolated proxy CPU cost.
- Three measured batches per method/workload; order alternates after a small-request warm-up.
- Small-request latency is send-to-complete-echo wall time on persistent connections, including payload comparison and Python scheduling. No pipelining; at most one outstanding request per client.
- p50/p95 pool individual request samples across three batches at the stated concurrency; they are not medians of averages.
- Bulk throughput counts verified request payload once despite a full echo roundtrip; it is not total wire bandwidth. Batch wall time includes connect, threads, validation, half-close and cleanup.
- All response bytes are checked. Proxy failed/rejected counts must be zero. Warm-up traffic is included in final proxy metrics.
- No CPU affinity, thermal control, WAN, TLS performance, long-duration soak or per-process CPU/RSS profiling. Backend/client Python overhead can dominate.

Observed maximum occupied application buffer per tunnel: 131,072 bytes; configured limit: 131,072 bytes. This is not process peak RAM; rings reserve capacity and kernel socket buffers add separate memory.

These numbers describe this loopback setup, not production capacity or a claim to outperform direct TCP. Reproduce with `make benchmark`. [Raw samples, metrics and hashes](benchmarks/latest.json).
