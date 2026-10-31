#include "lb/server.hpp"
#include <iostream>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop(int) {
    stop_requested = 1;
}
std::uint64_t number(const std::string& value) {
    if (value.empty() || value.size() > 10 ||
        value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("expected a positive decimal integer");
    }
    return std::stoull(value);
}
const char* help = R"(TCP Load Balancer — C++17, nonblocking IPv4 TCP proxy

Usage:
  tcp-lb --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 [options]

  --listen IPv4:port             default 127.0.0.1:8080; 0 selects a free port
  --admin IPv4:port              default 127.0.0.1:9091; loopback only
  --policy least-connections|round-robin
  --max-connections N            default 512
  --buffer-bytes N               default 65536 per direction
  --connect-timeout-ms N         default 1000
  --idle-timeout-ms N            default 30000
  --drain-timeout-ms N           default 5000
  --health-interval-ms N         default 1000
  --health-timeout-ms N          default 500
  --rise N                      successful probes before eligibility; default 2
  --fall N                      failed probes before exclusion; default 2

GET /metrics and /ready on the admin port. SIGINT/SIGTERM drains existing
connections; it stops accepting new ones. Backend probes check TCP reachability,
not application correctness. Existing streams are never replayed to a new backend.
)";
} // namespace
int main(int argc, char** argv) {
    try {
        lb::Config config;
        if (argc == 1) {
            std::cout << help;
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            std::string flag = argv[i];
            if (flag == "--help") {
                std::cout << help;
                return 0;
            }
            if (i + 1 == argc) {
                throw std::invalid_argument("missing value for " + flag);
            }
            std::string value = argv[++i];
            if (flag == "--listen") {
                config.listen = lb::endpoint(value, true);
            } else if (flag == "--admin") {
                config.admin = lb::endpoint(value, true);
            } else if (flag == "--backend") {
                config.backends.push_back(lb::endpoint(value));
            } else if (flag == "--policy") {
                if (value == "least-connections") {
                    config.policy = lb::Policy::LeastConnections;
                } else if (value == "round-robin") {
                    config.policy = lb::Policy::RoundRobin;
                } else {
                    throw std::invalid_argument("unknown policy");
                }
            } else if (flag == "--max-connections") {
                config.max_connections = number(value);
            } else if (flag == "--buffer-bytes") {
                config.buffer_bytes = number(value);
            } else if (flag == "--connect-timeout-ms") {
                config.connect_timeout_ms = number(value);
            } else if (flag == "--idle-timeout-ms") {
                config.idle_timeout_ms = number(value);
            } else if (flag == "--drain-timeout-ms") {
                config.drain_timeout_ms = number(value);
            } else if (flag == "--health-interval-ms") {
                config.health_interval_ms = number(value);
            } else if (flag == "--health-timeout-ms") {
                config.health_timeout_ms = number(value);
            } else if (flag == "--rise") {
                auto n = number(value);
                if (n > 10) {
                    throw std::invalid_argument("rise exceeds 10");
                }
                config.rise = n;
            } else if (flag == "--fall") {
                auto n = number(value);
                if (n > 10) {
                    throw std::invalid_argument("fall exceeds 10");
                }
                config.fall = n;
            } else {
                throw std::invalid_argument("unknown option: " + flag);
            }
        }
        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        return lb::run(config, stop_requested);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
