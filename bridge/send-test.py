#!/usr/bin/env python3
"""Manually push a test prompt to the device and wait for your tap.
Use this to verify the Core2 + LAN wiring before touching Claude Code.

    python3 bridge/send-test.py                 # default host from config
    python3 bridge/send-test.py 192.168.1.42     # explicit host/ip
"""
import json
import sys
import time
import uuid
import urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else "agent-remote.local"
base = f"http://{host}"


def call(path, payload=None, method="GET"):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(base + path, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode() or "{}")


print(f"ping -> {call('/ping')}")

rid = uuid.uuid4().hex
call("/request", {
    "id": rid,
    "title": "Bash",
    "summary": "git push origin main",
    "detail": "cwd: ~/01_DEV/iozzialabs/agent-remote",
    "options": ["Approve", "Deny"],
}, method="POST")
print("sent prompt — tap the device...")

while True:
    s = call(f"/status?id={rid}")
    if s.get("state") == "answered":
        print(f"you tapped: {s['choice']}")
        break
    time.sleep(0.5)
