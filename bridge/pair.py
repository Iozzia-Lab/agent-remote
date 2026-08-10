#!/usr/bin/env python3
"""
Pair this computer with the agent-remote device.

On the device: Settings > Pair Agent  (opens a 90-second pairing window).
Then run this. The device shows "Pair with <this computer>? Approve / Deny".
Tap Approve; this saves the returned token into agent-remote.config.json so
the hook can authenticate.

    python3 bridge/pair.py                     # host from config (mDNS default)
    python3 bridge/pair.py 192.168.1.42        # explicit host/ip
"""
import json
import os
import socket
import sys
import time
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "agent-remote.config.json")
EXAMPLE_PATH = os.path.join(HERE, "agent-remote.config.example.json")


def load_config():
    for p in (CONFIG_PATH, EXAMPLE_PATH):
        try:
            with open(p) as f:
                return json.load(f)
        except FileNotFoundError:
            continue
    return {"device_host": "agent-remote.local", "port": 80}


def save_token(cfg, token):
    cfg["token"] = token
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)
    print(f"saved token to {CONFIG_PATH}")


def call(base, path, payload=None, method="GET"):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(base + path, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode() or "{}")


def main():
    cfg = load_config()
    host = sys.argv[1] if len(sys.argv) > 1 else cfg.get("device_host", "agent-remote.local")
    base = f"http://{host}:{cfg.get('port', 80)}"
    client = socket.gethostname()

    print(f"Pairing with {base} as '{client}'")
    print("Make sure the device is in pairing mode (Settings > Pair Agent).")

    try:
        r = call(base, "/pair", {"client": client}, method="POST")
    except urllib.error.HTTPError as e:
        if e.code == 403:
            print("Device is NOT in pairing mode. On the device: Settings > Pair Agent, then rerun.")
        else:
            print(f"Device returned HTTP {e.code}.")
        sys.exit(1)
    except (urllib.error.URLError, OSError) as e:
        print(f"Could not reach the device ({e}). Same WiFi? Try the IP instead of mDNS.")
        sys.exit(1)

    if r.get("status") != "pending":
        print(f"Unexpected response: {r}")
        sys.exit(1)

    print("Look at the device — tap Approve to confirm pairing...")
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            s = call(base, "/pairstatus")
        except (urllib.error.URLError, OSError):
            time.sleep(0.5)
            continue
        st = s.get("status")
        if st == "approved":
            save_token(cfg, s["token"])
            print("Paired! You can now install the hook: python3 bridge/install.py")
            return
        if st == "denied":
            print("Pairing was denied on the device.")
            sys.exit(1)
        time.sleep(0.5)

    print("Timed out waiting for approval.")
    sys.exit(1)


if __name__ == "__main__":
    main()
