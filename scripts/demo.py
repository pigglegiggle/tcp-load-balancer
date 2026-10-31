"""Two real HTTP backends behind the C++ TCP proxy; no frontend framework."""

import argparse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from support import Balancer, wait_for


class HTTPBackend:
    def __init__(self, name, port=0):
        self.name = name
        self.closed = False

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = json.dumps({"backend": name, "path": self.path}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except OSError:
                    pass

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True
        )
        self.thread.start()

    @property
    def address(self):
        return f"127.0.0.1:{self.port}"

    def close(self):
        if not self.closed:
            self.closed = True
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=2)


def get(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request("GET", "/hello")
        response = connection.getresponse()
        assert response.status == 200
        return json.loads(response.read())["backend"]
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true", help="keep HTTP demo running until Ctrl+C")
    args = parser.parse_args()
    a, b = HTTPBackend("A"), HTTPBackend("B")
    lb = None
    try:
        options = ["--policy", "round-robin", "--rise", "2", "--health-interval-ms", "100"]
        if args.serve:
            options += ["--listen", "127.0.0.1:8600", "--admin", "127.0.0.1:8601"]
        lb = Balancer(ROOT / "build/tcp-lb", [a.address, b.address], *options)
        wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
        print(f"HTTP proxy: http://127.0.0.1:{lb.port}/hello", flush=True)
        print(f"Metrics:    http://127.0.0.1:{lb.admin_port}/metrics", flush=True)
        print(f"Backends:   {a.address} / {b.address}", flush=True)
        if args.serve:
            print("Refresh /hello to see A/B routing. Ctrl+C stops owned services.", flush=True)
            while lb.process.poll() is None:
                time.sleep(0.2)
            raise RuntimeError("Proxy stopped: " + "".join(lb.stderr))
        before = [get(lb.port) for _ in range(6)]
        print("Before failure:", before)
        assert before == ["A", "B"] * 3
        port = a.port
        a.close()
        wait_for(lambda: lb.metrics()["healthy_backends"] == 1)
        during = [get(lb.port) for _ in range(4)]
        print("A is down:     ", during)
        assert during == ["B"] * 4
        a = HTTPBackend("A", port)
        wait_for(lambda: lb.metrics()["healthy_backends"] == 2)
        after = [get(lb.port) for _ in range(6)]
        print("A recovered:   ", after)
        assert set(after) == {"A", "B"}
        print(json.dumps(lb.metrics(), indent=2))
        print("PASS: routing, failure exclusion, recovery, and real HTTP responses.")
    except KeyboardInterrupt:
        print("\nStopping demo.")
    finally:
        if lb:
            lb.close()
        a.close()
        b.close()


if __name__ == "__main__":
    main()
