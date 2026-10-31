# How to run

## Setup

```sh
git clone https://github.com/pigglegiggle/tcp-load-balancer.git
cd tcp-load-balancer
make
make test
make demo
```

For another checkout location, enter that directory instead. The C++ executable has no third-party runtime dependency. Tools require Python 3.10+ with its standard library only. macOS needs command-line developer tools (`xcode-select --install`); Ubuntu needs `build-essential clang python3`. Build/test on Linux/WSL has not been executed on this development Mac.

`make` uses clang++ by default. `make CXX=g++` selects another C++17 compiler. For additional checking, `make sanitize` builds separate ASan/UBSan executables and runs both DSA and socket integration tests.

## Automated / interactive demo

`make demo` starts two local HTTP servers and a proxy with automatically selected free ports. It verifies six A/B requests, removes A, verifies four B requests, restarts A and verifies both return. It stops its own services on exit.

```sh
python3 scripts/demo.py --serve
```

The persistent demo uses proxy port **8600** and admin port **8601**:

```sh
curl http://127.0.0.1:8600/hello
curl http://127.0.0.1:8600/hello
curl -s http://127.0.0.1:8601/metrics | python3 -m json.tool
```

Each response identifies the real backend that answered. In this demo both servers are local threads and the proxy is its own C++ process; performance across actual machines must be measured separately. Ctrl+C stops all owned demo services. The demo is deliberately plain HTTP/JSON because the project focuses on the forwarding backend.

## CLI configuration

```sh
./build/tcp-lb \
  --listen 127.0.0.1:8080 \
  --admin 127.0.0.1:9091 \
  --backend 127.0.0.1:9001 \
  --backend 127.0.0.1:9002 \
  --policy least-connections \
  --max-connections 512 \
  --buffer-bytes 65536 \
  --connect-timeout-ms 1000 \
  --idle-timeout-ms 30000 \
  --drain-timeout-ms 5000 \
  --health-interval-ms 1000 \
  --health-timeout-ms 500 \
  --rise 2 --fall 2
```

Repeat `--backend` for each unique numeric IPv4 endpoint, up to 64. Listener/admin port 0 chooses an OS-assigned free port; the startup JSON prints the actual ports and PID. Backend port 0 and the unspecified backend address `0.0.0.0` are invalid. Backends that point at this process's public or admin listener are rejected, including loopback/interface addresses covered by a wildcard public bind. Validation runs again after port 0 listeners receive their actual OS-assigned ports. An unrelated backend may use the same port on a different address. Scalar options repeated on the CLI use their last value.

Policies: `least-connections` (default), `round-robin`. Lower active traffic-connection counts win; ties rotate by least-recent selection. Health probes are excluded from active load. Accepted streams are pinned to their assigned backend after connect success.

| Option | Allowed bounds / meaning |
|---|---|
| `--max-connections` | 1..16,384 active tunnels |
| `--buffer-bytes` | 1,024..4,194,304 bytes per direction |
| All five `*-ms` options | 10..3,600,000 ms |
| `--rise`, `--fall` | 1..10 consecutive active probe outcomes |
| Potential ring capacity | `2 × max-connections × buffer-bytes` must be ≤ 1 GiB |

Timeout checks have roughly 20 ms poll granularity plus loop work. Idle time advances only on successful data reads/writes, not on health checks. The drain deadline starts after SIGINT/SIGTERM is observed. A passive client-connect failure excludes a backend immediately even if `fall` is greater than one.

For a larger connection count, set a suitable descriptor limit before starting:

```sh
ulimit -n
```

Account for about two data-plane fds per tunnel plus probes/admin/listeners. The exact hard limit and permission to raise it depend on the OS. Raising the configured connection limit alone does not raise the process fd limit.

## Admin interface

The admin address must be IPv4 loopback (127/8). It intentionally has no public bind option.

| Request | Response |
|---|---|
| `GET /metrics` | JSON metrics; 200 |
| `GET /ready` | 200 with `ready:true` when at least one backend is eligible and not draining; otherwise 503 |
| `GET /health` | Same readiness semantics as `/ready` |
| Unknown path | 404 |
| Other method | 405 |

Check readiness after launch; the listening socket may open before the backend rise threshold is reached. Probe readiness establishes TCP reachability, not application correctness. Headers are capped at 8 KiB, sessions at one second and at most 32 concurrent admin sessions. All responses close the connection.

Counters and identities are documented in [METRICS.md](METRICS.md). Startup and final metrics are also newline-delimited JSON on stdout; backend transitions and draining notifications go to stderr. Application payloads are not logged.

## Stop and failure behavior

Use Ctrl+C or send SIGTERM to the proxy PID. The public listener closes, existing tunnels drain, and the deadline forces any leftovers closed. The final stdout event is `stopped` with terminal metrics.

Failed connects can be retried on an unattempted eligible backend. A connected stream is never retried/migrated after an application byte may have been delivered. If it resets, the application must decide whether retrying its operation is safe. A health transition is not an instruction to replay an established stream.

## Troubleshooting

| Symptom | Check |
|---|---|
| `Nothing to be done` | Build is up to date; run the executable/demo. |
| Bind/listen error | Port in use or address not local; use another port or port 0. |
| `localhost:9001` rejected | Use numeric IPv4, e.g. `127.0.0.1:9001`. |
| Readiness is 503 immediately | Wait for successful rise probes and inspect backend states. |
| Readiness 200 but requests hang | TCP reachability does not check application protocol; inspect service health and timeouts. |
| All HTTP requests reach one backend | They may share a keep-alive TCP connection; L4 routing is per connection. |
| Connection rejected | Inspect connection cap, eligibility, socket setup and OS fd limits. |
| Large response truncated after client EOF | Run tests and inspect half-close behavior; EOF is not full-duplex completion. |
| Admin endpoint unreachable from another host | Expected: admin binds only to loopback. |
| Benchmark appears slower at more clients | Clients/backends share the host, Python and loopback; inspect the stated end-to-end method. |
| Native Windows fails to build | Use WSL2/Linux; native Winsock/MSVC adaptation is not implemented. |

Build artifacts and private `study/` are ignored by Git. Public raw measurements under `docs/benchmarks/` are intentionally retainable.
