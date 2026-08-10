#!/usr/bin/env python3
"""Manually push a test prompt to the device and wait for your tap.
Use this to verify the Core2 link after pairing.

    python3 bridge/send-test.py                 # host + token from config
    python3 bridge/send-test.py 192.168.1.42     # explicit host/ip

Requires a paired device: run bridge/pair.py first so a token is in
agent-remote.config.json (otherwise /request returns 401).
"""
import json
import os
import sys
import time
import uuid
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def load_config():
    for name in ("agent-remote.config.json", "agent-remote.config.example.json"):
        try:
            with open(os.path.join(HERE, name)) as f:
                return json.load(f)
        except FileNotFoundError:
            continue
    return {"device_host": "agent-remote.local", "port": 80, "token": ""}


cfg = load_config()
host = sys.argv[1] if len(sys.argv) > 1 else cfg.get("device_host", "agent-remote.local")
base = f"http://{host}:{cfg.get('port', 80)}"
token = cfg.get("token", "")


def call(path, payload=None, method="GET"):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(base + path, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Agent-Token", token)
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode() or "{}")


print(f"ping -> {call('/ping')}")
if not token:
    print("WARNING: no token in config — /request will 401. Run bridge/pair.py first.")

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
