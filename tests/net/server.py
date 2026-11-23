# a local http server for the network tests, it prints its port then serves until killed.
import http.server
import sys
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, status, body, headers=()):
        data = body.encode()
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/bytes/"):
            self.reply(200, "x" * int(self.path[7:]))
        elif self.path.startswith("/stream/"):
            # no content-length, the body ends with the connection.
            self.send_response(200)
            self.end_headers()
            try:
                for _ in range(int(self.path[8:]) // 1000):
                    self.wfile.write(b"x" * 1000)
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif self.path == "/huge":
            # announces far more than it sends then stalls, only a check of the headers refuses it in time.
            self.send_response(200)
            self.send_header("Content-Length", str(1 << 40))
            self.end_headers()
            self.wfile.write(b"x" * 1000)
            self.wfile.flush()
            time.sleep(3)
        elif self.path == "/hello":
            self.reply(200, "hello", [("Set-Cookie", "session=abc; Path=/"), ("Set-Cookie", "theme=dark; Path=/")])
        elif self.path == "/token-length":
            self.reply(200, str(len(self.headers.get("X-Token", ""))))
        elif self.path == "/redirect":
            self.reply(302, "", [("Location", "/hello")])
        elif self.path == "/redirect-ftp":
            self.reply(302, "", [("Location", "ftp://127.0.0.1/hello")])
        else:
            self.reply(404, "missing")

    def body(self):
        if self.headers.get("Transfer-Encoding", "").lower() != "chunked":
            return self.rfile.read(int(self.headers.get("Content-Length", 0)))
        data = b""
        while True:
            size = int(self.rfile.readline().split(b";")[0], 16)
            if size == 0:
                while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                    pass
                return data
            data += self.rfile.read(size)
            self.rfile.readline()

    def do_POST(self):
        self.reply(200, self.body().decode().upper())

    # checks an uploaded body made of the bytes `i % 251`, and how it was framed.
    def do_PUT(self):
        data = self.body()
        if self.path == "/redirect-upload":
            self.reply(307, "", [("Location", "/upload")])
            return
        framing = "chunked" if "Transfer-Encoding" in self.headers else "sized"
        bad = next((i for i, b in enumerate(data) if b != i % 251), None)
        self.reply(200, f"{framing} {len(data)}" if bad is None else f"bad at {bad}")

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    # clients stopping a transfer on purpose break their connection mid request.
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (OSError, ValueError)):
            super().handle_error(request, client_address)


server = Server(("127.0.0.1", 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
