# Protocol

The Core2 device is a tiny HTTP server on your LAN. The Mac-side hook is the
client. Everything is JSON over HTTP. One active request at a time (MVP).

Base URL: `http://agent-remote.local` (mDNS) or `http://<device-ip>`.

## `POST /request`

Push a new prompt to the device. The device stores it, wakes the screen,
chimes, and renders one button per option.

Request body:

```json
{
  "id": "b1f2...",           // unique id the hook generates (uuid4 hex)
  "title": "Bash",           // short header (e.g. the tool name)
  "summary": "git push origin main",   // the thing being approved
  "detail": "cwd: ~/proj",   // optional smaller-print context
  "options": ["Approve", "Deny"]        // 1–4 buttons; labels drive colors
}
```

Button colors are inferred from the label: approve/allow/yes/ok → green,
deny/block/no/reject → red, anything else → grey.

Response: `{"ok": true, "id": "b1f2..."}`

## `GET /status?id=<id>`

The hook polls this until the user taps.

- `{"state": "pending"}` — waiting for a tap
- `{"state": "answered", "choice": "Approve"}` — user tapped; `choice` is the exact option label
- `{"state": "none"}` — no matching active request (unknown/cleared id)

## `POST /clear`

Reset the device to idle. Response: `{"ok": true}`

## `GET /ping`

Health check: `{"ok": true, "device": "agent-remote", "state": "idle"}`

## Timeout behavior

If the hook polls past its timeout without an answer, it should `POST /clear`
and fall back to normal on-screen prompting on the Mac (fail-open), so a device
that's off or asleep never blocks you.
