#!/usr/bin/env python3
"""Minimal local EST test adapter.

This is a test fixture, not a production EST implementation. It uses the
OpenSSL CLI to sign CSRs and returns a PKCS#7 certificate bundle. Authentication
mode is selected with EST_AUTH_MODE=0|1|2.
"""
import base64
import http.server
import os
import secrets
import shutil
import ssl
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import urlparse

HOST = "127.0.0.1"
PORT = int(os.environ.get("EST_PORT", "8443"))
AUTH_MODE = int(os.environ.get("EST_AUTH_MODE", "0"))
EST_USERNAME = os.environ.get("EST_USERNAME", "est-user")
EST_PASSWORD = os.environ.get("EST_PASSWORD", "est-password")
EST_BOOTSTRAP_TOKEN = os.environ.get("EST_BOOTSTRAP_TOKEN", "bootstrap-token")
ROOT = Path(tempfile.mkdtemp(prefix="fieldradio-est-"))
CA_KEY = ROOT / "ca.key"
CA_CERT = ROOT / "ca.crt"
SERVER_KEY = ROOT / "server.key"
SERVER_CSR = ROOT / "server.csr"
SERVER_CERT = ROOT / "server.crt"
CA_BUNDLE = ROOT / "ca-bundle.p7b"


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def setup():
    if AUTH_MODE not in (0, 1, 2):
        raise SystemExit("EST_AUTH_MODE must be 0, 1, or 2")
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
        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
        "-out", str(SERVER_CSR))
    run("openssl", "x509", "-req", "-in", str(SERVER_CSR), "-CA", str(CA_CERT),
        "-CAkey", str(CA_KEY), "-set_serial", "1", "-days", "365",
        "-copy_extensions", "copy",
        "-sha256", "-out", str(SERVER_CERT))
    run("openssl", "crl2pkcs7", "-nocrl", "-certfile", str(CA_CERT),
        "-outform", "DER", "-out", str(CA_BUNDLE))


def authorized(handler):
    if AUTH_MODE == 0:
        return True
    value = handler.headers.get("Authorization", "")
    if AUTH_MODE == 1:
        expected = "Basic " + base64.b64encode(
            f"{EST_USERNAME}:{EST_PASSWORD}".encode()
        ).decode()
        return secrets.compare_digest(value, expected)
    return secrets.compare_digest(value, "Bearer " + EST_BOOTSTRAP_TOKEN)


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, body, content_type="application/pkcs7-mime"):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth_or_unauthorized(self):
        if authorized(self):
            return True
        self.send_response(401)
        if AUTH_MODE == 1:
            self.send_header("WWW-Authenticate", 'Basic realm="EST"')
        elif AUTH_MODE == 2:
            self.send_header("WWW-Authenticate", "Bearer")
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def do_GET(self):
        if not self._auth_or_unauthorized():
            return
        path = urlparse(self.path).path
        if path.endswith("/cacerts"):
            self._send(CA_BUNDLE.read_bytes())
        elif path.endswith("/csrattrs"):
            self._send(b"0\r\n", "application/csrattrs")
        else:
            self.send_error(404)

    def do_POST(self):
        if not self._auth_or_unauthorized():
            return
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0"))
        csr = self.rfile.read(length)
        if not path.endswith("/simpleenroll") and not path.endswith("/simplereenroll"):
            self.send_error(404)
            return
        with tempfile.TemporaryDirectory(prefix="est-request-", dir=ROOT) as td:
            request_dir = Path(td)
            client_csr = request_dir / "client.csr"
            issued_cert = request_dir / "issued.crt"
            bundle = request_dir / "bundle.p7b"
            client_csr.write_bytes(csr)
            run("openssl", "x509", "-req", "-in", str(client_csr),
                "-CA", str(CA_CERT), "-CAkey", str(CA_KEY),
                "-set_serial", str(secrets.randbits(63) | 1),
                "-days", "30", "-sha256", "-out", str(issued_cert))
            run("openssl", "crl2pkcs7", "-nocrl", "-certfile", str(issued_cert),
                "-outform", "DER", "-out", str(bundle))
            self._send(bundle.read_bytes())

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    setup()
    try:
        server = http.server.ThreadingHTTPServer((HOST, PORT), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(SERVER_CERT, SERVER_KEY)
        if AUTH_MODE == 0:
            context.verify_mode = ssl.CERT_REQUIRED
            context.load_verify_locations(cafile=CA_CERT)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.serve_forever()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
