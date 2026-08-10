# agent-remote

**A physical Approve / Deny button for your AI coding agent.**

Step away from your computer and keep the session moving. When Claude Code (or
any agent with a pre-tool hook) needs your OK to run a command, an **M5Stack
Core2** on your desk chimes and shows the request on its touchscreen. Tap
**Approve** or **Deny** and the agent keeps going — no walking back to the
keyboard.

Think of it as an open-source take on the little "vibe-coder confirm" keyboards,
built on a cheap ESP32 device you probably already have.

```
┌─────────────┐   PreToolUse    ┌──────────────┐
│ Claude Code │ ──── hook ────► │ M5Stack Core2│   "Bash: git push origin main"
│  (your Mac) │ ◄── decision ── │  touchscreen │    [ Approve ]   [ Deny ]
└─────────────┘   over your LAN └──────────────┘
```

## How it works

1. The Core2 runs a tiny HTTP server on your WiFi (`agent-remote.local`).
2. A **PreToolUse hook** in Claude Code fires before a gated tool runs. It POSTs
   the request to the device and blocks, polling for your answer.
3. The device chimes, wakes the screen, and shows the prompt with tappable
   buttons.
4. You tap. The hook reads the choice and returns `allow` / `deny` to Claude
   Code, which continues.

If the device is off, asleep, or you don't answer in time, the hook **fails
open** — Claude just prompts on-screen as normal. It can never lock you out.

Everything stays on your LAN. No cloud, no broker, no account.

## Repo layout

| Path | What |
|------|------|
| `firmware/` | Core2 Arduino sketch (M5Unified). The touchscreen UI + HTTP server. |
| `bridge/`   | Python hook + installer that plug into Claude Code. |
| `docs/`     | `PROTOCOL.md` — the HTTP contract between hook and device. |

## Setup

### 1. Flash the Core2

Using **PlatformIO** (recommended):

```bash
cd firmware
cp config.example.h config.h      # fill in WiFi SSID/password
pio run -t upload && pio device monitor
```

Or **Arduino IDE**: open `firmware/agent-remote.ino`, install the **M5Unified**
and **ArduinoJson** libraries, select board **M5Core2**, copy `config.example.h`
to `config.h`, and upload.

On boot the screen shows the device IP and `waiting for agent...`. Note the IP
(or rely on `agent-remote.local` via Bonjour, which macOS supports natively).

### 2. Smoke-test the link

From your Mac, on the same WiFi:

```bash
python3 bridge/send-test.py agent-remote.local
# or:  python3 bridge/send-test.py 192.168.1.42
```

The device should chime and show a test prompt. Tap a button — the script prints
what you tapped. If this works, the hard part is done.

### 3. Install the Claude Code hook

```bash
python3 bridge/install.py
```

This merges the hook into `~/.claude/settings.json` (with a backup) and creates
`bridge/agent-remote.config.json`. Edit that file if you use a raw IP instead of
mDNS, or to change which tools require approval:

```json
{
  "device_host": "agent-remote.local",
  "gate_tools": ["Bash", "Write", "Edit", "NotebookEdit", "WebFetch"],
  "timeout_s": 120
}
```

Restart Claude Code. Now, whenever it wants to run one of the gated tools, your
Core2 lights up. Read-only tools (Read, Grep, etc.) are never gated, so you only
get pinged for things that actually matter.

To remove: `python3 bridge/install.py --remove`.

## Configuration notes

- **`gate_tools`** — only these tool names are sent to the device; everything
  else defers to Claude's normal flow. Start narrow (`Bash`) if you want fewer
  interruptions.
- **`timeout_s`** — how long the hook waits for a tap before failing open. Keep
  it a bit **below** the hook `timeout` in settings (default 150s) so the device
  fails open cleanly instead of Claude force-denying.
- **Button colors** are inferred from labels: approve/allow/yes → green,
  deny/block/no → red.

## Roadmap

- [ ] `Notification` hook → push "Claude is idle / needs input" to the device
- [ ] Multi-option prompts (e.g. Approve / Approve-all / Deny)
- [ ] Adapters for Codex CLI and Grok CLI approval flows
- [ ] On-device history of recent decisions
- [ ] Optional cloud/MQTT transport for off-network use

## License

See [LICENSE](LICENSE).
