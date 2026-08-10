#!/usr/bin/env python3
"""
agent-remote — Claude Code PreToolUse hook.

Registered as a PreToolUse hook. For each gated tool call it pushes a prompt to
the Core2 device, waits for you to tap Approve/Deny, and returns that decision
to Claude Code. If the device is unreachable or you don't answer in time, it
fails OPEN (defers to Claude's normal on-screen prompt) so it never blocks you.

Config: bridge/agent-remote.config.json (see agent-remote.config.example.json).

Hook I/O reference: https://docs.claude.com/en/docs/claude-code/hooks
"""
import json
import os
import sys
import time
import uuid
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "agent-remote.config.json")

DEFAULTS = {
    # mDNS name from firmware/config.h, or a raw IP like "192.168.1.42".
    "device_host": "agent-remote.local",
    "port": 80,
    # Only these tools are sent to the device; everything else defers to
    # Claude's normal flow (so reads/searches never make you tap).
    "gate_tools": ["Bash", "Write", "Edit", "NotebookEdit", "WebFetch"],
    # How long to wait for a tap before failing open (seconds).
    "timeout_s": 120,
    # Poll interval while waiting (seconds).
    "poll_s": 0.5,
    # Button labels. First should map to allow, last to deny.
    "options": ["Approve", "Deny"],
    "allow_label": "Approve",
    "deny_label": "Deny",
}


def load_config():
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH) as f:
            cfg.update(json.load(f))
    except FileNotFoundError:
        pass
    except Exception as e:
        log(f"config error, using defaults: {e}")
    return cfg


def log(msg):
    # Hook stderr is surfaced in Claude Code's debug output, not to the model.
    print(f"[agent-remote] {msg}", file=sys.stderr)


def base_url(cfg):
    return f"http://{cfg['device_host']}:{cfg['port']}"


def http_json(url, payload=None, method="GET", timeout=4):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode() or "{}")


def summarize(tool_name, tool_input):
    """Build a short human-readable summary + detail from the tool input."""
    ti = tool_input or {}
    if tool_name == "Bash":
        return ti.get("command", ""), ti.get("description", "")
    if tool_name in ("Write", "Edit", "NotebookEdit"):
        return ti.get("file_path", "(file)"), tool_name
    if tool_name == "WebFetch":
        return ti.get("url", ""), "web fetch"
    # Generic fallback: first useful-looking field.
    for k in ("command", "path", "file_path", "url", "query", "prompt"):
        if k in ti:
            return str(ti[k])[:120], tool_name
    return tool_name, ""


def decide(permission, reason=""):
    """Emit the PreToolUse permission decision JSON and exit."""
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": permission,       # "allow" | "deny" | "ask"
            "permissionDecisionReason": reason,
        }
    }))
    sys.exit(0)


def defer():
    """No opinion — let Claude Code prompt normally on screen."""
    sys.exit(0)


def main():
    cfg = load_config()

    try:
        event = json.load(sys.stdin)
    except Exception:
        defer()

    tool_name = event.get("tool_name", "")
    tool_input = event.get("tool_input", {})

    if tool_name not in cfg["gate_tools"]:
        defer()

    summary, detail = summarize(tool_name, tool_input)
    cwd = event.get("cwd", "")
    if cwd:
        detail = (detail + "  " if detail else "") + f"cwd: {cwd}"

    req_id = uuid.uuid4().hex
    payload = {
        "id": req_id,
        "title": tool_name,
        "summary": summary[:160],
        "detail": detail[:160],
        "options": cfg["options"],
    }

    url = base_url(cfg)
    try:
        http_json(f"{url}/request", payload, method="POST")
    except (urllib.error.URLError, OSError) as e:
        log(f"device unreachable ({e}); deferring to normal prompt")
        defer()

    # Poll for the tap.
    deadline = time.time() + cfg["timeout_s"]
    choice = None
    while time.time() < deadline:
        try:
            status = http_json(f"{url}/status?id={req_id}")
        except (urllib.error.URLError, OSError):
            time.sleep(cfg["poll_s"])
            continue
        if status.get("state") == "answered":
            choice = status.get("choice")
            break
        time.sleep(cfg["poll_s"])

    if choice is None:
        log("no answer before timeout; clearing device and deferring")
        try:
            http_json(f"{url}/clear", {}, method="POST")
        except Exception:
            pass
        defer()

    if choice == cfg["deny_label"]:
        decide("deny", "Denied from agent-remote device")
    # Treat the allow label (or anything else tapped) as approval.
    decide("allow", f"Approved from agent-remote device ({choice})")


if __name__ == "__main__":
    main()
