# a local http server for the network tests, it prints its port then serves until killed.
import http.server
import sys


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
        if self.path == "/hello":
            self.reply(200, "hello", [("Set-Cookie", "session=abc; Path=/"), ("Set-Cookie", "theme=dark; Path=/")])
        elif self.path == "/redirect":
            self.reply(302, "", [("Location", "/hello")])
        else:
            self.reply(404, "missing")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        self.reply(200, body.upper())

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
