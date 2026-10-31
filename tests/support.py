"""Real TCP fixtures shared by integration tests, demo and benchmark (stdlib only)."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import selectors
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time


def wait_for(predicate, timeout=5):
    end = time.monotonic() + timeout
    error = None
    while time.monotonic() < end:
        try:
            result = predicate()
            if result:
                return result
        except (ConnectionError, OSError, http.client.HTTPException) as caught:
            error = caught
        time.sleep(.02)
    raise AssertionError(f"Condition did not become true: {error}")


class Backend:
    def __init__(self, name="backend", mode="echo", port=0, delay=0):
        self.name, self.mode, self.delay = name, mode, delay
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.connections = set()
        self.messages = 0
        owner = self

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True
            request_queue_size = 128

            def handle_error(self, request, client_address):
                pass

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                s = self.request
                s.settimeout(5)
                with owner.lock:
                    owner.connections.add(s)
                try:
                    if owner.mode == "stall":
                        owner.stop.wait(8)
                        return
                    if owner.mode == "greeting":
                        s.sendall(b"HELLO")
                        s.shutdown(socket.SHUT_WR)
                    collected = bytearray()
                    while not owner.stop.is_set():
                        if owner.delay:
                            time.sleep(owner.delay)
                        data = s.recv(65536)
                        if not data:
                            if owner.mode == "after-eof" and collected:
                                s.sendall(bytes(collected)[::-1])
                            break
                        with owner.lock:
                            owner.messages += 1
                        if owner.mode == "reset":
                            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                            s.close()
                            return
                        if owner.mode == "identity":
                            s.sendall(owner.name.encode() + b":" + data)
                        elif owner.mode == "after-eof":
                            collected.extend(data)
                        elif owner.mode != "greeting":
                            s.sendall(data)
                except OSError:
                    pass
                finally:
                    with owner.lock:
                        owner.connections.discard(s)

        self.server = Server(("127.0.0.1", port), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": .02}, daemon=True)
        self.thread.start()

    @property
    def address(self):
        return f"127.0.0.1:{self.port}"

    def close(self):
        self.stop.set()
        self.server.shutdown()
        self.server.server_close()
        with self.lock:
            for connection in list(self.connections):
                try:
                    connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        self.thread.join(timeout=2)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Balancer:
    def __init__(self, binary, backends, *options, ready=True, fd_limit=None):
        self.stderr = []
        self.final = None
        self.closed = False
        command = [str(Path(binary).resolve()), "--listen", "127.0.0.1:0", "--admin", "127.0.0.1:0",
                   "--health-interval-ms", "50", "--health-timeout-ms", "100", "--rise", "1", "--fall", "1"]
        for backend in backends:
            command.extend(["--backend", backend.address if isinstance(backend, Backend) else backend])
        command.extend(options)
        if fd_limit is not None:
            command = [sys.executable, "-c",
                       "import os, resource, sys; limit = int(sys.argv[1]); "
                       "resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit)); "
                       "os.execv(sys.argv[2], sys.argv[2:])", str(fd_limit), *command]
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.error_thread = threading.Thread(target=self._stderr, daemon=True)
        self.error_thread.start()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=5):
                    raise RuntimeError("Balancer startup timed out")
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("Balancer exited on startup: " + "".join(self.stderr))
            self.startup = json.loads(line)
            self.port = int(self.startup["listen"].rsplit(":", 1)[1])
            self.admin_port = int(self.startup["admin"].rsplit(":", 1)[1])
            if ready:
                wait_for(lambda: self.request("/ready")[0] == 200)
        except BaseException:
            self.close()
            raise

    def _stderr(self):
        for line in self.process.stderr:
            self.stderr.append(line)

    def request(self, path="/metrics", method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.admin_port, timeout=2)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def metrics(self):
        code, body = self.request()
        assert code == 200
        return body

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)

    def close(self):
        if self.closed:
            return self.final
        self.closed = True
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        output = self.process.stdout.read()
        self.error_thread.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()
        for line in output.splitlines():
            try:
                event = json.loads(line)
                if event.get("event") == "stopped":
                    self.final = event["metrics"]
            except ValueError:
                pass
        return self.final

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def exchange(address, payload, fragment=65536):
    """Full duplex: send concurrently, half-close, receive through EOF."""
    with socket.create_connection(address, timeout=8) as client:
        def send():
            for offset in range(0, len(payload), fragment):
                client.sendall(payload[offset:offset + fragment])
            client.shutdown(socket.SHUT_WR)
        with ThreadPoolExecutor(max_workers=1) as pool:
            sender = pool.submit(send)
            chunks = []
            while True:
                data = client.recv(65536)
                if not data:
                    break
                chunks.append(data)
            sender.result(timeout=8)
        return b"".join(chunks)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
