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
import fnmatch
import json
import os
import re
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
    # Pairing token (written by pair.py). Empty until you pair the device.
    "token": "",
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
    # Only engage the device for calls Claude would ACTUALLY prompt on: skip
    # anything your permission_mode or allow-rules already permit (they run
    # silently), and anything your deny-rules block (Claude handles those). Set
    # false to gate every tool in gate_tools regardless of your permissions.
    "respect_permissions": True,
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


def http_json(url, payload=None, method="GET", timeout=4, token=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Agent-Token", token)
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


def session_label(event):
    """A short human-readable label for this session, shown on the device so you
    can tell concurrent sessions apart. Fallback chain:
      1. AGENT_REMOTE env var  (you set it per session, e.g. `AGENT_REMOTE=... claude`)
      2. project folder name from cwd / CLAUDE_PROJECT_DIR
      3. short session_id tag
    Claude Code does NOT expose the session name/title to hooks, so the env var
    is the way to give a session your own name.
    """
    lbl = os.environ.get("AGENT_REMOTE", "").strip()
    if lbl:
        return lbl[:40]
    cwd = event.get("cwd") or os.environ.get("CLAUDE_PROJECT_DIR") or ""
    base = os.path.basename(cwd.rstrip("/")) if cwd else ""
    if base:
        return base[:40]
    sid = event.get("session_id", "")
    return ("#" + sid[:6]) if sid else "agent"


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


# --- Approximate Claude Code's permission matching so the device only fires for
#     calls Claude would actually PROMPT on (not ones your rules already allow). ---
def _parse_rule(rule):
    m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\((.*)\))?\s*$", rule or "")
    return (m.group(1), m.group(2)) if m else (None, None)   # (tool, spec-or-None)


def _bash_spec_matches(spec, cmd):
    spec, cmd = spec.strip(), (cmd or "").strip()
    if spec.endswith(":*"):                       # prefix rule, e.g. Bash(git commit:*)
        prefix = spec[:-2].strip()
        return cmd == prefix or cmd.startswith(prefix + " ")
    if "*" in spec:                               # glob rule, e.g. Bash(npm run *)
        return fnmatch.fnmatch(cmd, spec)
    return cmd == spec                            # exact


def rule_matches(rule, tool_name, tool_input):
    tool, spec = _parse_rule(rule)
    if tool != tool_name:
        return False
    if spec is None:
        return True                               # whole-tool rule, e.g. "Bash"
    ti = tool_input or {}
    if tool_name == "Bash":
        return _bash_spec_matches(spec, ti.get("command", ""))
    target = ti.get("file_path") or ti.get("path") or ti.get("url") or ""
    spec2 = spec.split(":", 1)[1] if spec.startswith("domain:") else spec
    return fnmatch.fnmatch(target, spec2) or fnmatch.fnmatch(target, "*" + spec2 + "*")


def load_permission_rules(cwd):
    """Merge allow/deny rules from user + project Claude settings."""
    paths = [os.path.expanduser("~/.claude/settings.json"),
             os.path.expanduser("~/.claude/settings.local.json")]
    if cwd:
        paths += [os.path.join(cwd, ".claude", "settings.json"),
                  os.path.join(cwd, ".claude", "settings.local.json")]
    allow, deny = [], []
    for p in paths:
        try:
            with open(p) as f:
                perms = (json.load(f) or {}).get("permissions", {}) or {}
            allow += perms.get("allow", []) or []
            deny += perms.get("deny", []) or []
        except Exception:
            pass
    return allow, deny


def already_permitted(event, tool_name, tool_input, cfg):
    """True if Claude would run this without prompting (so the device stays quiet)."""
    if not cfg.get("respect_permissions", True):
        return False
    mode = event.get("permission_mode", "default")
    if mode in ("bypassPermissions", "plan"):
        return True
    if mode == "acceptEdits" and tool_name in ("Write", "Edit", "MultiEdit", "NotebookEdit"):
        return True
    allow, deny = load_permission_rules(event.get("cwd", ""))
    # A deny match means Claude blocks it itself — no need to bother the device.
    if any(rule_matches(r, tool_name, tool_input) for r in deny):
        return True
    return any(rule_matches(r, tool_name, tool_input) for r in allow)


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

    # Only bother the device for calls Claude would actually stop and prompt on.
    if already_permitted(event, tool_name, tool_input, cfg):
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
        "context": session_label(event),
        "session": event.get("session_id", ""),   # keys the device's session tile
        "cloud": os.environ.get("CLAUDE_CODE_REMOTE") == "true",
    }

    url = base_url(cfg)
    tok = cfg.get("token") or None
    if not tok:
        log("no pairing token in config; run bridge/pair.py. Deferring.")
        defer()

    try:
        http_json(f"{url}/request", payload, method="POST", token=tok)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            log("device rejected token (401) — re-pair with bridge/pair.py. Deferring.")
        else:
            log(f"device error {e.code}; deferring")
        defer()
    except (urllib.error.URLError, OSError) as e:
        log(f"device unreachable ({e}); deferring to normal prompt")
        defer()

    # Poll for the tap.
    deadline = time.time() + cfg["timeout_s"]
    choice = None
    while time.time() < deadline:
        try:
            status = http_json(f"{url}/status?id={req_id}", token=tok)
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
            http_json(f"{url}/clear?id={req_id}", {}, method="POST", token=tok)
        except Exception:
            pass
        defer()

    # Answered — clear this session's tile from the dashboard, then decide.
    try:
        http_json(f"{url}/clear?id={req_id}", {}, method="POST", token=tok)
    except Exception:
        pass

    if choice == cfg["deny_label"]:
        decide("deny", "Denied from agent-remote device")
    # Treat the allow label (or anything else tapped) as approval.
    decide("allow", f"Approved from agent-remote device ({choice})")


if __name__ == "__main__":
    main()
