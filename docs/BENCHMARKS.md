# Benchmark methodology

Run `make benchmark` after building. `scripts/benchmark.py` uses the same compiled executable and real TCP fixtures as the integration suite. It writes `docs/benchmarks/latest.json` with raw samples, final proxy metrics, source/binary hashes, compiler flags and environment; the generated Markdown report is `docs/PERFORMANCE_REPORT.md`.

## Direct versus proxy paths

Two local Python threaded echo backends stay available for both methods. Direct clients alternate destinations; proxy clients all connect to the C++ listener using least connections. Health probes run while both methods are measured. A direct result is a baseline application path, not an alternative load balancer implementation.

Small requests: 256 bytes per echo, 100 sequential requests per persistent client, at concurrency 1, 8 and 32. There is one outstanding request per client and no application pipelining. A barrier starts each client batch together. Client TCP_NODELAY is enabled. Timing starts before send and ends after the complete echo has been read and compared. Client socket creation and thread setup are outside individual request latency, but backend connect readiness may affect initial proxy requests.

Three batches per method/workload alternate direct/proxy order after a small-request warm-up. p50/p95 use nearest-rank percentiles pooled from individual request samples across the three batches, not medians of batch averages. Scheduling in the Python driver/backend threads is included.

Bulk requests: each client sends 4 MiB and receives an exact verified echo; concurrency is 1, 4 and 16. Send and receive run concurrently to avoid a test-driver full-duplex deadlock. Batch time includes thread creation, connect, copying/comparison, half-close and cleanup. Reported throughput is **request payload bytes counted once / batch wall time**, despite the full echo roundtrip. It is not total network wire bandwidth. The report uses the median throughput of three batches.

## Correctness and interpretation

Every returned response is compared byte-for-byte. The benchmark requires proxy failed/rejected counts to remain zero. Final proxy metrics include warm-up traffic. Unit/integration tests establish other correctness properties; throughput measurements alone do not prove them.

All clients, backend services and the proxy share one host's CPU and loopback network. Backend/client Python overhead, allocation and scheduling may dominate, especially at higher concurrency. The proxy adds a real forwarding hop; lower direct latency is expected and is not hidden. This is not a WAN, TLS, line-rate capacity, adversarial or long-duration benchmark, and does not compare against HAProxy/Nginx/Envoy.

There is no CPU affinity, thermal isolation or process-specific CPU/RSS profiler. Occupied application-ring metrics are not memory RSS. The fixed configured capacity is a structural bound, while kernel buffers and metadata add separate memory. Read the raw method and workload before quoting any latency or throughput number.
