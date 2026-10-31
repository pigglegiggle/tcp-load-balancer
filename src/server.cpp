#include "lb/server.hpp"
#include "lb/buffer.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <memory>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>

namespace lb {
namespace {
using Clock = std::chrono::steady_clock;
using Millis = std::int64_t;
Millis now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
constexpr std::size_t none = static_cast<std::size_t>(-1);
bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
std::system_error socket_error(const std::string &context) {
    return std::system_error(errno, std::generic_category(), context);
}

class Socket {
    int fd_ = -1;

  public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() {
        reset();
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    Socket &operator=(Socket &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const {
        return fd_;
    }
    explicit operator bool() const {
        return fd_ >= 0;
    }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
};
void prepare(int fd) {
    auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw socket_error("set nonblocking/close-on-exec");
    int enabled = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0)
        throw socket_error("TCP_NODELAY");
}
Socket tcp_socket() {
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket)
        throw socket_error("socket");
    prepare(socket.get());
    return socket;
}
sockaddr_in address(const Endpoint &endpoint) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.host.c_str(), &result.sin_addr) != 1)
        throw std::invalid_argument("numeric IPv4 address required");
    return result;
}
bool local_address(in_addr target) {
    if ((ntohl(target.s_addr) >> 24) == 127)
        return true;
    ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) < 0)
        throw socket_error("enumerate local IPv4 interfaces");
    std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owned(interfaces, ::freeifaddrs);
    for (auto *item = interfaces; item; item = item->ifa_next)
        if (item->ifa_addr && item->ifa_addr->sa_family == AF_INET &&
            reinterpret_cast<sockaddr_in *>(item->ifa_addr)->sin_addr.s_addr == target.s_addr)
            return true;
    return false;
}
bool targets_listener(const Endpoint &backend, const Endpoint &listener) {
    if (!listener.port || backend.port != listener.port)
        return false;
    auto target = address(backend).sin_addr;
    auto bound = address(listener).sin_addr;
    return target.s_addr == bound.s_addr ||
           (bound.s_addr == htonl(INADDR_ANY) && local_address(target));
}
std::string label(const Endpoint &e) {
    return e.host + ":" + std::to_string(e.port);
}
Socket listener(Endpoint &endpoint) {
    auto socket = tcp_socket();
    int enabled = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
        throw socket_error("SO_REUSEADDR");
    auto addr = address(endpoint);
    if (::bind(socket.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(socket.get(), 256) < 0)
        throw socket_error("listen " + label(endpoint));
    socklen_t size = sizeof(addr);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&addr), &size) < 0)
        throw socket_error("getsockname");
    endpoint.port = ntohs(addr.sin_port);
    return socket;
}
bool connected(int fd) {
    int error = 0;
    socklen_t size = sizeof(error);
    return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
}

struct Backend {
    Endpoint endpoint;
    Socket probe;
    Millis next_probe = 0, probe_deadline = 0;
    unsigned successes = 0, failures = 0;
    std::uint64_t probe_successes = 0, probe_failures = 0, connect_failures = 0;
};
struct Tunnel {
    Socket client, upstream;
    RingBuffer to_backend, to_client;
    std::size_t backend = none;
    std::uint64_t attempted = 0;
    Millis last_activity = 0, connect_deadline = 0;
    bool connecting = false, client_eof = false, upstream_eof = false;
    bool client_write_closed = false, upstream_write_closed = false;
    Tunnel(Socket socket, std::size_t capacity)
        : client(std::move(socket)), to_backend(capacity), to_client(capacity),
          last_activity(now_ms()) {}
};
struct Admin {
    Socket socket;
    std::string request, response;
    std::size_t sent = 0;
    Millis deadline = 0;
};
struct Stats {
    std::uint64_t accepted = 0, completed = 0, failed = 0, rejected = 0, rejected_limit = 0,
                  rejected_no_backend = 0;
    std::uint64_t connect_attempts = 0, connect_retries = 0, connect_failures = 0,
                  idle_timeouts = 0, forced_shutdowns = 0;
    std::uint64_t client_bytes_read = 0, backend_bytes_written = 0, backend_bytes_read = 0,
                  client_bytes_written = 0;
    std::uint64_t backpressure_pauses = 0, accept_errors = 0, socket_setup_failures = 0,
                  poll_calls = 0;
    std::size_t peak_connections = 0, peak_tunnel_buffered = 0;
};
enum class Kind { Listener, AdminListener, Client, Upstream, Probe, Admin };
struct Watch {
    Kind kind;
    std::uint64_t id;
    int fd;
};

class Reactor {
    Config config_;
    Socket listener_, admin_listener_;
    std::vector<Backend> backends_;
    BackendPool pool_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Tunnel>> tunnels_;
    std::unordered_map<std::uint64_t, Admin> admins_;
    Stats stats_;
    Millis started_ = now_ms(), drain_deadline_ = 0, accept_after_ = 0;
    bool draining_ = false;
    std::uint64_t next_id_ = 1;

    void health(std::size_t id, bool healthy) {
        if (pool_.healthy(id) == healthy)
            return;
        pool_.health(id, healthy);
        std::cerr << "backend " << label(backends_[id].endpoint) << " " << (healthy ? "up" : "down")
                  << '\n';
    }
    void probe_result(std::size_t id, bool success, Millis now) {
        auto &b = backends_[id];
        b.probe.reset();
        b.next_probe = now + config_.health_interval_ms;
        if (success) {
            ++b.probe_successes;
            b.failures = 0;
            b.successes = std::min(b.successes + 1, config_.rise);
            if (b.successes >= config_.rise)
                health(id, true);
        } else {
            ++b.probe_failures;
            b.successes = 0;
            b.failures = std::min(b.failures + 1, config_.fall);
            if (b.failures >= config_.fall)
                health(id, false);
        }
    }
    void start_probe(std::size_t id, Millis now) {
        auto &b = backends_[id];
        try {
            b.probe = tcp_socket();
        } catch (const std::system_error &) {
            ++stats_.socket_setup_failures;
            b.next_probe = now + config_.health_interval_ms;
            return;
        }
        auto addr = address(b.endpoint);
        auto result = ::connect(b.probe.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        if (result == 0)
            probe_result(id, true, now);
        else if (errno == EINPROGRESS)
            b.probe_deadline = now + config_.health_timeout_ms;
        else
            probe_result(id, false, now);
    }
    void connect_failed(Tunnel &tunnel) {
        ++stats_.connect_failures;
        auto id = tunnel.backend;
        if (id != none) {
            ++backends_[id].connect_failures;
            backends_[id].successes = 0;
            health(id, false);
            pool_.release(id);
        }
        tunnel.backend = none;
        tunnel.upstream.reset();
        tunnel.connecting = false;
    }
    bool route(Tunnel &tunnel, Millis now) {
        // Retry only before a backend connection succeeds. Never replay application bytes.
        for (;;) {
            auto selected = pool_.acquire(tunnel.attempted);
            if (!selected)
                return false;
            auto id = *selected;
            if (tunnel.attempted)
                ++stats_.connect_retries;
            tunnel.attempted |= std::uint64_t{1} << id;
            tunnel.backend = id;
            ++stats_.connect_attempts;
            try {
                tunnel.upstream = tcp_socket();
            } catch (const std::system_error &) {
                ++stats_.socket_setup_failures;
                pool_.release(id);
                tunnel.backend = none;
                return false;
            }
            auto addr = address(backends_[id].endpoint);
            auto result =
                ::connect(tunnel.upstream.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            if (result == 0) {
                tunnel.connecting = false;
                return true;
            }
            if (errno == EINPROGRESS) {
                tunnel.connecting = true;
                tunnel.connect_deadline = now + config_.connect_timeout_ms;
                return true;
            }
            connect_failed(tunnel);
        }
    }
    void close_tunnel(std::uint64_t id, bool complete) {
        auto found = tunnels_.find(id);
        if (found == tunnels_.end())
            return;
        if (found->second->backend != none)
            pool_.release(found->second->backend);
        tunnels_.erase(found);
        if (complete)
            ++stats_.completed;
        else
            ++stats_.failed;
    }
    void accept_clients(Millis now) {
        for (int count = 0; count < 64; ++count) {
            auto fd = ::accept(listener_.get(), nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == ECONNABORTED) {
                    ++stats_.accept_errors;
                    continue;
                }
                if (!would_block()) {
                    ++stats_.accept_errors;
                    accept_after_ = now + 100;
                }
                return;
            }
            Socket socket(fd);
            ++stats_.accepted;
            try {
                prepare(fd);
            } catch (const std::system_error &) {
                ++stats_.socket_setup_failures;
                ++stats_.rejected;
                continue;
            }
            if (tunnels_.size() >= config_.max_connections) {
                ++stats_.rejected;
                ++stats_.rejected_limit;
                continue;
            }
            auto tunnel = std::make_unique<Tunnel>(std::move(socket), config_.buffer_bytes);
            if (!route(*tunnel, now)) {
                ++stats_.rejected;
                ++stats_.rejected_no_backend;
                continue;
            }
            tunnels_.emplace(next_id_++, std::move(tunnel));
            stats_.peak_connections = std::max(stats_.peak_connections, tunnels_.size());
        }
    }
    bool receive(int fd, RingBuffer &buffer, bool &eof, std::uint64_t &bytes, Millis &activity) {
        std::size_t budget = 262144;
        while (!eof && !buffer.full() && budget) {
            auto n = ::recv(fd, buffer.write_data(), std::min(buffer.writable(), budget), 0);
            if (n > 0) {
                buffer.produced(static_cast<std::size_t>(n));
                bytes += n;
                budget -= n;
                activity = now_ms();
                if (buffer.full())
                    ++stats_.backpressure_pauses;
            } else if (n == 0) {
                eof = true;
                break;
            } else if (errno == EINTR)
                continue;
            else
                return would_block();
        }
        return true;
    }
    bool send(int fd, RingBuffer &buffer, std::uint64_t &bytes, Millis &activity) {
        std::size_t budget = 262144;
        while (!buffer.empty() && budget) {
            auto n = ::send(fd, buffer.read_data(), std::min(buffer.readable(), budget), 0);
            if (n > 0) {
                buffer.consumed(static_cast<std::size_t>(n));
                bytes += n;
                budget -= n;
                activity = now_ms();
            } else if (n < 0 && errno == EINTR)
                continue;
            else if (n < 0 && would_block())
                return true;
            else
                return false;
        }
        return true;
    }
    bool half_close(Tunnel &t) {
        // EOF closes one direction only, after its last buffered byte is delivered.
        if (t.client_eof && t.to_backend.empty() && !t.connecting && !t.upstream_write_closed) {
            if (::shutdown(t.upstream.get(), SHUT_WR) < 0 && errno != ENOTCONN)
                return false;
            t.upstream_write_closed = true;
        }
        if (t.upstream_eof && t.to_client.empty() && !t.client_write_closed) {
            if (::shutdown(t.client.get(), SHUT_WR) < 0 && errno != ENOTCONN)
                return false;
            t.client_write_closed = true;
        }
        return true;
    }
    void traffic(const Watch &watch, short events, Millis now) {
        auto found = tunnels_.find(watch.id);
        if (found == tunnels_.end())
            return;
        auto &t = *found->second;
        bool client = watch.kind == Kind::Client;
        auto fd = client ? t.client.get() : t.upstream.get();
        if (fd != watch.fd)
            return;
        if (!client && t.connecting) {
            if (!connected(fd)) {
                connect_failed(t);
                if (!route(t, now))
                    close_tunnel(watch.id, false);
                return;
            }
            t.connecting = false;
        }
        bool okay = true;
        if (events & POLLOUT)
            okay = client ? send(fd, t.to_client, stats_.client_bytes_written, t.last_activity)
                          : send(fd, t.to_backend, stats_.backend_bytes_written, t.last_activity);
        if (okay && (events & (POLLIN | POLLHUP)))
            okay = client ? receive(fd, t.to_backend, t.client_eof, stats_.client_bytes_read,
                                    t.last_activity)
                          : receive(fd, t.to_client, t.upstream_eof, stats_.backend_bytes_read,
                                    t.last_activity);
        stats_.peak_tunnel_buffered =
            std::max(stats_.peak_tunnel_buffered, t.to_backend.size() + t.to_client.size());
        if (events & (POLLERR | POLLNVAL))
            okay = false;
        if (okay)
            okay = half_close(t);
        if (!okay) {
            close_tunnel(watch.id, false);
            return;
        }
        if (t.client_eof && t.upstream_eof && t.to_backend.empty() && t.to_client.empty())
            close_tunnel(watch.id, true);
    }
    std::size_t healthy_count() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < backends_.size(); ++i)
            if (pool_.healthy(i))
                ++n;
        return n;
    }
    std::string metrics() const {
        std::ostringstream o;
        std::size_t buffered = 0;
        for (const auto &pair : tunnels_)
            buffered += pair.second->to_backend.size() + pair.second->to_client.size();
        o << "{\"policy\":\""
          << (config_.policy == Policy::RoundRobin ? "round-robin" : "least-connections")
          << "\",\"draining\":" << (draining_ ? "true" : "false")
          << ",\"uptime_ms\":" << now_ms() - started_ << ",\"active\":" << tunnels_.size()
          << ",\"healthy_backends\":" << healthy_count() << ",\"accepted\":" << stats_.accepted
          << ",\"completed\":" << stats_.completed << ",\"failed\":" << stats_.failed
          << ",\"rejected\":" << stats_.rejected << ",\"rejected_limit\":" << stats_.rejected_limit
          << ",\"rejected_no_backend\":" << stats_.rejected_no_backend
          << ",\"connect_attempts\":" << stats_.connect_attempts
          << ",\"connect_failures\":" << stats_.connect_failures
          << ",\"connect_retries\":" << stats_.connect_retries
          << ",\"idle_timeouts\":" << stats_.idle_timeouts
          << ",\"forced_shutdowns\":" << stats_.forced_shutdowns
          << ",\"client_bytes_read\":" << stats_.client_bytes_read
          << ",\"backend_bytes_written\":" << stats_.backend_bytes_written
          << ",\"backend_bytes_read\":" << stats_.backend_bytes_read
          << ",\"client_bytes_written\":" << stats_.client_bytes_written
          << ",\"buffered_bytes\":" << buffered
          << ",\"peak_tunnel_buffered\":" << stats_.peak_tunnel_buffered
          << ",\"buffer_capacity_per_direction\":" << config_.buffer_bytes
          << ",\"backpressure_pauses\":" << stats_.backpressure_pauses
          << ",\"peak_connections\":" << stats_.peak_connections
          << ",\"accept_errors\":" << stats_.accept_errors
          << ",\"socket_setup_failures\":" << stats_.socket_setup_failures
          << ",\"poll_calls\":" << stats_.poll_calls << ",\"backends\":[";
        for (std::size_t i = 0; i < backends_.size(); ++i) {
            const auto &b = backends_[i];
            if (i)
                o << ',';
            o << "{\"address\":\"" << label(b.endpoint)
              << "\",\"healthy\":" << (pool_.healthy(i) ? "true" : "false")
              << ",\"active\":" << pool_.active(i) << ",\"selections\":" << pool_.selections(i)
              << ",\"probe_successes\":" << b.probe_successes
              << ",\"probe_failures\":" << b.probe_failures
              << ",\"connect_failures\":" << b.connect_failures << '}';
        }
        o << "]}";
        return o.str();
    }
    void accept_admins(Millis now) {
        for (int count = 0; count < 16; ++count) {
            auto fd = ::accept(admin_listener_.get(), nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            Socket socket(fd);
            try {
                prepare(fd);
            } catch (const std::system_error &) {
                ++stats_.socket_setup_failures;
                continue;
            }
            if (admins_.size() < 32)
                admins_.emplace(next_id_++, Admin{std::move(socket), {}, {}, 0, now + 1000});
        }
    }
    void respond(Admin &admin, int status, const std::string &body) {
        const char *reason = status == 200   ? "OK"
                             : status == 503 ? "Service Unavailable"
                             : status == 404 ? "Not Found"
                             : status == 405 ? "Method Not Allowed"
                             : status == 431 ? "Request Header Fields Too Large"
                                             : "Bad Request";
        admin.response =
            "HTTP/1.1 " + std::to_string(status) + " " + reason +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
    }
    void admin_event(const Watch &watch, short events) {
        auto found = admins_.find(watch.id);
        if (found == admins_.end() || found->second.socket.get() != watch.fd)
            return;
        auto &a = found->second;
        bool close = false;
        if (a.response.empty() && (events & (POLLIN | POLLHUP))) {
            char data[1024];
            auto n = ::recv(watch.fd, data, sizeof(data), 0);
            if (n > 0) {
                a.request.append(data, n);
                if (a.request.size() > 8192)
                    respond(a, 431, "{\"error\":\"headers too large\"}");
                else if (a.request.find("\r\n\r\n") != std::string::npos) {
                    auto end = a.request.find("\r\n");
                    std::istringstream line(a.request.substr(0, end));
                    std::string method, path, version, extra;
                    if (!(line >> method >> path >> version) || (line >> extra) ||
                        (version != "HTTP/1.1" && version != "HTTP/1.0"))
                        respond(a, 400, "{\"error\":\"bad request\"}");
                    else if (method != "GET")
                        respond(a, 405, "{\"error\":\"GET only\"}");
                    else if (path == "/metrics")
                        respond(a, 200, metrics());
                    else if (path == "/ready" || path == "/health") {
                        bool ready = !draining_ && healthy_count() > 0;
                        respond(a, ready ? 200 : 503,
                                ready ? "{\"ready\":true}" : "{\"ready\":false}");
                    } else
                        respond(a, 404, "{\"error\":\"not found\"}");
                }
            } else if (n == 0 || (n < 0 && errno != EINTR && !would_block()))
                close = true;
        }
        if (!close && !a.response.empty() && (events & POLLOUT)) {
            auto n = ::send(watch.fd, a.response.data() + a.sent, a.response.size() - a.sent, 0);
            if (n > 0) {
                a.sent += n;
                if (a.sent == a.response.size())
                    close = true;
            } else if (n == 0 || (errno != EINTR && !would_block()))
                close = true;
        }
        if (events & (POLLERR | POLLNVAL))
            close = true;
        if (close)
            admins_.erase(found);
    }
    void timers(Millis now, volatile std::sig_atomic_t &stop) {
        if (stop && !draining_) {
            draining_ = true;
            drain_deadline_ = now + config_.drain_timeout_ms;
            listener_.reset();
            for (auto &b : backends_)
                b.probe.reset();
            std::cerr << "draining existing connections\n";
        }
        if (!draining_)
            for (std::size_t i = 0; i < backends_.size(); ++i) {
                auto &b = backends_[i];
                if (b.probe && now >= b.probe_deadline)
                    probe_result(i, false, now);
                if (!b.probe && now >= b.next_probe)
                    start_probe(i, now);
            }
        std::vector<std::uint64_t> expired;
        for (auto &pair : tunnels_) {
            auto id = pair.first;
            auto &t = *pair.second;
            if (draining_ && now >= drain_deadline_) {
                ++stats_.forced_shutdowns;
                expired.push_back(id);
            } else if (now - t.last_activity >= config_.idle_timeout_ms) {
                ++stats_.idle_timeouts;
                expired.push_back(id);
            } else if (t.connecting && now >= t.connect_deadline) {
                connect_failed(t);
                if (!route(t, now))
                    expired.push_back(id);
            }
        }
        for (auto id : expired)
            close_tunnel(id, false);
        for (auto i = admins_.begin(); i != admins_.end();)
            if (now >= i->second.deadline)
                i = admins_.erase(i);
            else
                ++i;
    }

  public:
    explicit Reactor(Config config)
        : config_(std::move(config)), pool_(config_.backends.size(), config_.policy) {
        listener_ = listener(config_.listen);
        admin_listener_ = listener(config_.admin);
        // Recheck OS-assigned ports before probes or application routing can reach ourselves.
        validate(config_);
        for (const auto &endpoint : config_.backends) {
            Backend b;
            b.endpoint = endpoint;
            backends_.push_back(std::move(b));
        }
        std::cout << "{\"event\":\"listening\",\"listen\":\"" << label(config_.listen)
                  << "\",\"admin\":\"" << label(config_.admin) << "\",\"pid\":" << ::getpid()
                  << "}\n"
                  << std::flush;
    }
    int loop(volatile std::sig_atomic_t &stop) {
        for (;;) {
            auto now = now_ms();
            timers(now, stop);
            if (draining_ && tunnels_.empty())
                break;
            std::vector<pollfd> fds;
            std::vector<Watch> watches;
            fds.reserve(2 * tunnels_.size() + backends_.size() + admins_.size() + 2);
            watches.reserve(fds.capacity());
            auto watch = [&](int fd, short events, Kind kind, std::uint64_t id = 0) {
                if (fd < 0 || !events)
                    return;
                fds.push_back({fd, events, 0});
                watches.push_back({kind, id, fd});
            };
            if (!draining_ && now >= accept_after_)
                watch(listener_.get(), POLLIN, Kind::Listener);
            watch(admin_listener_.get(), POLLIN, Kind::AdminListener);
            for (std::size_t i = 0; i < backends_.size(); ++i)
                if (backends_[i].probe)
                    watch(backends_[i].probe.get(), POLLOUT, Kind::Probe, i);
            for (const auto &pair : tunnels_) {
                auto id = pair.first;
                const auto &t = *pair.second;
                short client = 0, upstream = 0;
                if (!t.client_eof && !t.to_backend.full())
                    client |= POLLIN;
                if (!t.to_client.empty())
                    client |= POLLOUT;
                if (t.connecting)
                    upstream = POLLOUT;
                else {
                    if (!t.upstream_eof && !t.to_client.full())
                        upstream |= POLLIN;
                    if (!t.to_backend.empty())
                        upstream |= POLLOUT;
                }
                watch(t.client.get(), client, Kind::Client, id);
                watch(t.upstream.get(), upstream, Kind::Upstream, id);
            }
            for (const auto &pair : admins_)
                watch(pair.second.socket.get(), pair.second.response.empty() ? POLLIN : POLLOUT,
                      Kind::Admin, pair.first);
            ++stats_.poll_calls;
            auto count = ::poll(fds.data(), fds.size(), 20);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw socket_error("poll");
            }
            now = now_ms();
            for (std::size_t i = 0; i < fds.size(); ++i) {
                auto events = fds[i].revents;
                if (!events)
                    continue;
                const auto &w = watches[i];
                switch (w.kind) {
                case Kind::Listener:
                    if (listener_ && !stop)
                        accept_clients(now);
                    break;
                case Kind::AdminListener:
                    accept_admins(now);
                    break;
                case Kind::Probe:
                    if (backends_[w.id].probe.get() == w.fd)
                        probe_result(w.id, connected(w.fd), now);
                    break;
                case Kind::Admin:
                    admin_event(w, events);
                    break;
                default:
                    traffic(w, events, now);
                    break;
                }
            }
        }
        std::cout << "{\"event\":\"stopped\",\"metrics\":" << metrics() << "}\n" << std::flush;
        return 0;
    }
};
} // namespace
Endpoint endpoint(const std::string &text, bool allow_zero) {
    auto colon = text.find(':');
    if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos)
        throw std::invalid_argument("endpoint must be numeric IPv4:port");
    auto host = text.substr(0, colon), port = text.substr(colon + 1);
    if (port.empty() || port.size() > 5 ||
        !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("invalid port");
    auto number = std::stoul(port);
    if (number > 65535 || (!allow_zero && !number))
        throw std::invalid_argument("port out of range");
    Endpoint result{host, static_cast<std::uint16_t>(number)};
    address(result);
    return result;
}
void validate(const Config &c) {
    address(c.listen);
    address(c.admin);
    if ((ntohl(address(c.admin).sin_addr.s_addr) >> 24) != 127)
        throw std::invalid_argument("admin must bind to IPv4 loopback");
    if (c.backends.empty() || c.backends.size() > 64)
        throw std::invalid_argument("provide 1..64 backends");
    std::vector<std::string> endpoints;
    for (const auto &b : c.backends) {
        address(b);
        if (!b.port)
            throw std::invalid_argument("backend port cannot be zero");
        if (address(b).sin_addr.s_addr == htonl(INADDR_ANY))
            throw std::invalid_argument("backend cannot use the unspecified IPv4 address");
        if (targets_listener(b, c.listen) || targets_listener(b, c.admin))
            throw std::invalid_argument("backend points to this process's public/admin listener: " +
                                        label(b));
        auto text = label(b);
        if (std::find(endpoints.begin(), endpoints.end(), text) != endpoints.end())
            throw std::invalid_argument("duplicate backend");
        endpoints.push_back(text);
    }
    if (!c.max_connections || c.max_connections > 16384 || c.buffer_bytes < 1024 ||
        c.buffer_bytes > 4 * 1024 * 1024 ||
        c.max_connections * c.buffer_bytes * 2 > 1024ULL * 1024 * 1024)
        throw std::invalid_argument("invalid capacity or configured buffers exceed 1 GiB");
    for (auto time : {c.connect_timeout_ms, c.idle_timeout_ms, c.drain_timeout_ms,
                      c.health_interval_ms, c.health_timeout_ms})
        if (time < 10 || time > 3600000)
            throw std::invalid_argument("timeouts must be 10..3600000 milliseconds");
    if (!c.rise || c.rise > 10 || !c.fall || c.fall > 10)
        throw std::invalid_argument("health thresholds must be 1..10");
}
int run(const Config &config, volatile std::sig_atomic_t &stop_requested) {
    validate(config);
    Reactor reactor(config);
    return reactor.loop(stop_requested);
}
} // namespace lb
