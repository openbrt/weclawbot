#!/usr/bin/env python3

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description="Serve the local WeClawBot web console.")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    web_dir = Path(__file__).resolve().parent.parent / "web"
    handler = partial(NoCacheHandler, directory=str(web_dir))
    server = ThreadingHTTPServer(("", args.port), handler)
    print(f"WeClawBot console: http://localhost:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
