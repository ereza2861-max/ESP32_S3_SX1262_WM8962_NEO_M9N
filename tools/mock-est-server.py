#!/usr/bin/env python3
"""Minimal local EST test adapter.

This is a test fixture, not a production EST implementation. It uses the
OpenSSL CLI to sign CSRs and returns a PKCS#7 certificate bundle.
"""
import http.server
import os
import shutil
import ssl
import subprocess
import tempfile
import ssl
from pathlib import Path
from urllib.parse import urlparse

HOST = "127.0.0.1"
PORT = int(os.environ.get("EST_PORT", "8443"))
ROOT = Path(tempfile.mkdtemp(prefix="fieldradio-est-"))
CA_KEY = ROOT / "ca.key"
CA_CERT = ROOT / "ca.crt"
SERVER_KEY = ROOT / "server.key"
SERVER_CSR = ROOT / "server.csr"
SERVER_CERT = ROOT / "server.crt"
CLIENT_CSR = ROOT / "client.csr"
ISSUED_CERT = ROOT / "issued.crt"
BUNDLE = ROOT / "bundle.p7b"
CA_BUNDLE = ROOT / "ca-bundle.p7b"


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def setup():
    if not shutil.which("openssl"):
        raise SystemExit("openssl is required")
    run("openssl", "genpkey", "-algorithm", "EC",
        "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(CA_KEY))
    run("openssl", "req", "-x509", "-new", "-key", str(CA_KEY),
        "-sha256", "-days", "3650", "-subj", "/CN=FieldRadio Test CA",
        "-out", str(CA_CERT))
    run("openssl", "genpkey", "-algorithm", "EC",
        "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(SERVER_KEY))
    run("openssl", "req", "-new", "-key", str(SERVER_KEY),
        "-subj", "/CN=127.0.0.1", "-out", str(SERVER_CSR))
    run("openssl", "x509", "-req", "-in", str(SERVER_CSR), "-CA", str(CA_CERT),
        "-CAkey", str(CA_KEY), "-CAcreateserial", "-days", "365",
        "-sha256", "-out", str(SERVER_CERT))
    run("openssl", "crl2pkcs7", "-nocrl", "-certfile", str(CA_CERT),
        "-outform", "DER", "-out", str(CA_BUNDLE))


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, body, content_type="application/pkcs7-mime"):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path.endswith("/cacerts"):
            self._send(CA_BUNDLE.read_bytes())
        elif path.endswith("/csrattrs"):
            self._send(b"0\r\n", "application/csrattrs")
        else:
            self.send_error(404)

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0"))
        csr = self.rfile.read(length)
        CLIENT_CSR.write_bytes(csr)
        if not path.endswith("/simpleenroll") and not path.endswith("/simplereenroll"):
            self.send_error(404)
            return
        run("openssl", "x509", "-req", "-in", str(CLIENT_CSR),
            "-CA", str(CA_CERT), "-CAkey", str(CA_KEY), "-CAcreateserial",
            "-days", "30", "-sha256", "-out", str(ISSUED_CERT))
        run("openssl", "crl2pkcs7", "-nocrl", "-certfile", str(ISSUED_CERT),
            "-outform", "DER", "-out", str(BUNDLE))
        self._send(BUNDLE.read_bytes())

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    setup()
    try:
        server = http.server.ThreadingHTTPServer((HOST, PORT), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(SERVER_CERT, SERVER_KEY)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.serve_forever()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
