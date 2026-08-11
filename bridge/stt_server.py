#!/usr/bin/env python3
"""
agent-remote — local speech-to-text server.

Runs on your Mac (or any machine on the LAN). The Core2 records mic audio and
POSTs it here as a WAV; this transcribes it locally with faster-whisper and
returns {"text": "..."}. Audio never leaves your network — no API key, no cost.

Run:
    python3 bridge/stt_server.py            # model "base.en" on port 8766
    STT_MODEL=small.en STT_PORT=8766 python3 bridge/stt_server.py

First run downloads the model (~150 MB for base.en) from Hugging Face.

Endpoints:
    POST /transcribe   body = WAV audio  -> {"text": "..."}
    GET  /health                          -> {"ok": true, "model": "..."}
"""
import io
import json
import os
import tempfile
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_NAME = os.environ.get("STT_MODEL", "base.en")
PORT = int(os.environ.get("STT_PORT", "8766"))

_model = None


def get_model():
    global _model
    if _model is None:
        from faster_whisper import WhisperModel
        print(f"[stt] loading model '{MODEL_NAME}' (first run downloads it)...")
        _model = WhisperModel(MODEL_NAME, device="cpu", compute_type="int8")
        print("[stt] model ready")
    return _model


def transcribe_wav(data: bytes) -> str:
    # Write to a temp WAV so faster-whisper can decode it via ffmpeg/av.
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        # Quick sanity: log duration if it's a valid WAV.
        try:
            with wave.open(io.BytesIO(data), "rb") as w:
                dur = w.getnframes() / float(w.getframerate() or 1)
                print(f"[stt] {len(data)} bytes, {dur:.1f}s, {w.getframerate()}Hz")
        except Exception:
            print(f"[stt] {len(data)} bytes (non-standard WAV header)")
        segments, _info = get_model().transcribe(path, beam_size=1, language="en")
        text = " ".join(s.text.strip() for s in segments).strip()
        print(f"[stt] -> {text!r}")
        return text
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/health"):
            self._json(200, {"ok": True, "model": MODEL_NAME})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self.path.startswith("/transcribe"):
            self._json(404, {"error": "not found"})
            return
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n) if n else b""
        if not data:
            self._json(400, {"error": "empty body"})
            return
        try:
            text = transcribe_wav(data)
            self._json(200, {"text": text})
        except Exception as e:
            print(f"[stt] error: {e}")
            self._json(500, {"error": str(e)})

    def log_message(self, *a):
        pass  # quiet default access log


def main():
    print(f"[stt] agent-remote STT server on :{PORT}  (model {MODEL_NAME})")
    print("[stt] warming up the model...")
    get_model()
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
