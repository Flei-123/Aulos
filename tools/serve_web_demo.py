#!/usr/bin/env python3
"""Static server for bindings/web/demo, serving from the repository root.

python's http.server does not know the .mjs extension, and a module script
delivered as application/octet-stream is rejected by the browser, so the map is
extended here. Deliberately no COOP/COEP headers: the point of this build is
that it does not need cross-origin isolation.

    python3 tools/serve_web_demo.py 8099
    -> http://localhost:8099/bindings/web/demo/
"""
import functools, http.server, os, socketserver, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".json": "application/json",
        ".wasm": "application/wasm",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % a))


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", PORT), functools.partial(Handler, directory=ROOT)) as httpd:
    print(f"serving {ROOT} on http://localhost:{PORT}/bindings/web/demo/")
    httpd.serve_forever()
