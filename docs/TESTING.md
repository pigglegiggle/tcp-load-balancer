# Verification and known limits

`make test` builds native unit tests and runs black-box TCP tests using only Python's standard library. `make sanitize` repeats both layers with AddressSanitizer and UndefinedBehaviorSanitizer. There are no third-party mocked network stacks in these tests.

## DSA and config

- Seed 20261101: ring buffers of capacities 1, 2, 7, 64 and 1024 are mutated against a deque reference for 20,000 operations each, checking bytes and size/full/empty invariants.
- Seed 20261102: 30,000 randomized least-connections acquisitions, releases, exclusions and health changes compare against an independent linear selector. Every step checks active counts, health flags and indexed heap positions.
- Explicit cases cover round robin fairness, empty pools, excluded IDs, the 64th mask bit, lease underflow and invalid endpoint/capacity/threshold configuration.

## Real network tests

| Case | What it establishes |
|---|---|
| 2 MiB binary and tiny fragments | Exact byte order through short reads/writes and ring wrap |
| Response after client SHUT_WR | Request EOF does not destroy the return path |
| Backend greeting + SHUT_WR | Client can keep transmitting after backend write EOF |
| Round robin / held least-connection session | Actual selection reflects policy and live leases |
| Concurrent clients | Payloads stay associated with their own streams |
| Backend close and restart | Health exclusion and re-entry, including rise/fall configuration |
| Failed connect to a stale-healthy backend | Pre-connect retry works before any upstream application delivery |
| Backend reset after receiving application data | No transparent replay to another backend |
| Connection cap / no eligible backend | Admission rejects promptly and counters conserve connections |
| Idle timeout | Real inactive tunnel expires |
| Small rings and slow backend | Full buffers pause reading and preserve payload integrity |
| Rapid client RSTs | Aborted peers/socket-setup failure do not terminate the reactor |
| Graceful and forced shutdown | Existing connection survives listener closure; drain deadline bounds exit |
| Admin malformed/method/oversize requests | Bounded GET-only admin interface behaves as documented |

Connection and lease conservation plus the fixed-buffer bound are checked after traffic settles. Startup/free ports and health polling have bounded waits; readiness races are not replaced with arbitrary multi-second sleeps. Fixture backends use threads; the proxy under test is an actual compiled child process.

## Not established by this suite

No Linux/WSL execution has been performed on the development Mac. Packet loss, physical network faults, kernel send/receive-buffer tuning, deliberate descriptor exhaustion, blackholed connect/probe timeout expiration, TLS interoperability/performance, long-running soak, IPv6 and production adversarial traffic have not been validated here. Connect/probe timeout paths are implemented, but localhost refusal tests do not prove blackhole timeout behavior. Sanitizers do not establish thread/race behavior for a future multithreaded design.

The reactor uses `poll`, so linear fd scans and repeated watch-vector construction become scalability costs. No claim is made to match an epoll/kqueue production proxy. Reproducible measured end-to-end results are in [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md).

Admin descriptor-pressure coverage starts a child with a 32-fd soft/hard limit, reserves a metrics connection, fills the admin listener, then checks bounded poll activity, the accept-error counter and subsequent data-plane recovery. The limit is applied in a small exec wrapper rather than an unsafe `preexec_fn` in the threaded test process.

A quiet established stream with a 60-second probe interval is observed for 300 ms; its poll counter must stay below ten additional calls. This rejects the former fixed 20 ms loop while allowing scheduling/admin-request overhead. Existing idle, health, admin-exhaustion and drain tests cover the deadlines included in adaptive polling.
