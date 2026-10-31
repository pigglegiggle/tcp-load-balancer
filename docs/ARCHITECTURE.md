# Architecture and connection lifecycle

## Ownership

One reactor owns the backend pool, tunnel map, health probes and admin sessions. Every socket is move-only RAII state. `Socket` closes its fd when reset or destroyed; no raw owning fd is shared between two owners. Socket setup sets O_NONBLOCK, FD_CLOEXEC and TCP_NODELAY. The executable ignores SIGPIPE so EPIPE is a connection failure, not process termination.

A tunnel holds a client socket, upstream socket, two ring buffers, EOF/write-shutdown flags, current backend lease, attempted-backend mask and real monotonic timestamps. Each direction has a separate ring and independent EOF state. The backend table outlives all tunnels and its IDs remain stable.

The Python tools are clients/test services, not part of forwarding or routing. The compiled proxy never invokes Python or shells while handling traffic.

## Event loop

Before each poll, the reactor checks timeout/probe/drain state, then constructs descriptor/watch vectors. `poll` waits until the nearest connect, idle, probe, admin, drain or accept-backoff deadline, or until readiness/signal activity, with a 250 ms maximum sleep. The maximum bounds shutdown responsiveness if a stop signal arrives between checking its flag and entering poll. Cancelled probes do not contribute deadlines while draining. One dispatch performs bounded work: up to 64 public accepts, 16 admin accepts, and 256 KiB per socket I/O direction. No production thread pool is used.

Watch records contain kind, connection ID and fd. Before handling a tunnel/admin event, the map entry must still exist and its current fd must match. An fd number reused by a newly accepted connection does not inherit the old connection ID. Probe watches similarly check the current backend probe fd. Changing socket state affects the next iteration's interests.

The poll vectors are rebuilt each pass, making total readiness bookkeeping linear in the watched socket count. This project favors a portable, inspectable baseline over an unmeasured scalability claim. `poll` is not an implementation of Linux epoll or macOS kqueue.

## Connect and lease states

1. Accept client, prepare its fd and enforce capacity.
2. Acquire one eligible backend lease and set the attempt bit.
3. Create a nonblocking upstream socket. Immediate connect success enters forwarding; EINPROGRESS waits for readiness.
4. On async connect readiness, read SO_ERROR. Zero enters forwarding; a failure releases the lease, excludes that backend immediately and tries an eligible unattempted backend.
5. While connecting, client bytes may fill the bounded pending ring, but cannot be sent upstream yet.
6. After connection success, a data-path error terminates the tunnel without replay or migration. A partially handled application operation must not be silently duplicated.
7. All final tunnel exits release exactly one live backend lease. A failed attempt is released before another is acquired.

An accepted client that cannot be admitted/routed immediately is `rejected`; one already in the active map that later fails is `failed`. `accepted = active + completed + failed + rejected`. Backend selection counts include retries, so their sum need not equal accepted clients.

## Forwarding and backpressure

`recv` writes into the ring's contiguous writable span. `send` reads its contiguous readable span and consumes exactly its positive return value. Wrapped buffers are handled by subsequent spans, not memory shifting. EAGAIN/EWOULDBLOCK stops the loop until readiness returns; EINTR retries within the dispatch budget. Errors close the affected tunnel.

When the client-to-backend ring is full, client POLLIN is disabled. When the reverse ring is full, upstream POLLIN is disabled. A socket with neither needed reads nor pending writes is omitted from the poll set, rather than watched with zero interests. Write interests exist only while a ring has pending bytes (or a connect is pending), avoiding a permanent writable-socket busy loop.

The application-ring bound is deterministic. Kernel socket buffers, accept backlog, C++ containers and allocator behavior are separate memory costs. `peak_tunnel_buffered` measures occupied bytes observed after dispatch, not allocated capacity or OS RSS; transient occupancy within an individual dispatch may be higher than that observed snapshot, but can never exceed the fixed ring limits.

## TCP half-close

Read EOF sets the corresponding read-finished flag. Its pending bytes are still forwarded. Once drained, `shutdown(destination, SHUT_WR)` propagates EOF only for that direction. The opposite direction remains usable.

Both read EOF flags plus both empty rings finish the tunnel normally. A backend may require client EOF before computing its response, or may send a greeting then close only its write side; tests cover both cases. POLLHUP can accompany readable data, so readiness handling attempts bounded reads rather than assuming that a hangup means all data should be discarded. POLLERR/POLLNVAL or failed I/O terminates the stream.

## Health model

Backends begin ineligible. At each probe interval, the reactor attempts a separate nonblocking TCP connection. Success must occur `rise` times consecutively to enable routing; `fall` consecutive failures disables an eligible backend. Opposite outcomes reset the consecutive count. Timeouts count as failed probes. Checks are scheduled after the prior result, not as an unbounded queue of overlapping probes.

Client-connect failures use immediate passive exclusion, independent of the active-probe fall threshold. Local probe socket-setup failure defers the probe and increments its counter instead of concluding that the backend is down. Probe connections never acquire traffic leases. Some backend applications will observe an empty TCP connection for each successful probe.

Health status changes influence **new** connections only. Existing connected streams continue until their own EOF, error, idle timeout or drain deadline. Healthy means TCP reachable, not that an application request will complete correctly or quickly.

## Selection invariants

For healthy backend IDs, `positions[heap[i]] == i`; unhealthy IDs use a missing-position sentinel. The heap contains exactly the eligible backend set. Its comparator is `(active tunnels including connecting, last-selection sequence, backend ID)`.

Acquisition increments active/selection counts and repairs the changed heap position; release decrements active and repairs if the backend remains eligible. Unhealthy backends retain their lease counts outside the heap, so recovery reinserts their correct load. Stable dense IDs permit O(1) position lookup without a hash table.

Normal least-connections acquisition/release and health transitions are O(log B), with O(1) root inspection. Excluded retry roots are temporarily removed/restored, up to O(B log B). Round robin scans at most B entries and still updates the shared heap bookkeeping. B is bounded at 64. The connection maps use expected O(1) hash lookups; no worst-case hash latency claim is made.

## Draining

The signal handler sets a `sig_atomic_t` flag. In the loop, draining closes the public listener, cancels health probes and sets a monotonic drain deadline. Admin readiness becomes 503 while metrics remain available as long as tunnels remain. Existing tunnels can finish forwarding and half-close normally. After the deadline, remaining tunnels are failed/closed and leases released. Once no active tunnels remain, the process emits final JSON metrics and exits; it does not wait indefinitely for admin clients.

## Scope

IPv4 numeric static endpoints; no DNS refresh, IPv6, dynamic config reload, weighted routing, sticky cookies, HTTP routing, TLS termination, cross-machine orchestration or production access-control system. Payloads are opaque; the admin interface is a bounded, GET-only local HTTP implementation, not a general HTTP server. Production deployment would require operational/security review appropriate to its environment.

There is no hardware scheduling, CUDA, simulated network timing or fabricated workload execution time. Network tests and measurements use actual sockets and wall clocks, with the stated loopback limits.
