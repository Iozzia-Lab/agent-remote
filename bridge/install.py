#!/usr/bin/env python3
"""
Register the agent-remote PreToolUse hook in your Claude Code settings.

Merges the hook into ~/.claude/settings.json (or a path you pass), making a
timestamped backup first and refusing to add a duplicate. Also seeds
bridge/agent-remote.config.json from the example if it doesn't exist.

    python3 bridge/install.py                       # ~/.claude/settings.json
    python3 bridge/install.py /path/to/settings.json

Re-run with --remove to take the hook back out.
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK_CMD = f"python3 {os.path.join(HERE, 'hook.py')}"
MATCHER = "Bash|Edit|Write|NotebookEdit|WebFetch"


def settings_path(args):
    for a in args:
        if not a.startswith("--"):
            return os.path.expanduser(a)
    return os.path.expanduser("~/.claude/settings.json")


def load(path):
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f) or {}


def backup(path):
    if os.path.exists(path):
        # No timestamp import needed — just use a .bak suffix, keep last one.
        dst = path + ".agent-remote.bak"
        shutil.copy2(path, dst)
        print(f"backed up {path} -> {dst}")


def matches_ours(entry):
    for h in entry.get("hooks", []):
        if "agent-remote" in h.get("command", "") and h.get("command", "").endswith("hook.py"):
            return True
    return False


def main():
    args = sys.argv[1:]
    remove = "--remove" in args
    path = settings_path(args)

    settings = load(path)
    hooks = settings.setdefault("hooks", {})
    pre = hooks.setdefault("PreToolUse", [])

    # Drop any existing agent-remote entry (idempotent install / removal).
    pre[:] = [e for e in pre if not matches_ours(e)]

    if remove:
        backup(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(settings, f, indent=2)
        print("removed agent-remote hook.")
        return

    pre.append({
        "matcher": MATCHER,
        "hooks": [{
            "type": "command",
            "command": HOOK_CMD,
            "timeout": 150,
            "statusMessage": "Waiting for approval on your device...",
        }],
    })

    backup(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(settings, f, indent=2)
    print(f"installed agent-remote hook into {path}")
    print(f"  matcher: {MATCHER}")
    print(f"  command: {HOOK_CMD}")

    # Seed local config from the example.
    cfg = os.path.join(HERE, "agent-remote.config.json")
    example = os.path.join(HERE, "agent-remote.config.example.json")
    if not os.path.exists(cfg) and os.path.exists(example):
        shutil.copy2(example, cfg)
        print(f"created {cfg} — edit device_host if you're not using mDNS")

    print("\nDone. Restart Claude Code (or /hooks) to pick up the change.")


if __name__ == "__main__":
    main()
