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
   the request (with your pairing token) to the device and blocks, polling for
   your answer.
3. The device chimes, wakes the screen, and shows the prompt with tappable
   buttons.
4. You tap. The hook reads the choice and returns `allow` / `deny` to Claude
   Code, which continues.

If the device is off, asleep, unreachable, or you don't answer in time, the hook
**fails open** — Claude just prompts on-screen as normal. It can never lock you
out.

Everything stays on your LAN. No cloud, no broker, no account.

**WiFi is set up on the device** (touchscreen → captive portal), so no
credentials live in the repo. **Pairing** mints a token that every request must
carry, so nobody else on the WiFi can drive your agent.

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
cp config.example.h config.h      # device name / UX only — no secrets
pio run -t upload && pio device monitor
```

Or **Arduino IDE**: open `firmware/agent-remote.ino`, install the **M5Unified**
and **ArduinoJson** libraries, select board **M5Core2**, copy `config.example.h`
to `config.h`, and upload.

Or **arduino-cli**:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install M5Unified ArduinoJson
cp firmware/config.example.h firmware/config.h
# sketch folder name must match the .ino, so build a matching copy:
mkdir -p /tmp/agent-remote && cp firmware/agent-remote.ino firmware/config.h /tmp/agent-remote/
arduino-cli compile --fqbn esp32:esp32:m5stack_core2 /tmp/agent-remote
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:m5stack_core2 /tmp/agent-remote
```

WiFi credentials are **not** in `config.h` — you set those on the device in
step 2 below.

On first boot the screen shows **agent-remote / No WiFi** and a **Settings**
button.

### 2. Connect the device to WiFi (on the device)

Tap **Settings → WiFi Setup**. The device starts a temporary hotspot:

1. On your phone, join WiFi **`agent-remote-setup`**
2. Open **http://192.168.4.1**
3. Pick your network (2.4 GHz) and enter the password

The device connects and remembers it across reboots. The status bar shows its
IP. macOS can also reach it at `agent-remote.local` (Bonjour).

### 3. Pair your computer with the device

On the device tap **Settings → Pair Agent** (opens a 90-second window), then on
your Mac:

```bash
python3 bridge/pair.py
# or:  python3 bridge/pair.py 192.168.1.42
```

The device shows **"Pair with `<your-mac>`? Approve / Deny"**. Tap **Approve**.
The token is saved to `bridge/agent-remote.config.json`.

### 4. Smoke-test the link

```bash
python3 bridge/send-test.py
```

The device should chime and show a test prompt. Tap a button — the script prints
what you tapped. If this works, the hard part is done.

### 5. Install the Claude Code hook

```bash
python3 bridge/install.py
```

This merges the hook into `~/.claude/settings.json` (with a backup). Edit
`bridge/agent-remote.config.json` to use a raw IP instead of mDNS, or to change
which tools require approval:

```json
{
  "device_host": "agent-remote.local",
  "token": "…set by pair.py…",
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

- [x] On-device WiFi setup (captive portal)
- [x] Pairing + per-request token auth
- [ ] `Notification` hook → push "Claude is idle / needs input" to the device
- [ ] Multi-option prompts (e.g. Approve / Approve-all / Deny)
- [ ] Adapters for Codex CLI and Grok CLI approval flows
- [ ] On-device history of recent decisions
- [ ] Optional cloud/MQTT transport for off-network use (walk-the-dog range)

## License

See [LICENSE](LICENSE).
