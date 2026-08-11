#!/usr/bin/env python3
"""
agent-remote — local speech-to-text server (streaming).

Runs on your Mac (or any LAN machine). The Core2 opens a WebSocket, streams mic
audio (16 kHz mono PCM) continuously, and gets live partial transcripts back
(Vosk) so text appears as you speak. On "stop", the full audio is re-transcribed
once with Whisper for an accurate final transcript, which the device shows for
review before sending. All local — no API key, no cloud.

Run:
    python3 bridge/stt_server.py
Deps:
    pip install faster-whisper vosk websockets

Ports:
    ws://<host>:8766/        streaming: send binary PCM frames; send text "stop"
                             -> receives {"partial": "..."} live, then {"final": "..."}
    http://<host>:8767/health   {"ok": true}
"""
import asyncio
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
import websockets
from vosk import Model as VoskModel, KaldiRecognizer, SetLogLevel

WS_PORT = int(os.environ.get("STT_PORT", "8766"))
HTTP_PORT = WS_PORT + 1
WHISPER_MODEL = os.environ.get("STT_MODEL", "base.en")
SR = 16000

SetLogLevel(-1)
_vosk = None
_whisper = None


def get_vosk():
    global _vosk
    if _vosk is None:
        print("[stt] loading Vosk streaming model (auto-downloads ~40MB first run)...")
        _vosk = VoskModel(lang="en-us")
        print("[stt] Vosk ready")
    return _vosk


def get_whisper():
    global _whisper
    if _whisper is None:
        from faster_whisper import WhisperModel
        print(f"[stt] loading Whisper '{WHISPER_MODEL}' for final pass...")
        _whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
        print("[stt] Whisper ready")
    return _whisper


def whisper_final(pcm: bytes) -> str:
    arr = np.frombuffer(pcm, np.int16).astype(np.float32) / 32768.0
    if arr.size < SR // 4:          # < 0.25s of audio
        return ""
    segments, _ = get_whisper().transcribe(arr, beam_size=1, language="en")
    return " ".join(s.text.strip() for s in segments).strip()


async def ws_handler(ws):
    print("[stt] client connected")
    rec = KaldiRecognizer(get_vosk(), SR)
    audio = bytearray()
    finalized = ""
    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                audio += msg
                if rec.AcceptWaveform(bytes(msg)):
                    seg = json.loads(rec.Result()).get("text", "")
                    if seg:
                        finalized = (finalized + " " + seg).strip()
                    live = finalized
                else:
                    part = json.loads(rec.PartialResult()).get("partial", "")
                    live = (finalized + " " + part).strip()
                await ws.send(json.dumps({"partial": live}))
            else:  # text control frame
                if msg == "stop":
                    seg = json.loads(rec.FinalResult()).get("text", "")
                    if seg:
                        finalized = (finalized + " " + seg).strip()
                    print(f"[stt] stop: {len(audio)} bytes, vosk={finalized!r}")
                    text = whisper_final(bytes(audio)) or finalized
                    print(f"[stt] whisper final={text!r}")
                    await ws.send(json.dumps({"final": text}))
                    break
    except websockets.ConnectionClosed:
        pass
    print("[stt] client done")


class HealthHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps({"ok": True, "vosk": True, "whisper": WHISPER_MODEL}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def run_http():
    ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), HealthHandler).serve_forever()


async def main_async():
    async with websockets.serve(ws_handler, "0.0.0.0", WS_PORT, max_size=None):
        print(f"[stt] streaming WS on ws://0.0.0.0:{WS_PORT}/  | health http://0.0.0.0:{HTTP_PORT}/health")
        await asyncio.Future()


def main():
    print("[stt] warming up models...")
    get_vosk()
    get_whisper()
    threading.Thread(target=run_http, daemon=True).start()
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
