#pragma once
#include "lb/backends.hpp"
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

namespace lb {
struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};
struct Config {
    Endpoint listen{"127.0.0.1", 8080}, admin{"127.0.0.1", 9091};
    std::vector<Endpoint> backends;
    Policy policy = Policy::LeastConnections;
    std::size_t max_connections = 512, buffer_bytes = 65536;
    std::int64_t connect_timeout_ms = 1000, idle_timeout_ms = 30000, drain_timeout_ms = 5000;
    std::int64_t health_interval_ms = 1000, health_timeout_ms = 500;
    unsigned rise = 2, fall = 2;
};
Endpoint endpoint(const std::string &text, bool allow_zero = false);
void validate(const Config &config);
int run(const Config &config, volatile std::sig_atomic_t &stop_requested);
} // namespace lb
