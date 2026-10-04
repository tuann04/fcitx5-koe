#!/usr/bin/env python3
"""OpenAI-compatible transcription shim over sherpa-onnx websocket server.

Listens for POST /v1/audio/transcriptions (multipart WAV) and forwards the
audio to a sherpa-onnx offline websocket server, which does the actual
Parakeet inference. Replies with {"text": "..."}.

Stdlib only: no numpy, no pip packages. WAV decode uses the wave module,
the websocket client is a minimal ws:// implementation.

Usage:
  ./parakeet-shim.py [--host 127.0.0.1] [--port 8179]
                     [--upstream 127.0.0.1:6006]
"""

import argparse
import base64
import hashlib
import io
import json
import os
import socket
import struct
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = 10240
MAX_BODY = 64 * 1024 * 1024


def parse_multipart(body: bytes, boundary: bytes) -> dict:
    """Return {field_name: (filename_or_None, bytes)} for a multipart body."""
    parts = {}
    for chunk in body.split(b"--" + boundary):
        if b"\r\n\r\n" not in chunk:
            continue
        head, data = chunk.split(b"\r\n\r\n", 1)
        if data.endswith(b"\r\n"):
            data = data[:-2]
        if data == b"--" or not data:
            continue
        name = None
        filename = None
        for line in head.decode("latin1").split("\r\n"):
            low = line.lower()
            if low.startswith("content-disposition:"):
                for seg in line.split(";"):
                    seg = seg.strip()
                    if seg.startswith("name="):
                        name = seg[5:].strip('"')
                    elif seg.startswith("filename="):
                        filename = seg[9:].strip('"')
        if name:
            parts[name] = (filename, data)
    return parts


def wav_to_float32(raw: bytes) -> tuple:
    """Decode 16-bit PCM WAV mono/stereo to (float32 bytes, sample_rate)."""
    with wave.open(io.BytesIO(raw), "rb") as f:
        channels = f.getnchannels()
        width = f.getsampwidth()
        rate = f.getframerate()
        frames = f.readframes(f.getnframes())
    if width != 2:
        raise ValueError(f"only 16-bit WAV supported, got {width * 8}-bit")
    count = len(frames) // 2
    ints = struct.unpack("<%dh" % count, frames)
    if channels == 2:
        samples = [(a + b) / 2.0 / 32768.0
                   for a, b in zip(ints[0::2], ints[1::2])]
    else:
        samples = [v / 32768.0 for v in ints[::channels]]
    return struct.pack("<%df" % len(samples), *samples), rate


def ws_handshake(sock: socket.socket, host: str, port: int, key: str):
    req = (
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        more = sock.recv(4096)
        if not more:
            raise ConnectionError("handshake closed")
        resp += more
    if b" 101 " not in resp.split(b"\r\n", 1)[0]:
        raise ConnectionError(f"handshake failed: {resp[:80]!r}")
    accept = hashlib.sha1(
        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
    ).digest()
    if base64.b64encode(accept) not in resp:
        raise ConnectionError("bad accept key")


def ws_send(sock: socket.socket, payload: bytes, opcode: int = 0x2):
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    mask = os.urandom(4)
    sock.sendall(header + mask + bytes(b ^ mask[i % 4]
                                       for i, b in enumerate(payload)))


def ws_recv(sock: socket.socket) -> tuple:
    """Read one frame. Return (opcode, payload). Server replies are tiny."""
    data = b""
    while len(data) < 2:
        more = sock.recv(4096)
        if not more:
            raise ConnectionError("closed mid-frame")
        data += more
    first, second = data[0], data[1]
    opcode = first & 0x0F
    length = second & 0x7F
    at = 2
    if length == 126:
        while len(data) < 4:
            data += sock.recv(4096)
        length = struct.unpack(">H", data[2:4])[0]
        at = 4
    elif length == 127:
        while len(data) < 10:
            data += sock.recv(4096)
        length = struct.unpack(">Q", data[2:10])[0]
        at = 10
    if second & 0x80:
        while len(data) < at + 4:
            data += sock.recv(4096)
        mask = data[at:at + 4]
        at += 4
    else:
        mask = None
    while len(data) < at + length:
        more = sock.recv(65536)
        if not more:
            raise ConnectionError("closed mid-payload")
        data += more
    payload = data[at:at + length]
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    if opcode == 0x8:
        raise ConnectionError("server closed connection")
    return opcode, payload


def transcribe(upstream: str, wav_bytes: bytes) -> str:
    samples, rate = wav_to_float32(wav_bytes)
    host, _, port = upstream.partition(":")
    with socket.create_connection((host, int(port or 6006)), timeout=30) as s:
        s.settimeout(120)
        key = base64.b64encode(os.urandom(16)).decode()
        ws_handshake(s, host, port or "6006", key)
        # Same framing as the reference client: each chunk is its own
        # complete binary message, 8-byte header first.
        blob = struct.pack("<II", rate, len(samples)) + samples
        for i in range(0, len(blob), CHUNK):
            ws_send(s, blob[i:i + CHUNK])
        _, payload = ws_recv(s)
        text = payload.decode("utf-8", "replace")
        ws_send(s, b"Done", opcode=0x1)
    return "" if text == "<EMPTY>" else text


class Handler(BaseHTTPRequestHandler):
    upstream = "127.0.0.1:6006"
    server_version = "koe-parakeet-shim"

    def log_message(self, *args):
        pass

    def _json(self, code: int, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/health", "/v1/health"):
            self._json(200, {"status": "ok"})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path not in ("/v1/audio/transcriptions", "/inference"):
            self._json(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self._json(400, {"error": "bad content length"})
            return
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype or "boundary=" not in ctype:
            self._json(400, {"error": "want multipart/form-data"})
            return
        boundary = ctype.split("boundary=")[1].strip().strip('"').encode()
        try:
            parts = parse_multipart(body, boundary)
            if "file" not in parts:
                self._json(400, {"error": "missing file part"})
                return
            text = transcribe(self.upstream, parts["file"][1])
            self._json(200, {"text": text})
        except Exception as e:  # noqa - surfaced as HTTP 500
            self._json(500, {"error": str(e)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8179)
    parser.add_argument("--upstream", default="127.0.0.1:6006",
                        help="sherpa-onnx websocket server")
    args = parser.parse_args()
    Handler.upstream = args.upstream
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print(f"shim on {args.host}:{args.port} -> sherpa at {args.upstream}",
          flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
