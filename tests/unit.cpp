#include "lb/backends.hpp"
#include "lb/buffer.hpp"
#include "lb/server.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool result, const char *message) {
    ++checks;
    if (!result)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "expected rejection");
}
void buffer_test() {
    std::mt19937 rng(20261101);
    for (std::size_t capacity : {1, 2, 7, 64, 1024}) {
        lb::RingBuffer buffer(capacity);
        std::deque<char> reference;
        for (int step = 0; step < 20000; ++step) {
            if (rng() % 2 && !buffer.full()) {
                auto count = 1 + rng() % buffer.writable();
                auto target = buffer.write_data();
                for (std::size_t i = 0; i < count; ++i) {
                    target[i] = static_cast<char>(rng() % 256);
                    reference.push_back(target[i]);
                }
                buffer.produced(count);
            } else if (!buffer.empty()) {
                auto count = 1 + rng() % buffer.readable();
                for (std::size_t i = 0; i < count; ++i) {
                    check(buffer.read_data()[i] == reference.front(), "ring byte order");
                    reference.pop_front();
                }
                buffer.consumed(count);
            }
            check(buffer.size() == reference.size(), "ring size");
            check(buffer.size() <= capacity, "ring capacity");
            check(buffer.full() == (reference.size() == capacity), "ring full");
            check(buffer.empty() == reference.empty(), "ring empty");
        }
        rejects([&] { buffer.produced(buffer.writable() + 1); });
        rejects([&] { buffer.consumed(buffer.size() + 1); });
    }
    rejects([] { lb::RingBuffer invalid(0); });
}
void pool_test() {
    using namespace lb;
    BackendPool rr(3, Policy::RoundRobin);
    check(!rr.acquire(), "no optimistic healthy backends");
    for (std::size_t i = 0; i < 3; ++i)
        rr.health(i, true);
    for (std::size_t i = 0; i < 30; ++i) {
        auto id = rr.acquire();
        check(id && *id == i % 3, "round robin");
        rr.release(*id);
    }
    rr.health(1, false);
    for (int i = 0; i < 10; ++i) {
        auto id = rr.acquire();
        check(id && *id != 1, "unhealthy skipped");
        rr.release(*id);
    }
    check(!rr.acquire(7), "excluded backends");
    check(rr.valid(), "round robin map invariant");
    std::mt19937 rng(20261102);
    constexpr std::size_t n = 17;
    BackendPool heap(n, Policy::LeastConnections);
    std::vector<bool> healthy(n);
    std::vector<std::size_t> active(n);
    std::vector<std::uint64_t> last(n);
    std::uint64_t sequence = 0;
    for (int step = 0; step < 30000; ++step) {
        auto id = rng() % n;
        if (rng() % 3 == 0) {
            healthy[id] = !healthy[id];
            heap.health(id, healthy[id]);
        } else if (rng() % 2 && active[id]) {
            --active[id];
            heap.release(id);
        } else {
            auto excluded = static_cast<std::uint64_t>(rng()) & ((std::uint64_t{1} << n) - 1);
            std::optional<std::size_t> expected;
            for (std::size_t i = 0; i < n; ++i)
                if (healthy[i] && !(excluded & (std::uint64_t{1} << i))) {
                    if (!expected || active[i] < active[*expected] ||
                        (active[i] == active[*expected] &&
                         (last[i] < last[*expected] ||
                          (last[i] == last[*expected] && i < *expected))))
                        expected = i;
                }
            auto actual = heap.acquire(excluded);
            check(actual == expected, "least connections reference");
            if (expected) {
                ++active[*expected];
                last[*expected] = ++sequence;
            }
        }
        check(heap.valid(), "indexed heap invariant");
        for (std::size_t i = 0; i < n; ++i)
            check(heap.active(i) == active[i] && heap.healthy(i) == healthy[i],
                  "backend accounting");
    }
    BackendPool last_bit(64, Policy::LeastConnections);
    last_bit.health(63, true);
    check(!last_bit.acquire(std::uint64_t{1} << 63), "64th exclusion bit");
    check(last_bit.acquire() == 63, "64th selection");
    rejects([] { BackendPool invalid(0, Policy::RoundRobin); });
    rejects([] { BackendPool invalid(65, Policy::RoundRobin); });
    BackendPool empty(1, Policy::RoundRobin);
    rejects([&] { empty.release(0); });
}
void config_test() {
    using namespace lb;
    check(endpoint("127.0.0.1:65535").port == 65535, "port parsing");
    check(endpoint("127.0.0.1:0", true).port == 0, "ephemeral port");
    for (auto value : {"localhost:80", "127.0.0.1:0", "127.0.0.1:-1", "127.0.0.1:65536", "::1:80",
                       "127.0.0.1:+2", "127.0.0.1:12x"})
        rejects([&] { endpoint(value); });
    Config c;
    c.backends = {endpoint("127.0.0.1:1234")};
    validate(c);
    auto bad = c;
    bad.admin = endpoint("0.0.0.0:9000");
    rejects([&] { validate(bad); });
    bad = c;
    bad.backends.push_back(c.backends[0]);
    rejects([&] { validate(bad); });
    bad = c;
    bad.buffer_bytes = 0;
    rejects([&] { validate(bad); });
    bad = c;
    bad.max_connections = 16384;
    bad.buffer_bytes = 4 * 1024 * 1024;
    rejects([&] { validate(bad); });
    bad = c;
    bad.rise = 0;
    rejects([&] { validate(bad); });
    bad = c;
    bad.idle_timeout_ms = 0;
    rejects([&] { validate(bad); });
}
} // namespace
int main() {
    try {
        buffer_test();
        pool_test();
        config_test();
        std::cout << checks << " checks passed; seeds 20261101/20261102\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
