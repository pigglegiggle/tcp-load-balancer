# Metrics reference

All values are a single reactor snapshot. Counters reset at process start and are unsigned totals. The admin request itself is not a public client connection. Health probes bypass the public listener and do not count as accepted application traffic.

| Field | Definition |
|---|---|
| `policy` | Configured routing policy |
| `draining` | Stop requested, public listener closed |
| `uptime_ms` | Monotonic elapsed process runtime |
| `healthy_backends` | Eligible for new routing after rise/fall rules |
| `active` | Tunnels currently stored; includes upstream connects in progress |
| `accepted` | Accepted data-plane client sockets, including later admission rejection |
| `completed` | Both directional EOFs reached and both app rings emptied |
| `failed` | An admitted tunnel ended on transport/setup failure, idle timeout or forced drain |
| `rejected` | Accepted client not entered into active map: capacity, socket setup or initial routing failure |
| `rejected_limit` | Rejected specifically by active-tunnel cap |
| `rejected_no_backend` | Initial routing could not supply a usable upstream connection; includes no eligible backend and immediate routing/setup failure |
| `connect_attempts` | Attempts after a backend lease was acquired; includes setup attempts |
| `connect_failures` | Immediate/asynchronous failed upstream connects and connect timeout; not local socket allocation failure |
| `connect_retries` | Additional backend selections for the same client before connect success |
| `idle_timeouts` | Tunnels expired since their last successful byte I/O |
| `forced_shutdowns` | Tunnels closed at drain deadline |
| `client_bytes_read` | Successful recv bytes from clients into pending rings |
| `backend_bytes_written` | Successful send bytes delivered into upstream TCP sockets |
| `backend_bytes_read` | Successful recv bytes from backends into pending rings |
| `client_bytes_written` | Successful send bytes delivered into client TCP sockets |
| `buffered_bytes` | Current sum of occupied app rings; excludes kernel buffers |
| `peak_tunnel_buffered` | Max combined occupied bytes in one tunnel observed after a socket dispatch; not process RAM or global concurrent peak |
| `buffer_capacity_per_direction` | Allocated ring capacity C; each tunnel owns two |
| `backpressure_pauses` | Read operations that filled a ring, causing its read interest to pause |
| `peak_connections` | Max active tunnel-map size |
| `accept_errors` | Non-EAGAIN public-listener accept errors, including aborted incoming connects |
| `admin_accept_errors` | Non-EAGAIN admin-listener accept errors; resource errors pause admin accepts for 100 ms |
| `socket_setup_failures` | Nonblocking/TCP option/socket creation errors in handled client/admin/probe/upstream setup paths |
| `poll_calls` | Calls to the readiness poll, including calls interrupted by signals |

Accounting identity at every snapshot:

```text
accepted = active + completed + failed + rejected
```

On normal idle completion, client-read equals backend-written and backend-read equals client-written. On failed/rejected streams, buffered or kernel-queued data can be lost and these totals need not match; a successful send is not an application-level acknowledgement.

Each backend reports `address`, `healthy`, `active`, `selections`, `probe_successes`, `probe_failures`, `connect_failures`. Backend `active` includes connecting and established leases. Summed selections can exceed accepted clients because of safe pre-connect retries. When unhealthy, existing leases remain visible even though the backend is removed from the selection heap.
