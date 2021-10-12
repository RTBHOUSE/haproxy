from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

import sys
import time
import threading
import urllib.parse

class Counter():
    def __init__(self):
        self.lock = threading.Lock()
        self.count = 0

    def inc(self):
        with self.lock:
            self.count += 1
        return self.count

    def dec(self):
        with self.lock:
            self.count -= 1
        return self.count

global inflight
inflight = Counter()

global healthy
healthy = True

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/inflight"):
            self.__respond_inflight()
        elif self.path.startswith("/health"):
            self.__respond_health()
        else:
            inflight.inc()
            try:
                time.sleep(0.05*inflight.count)
                self.__respond_inflight()
            except:
                pass
            finally:
                inflight.dec()

    def __respond_inflight(self):
        response = f"{str(inflight.count)}\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(bytes(response, "utf-8"))

    def __respond_health(self):
        global healthy

        params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)

        if "toggle" in params:
            if healthy:
                healthy = False
            else:
                healthy = True

        if healthy:
            response = bytes("HEALTHY", "utf-8")
            code = 200
        else:
            response = bytes("UNHEALTHY", "utf-8")
            code = 500

        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("Usage: python server.py <port>")
        sys.exit(-1)

    port = int(sys.argv[1])

    server = ThreadingHTTPServer(('', port), Handler)
    server.serve_forever()
