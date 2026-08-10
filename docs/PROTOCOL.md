# Protocol

The Core2 device is a small HTTP server on your LAN. The Mac-side hook is the
client. Everything is JSON over HTTP. One active request at a time (MVP).

Base URL: `http://agent-remote.local` (mDNS) or `http://<device-ip>`.

## Auth

After pairing, `/request`, `/status`, and `/clear` require a header:

```
X-Agent-Token: <token>
```

The token is minted on the device during pairing and returned to the client
(see `/pair`). Missing/wrong token → `401 {"error":"unauthorized"}`. `/ping`,
`/pair`, `/pairstatus`, and the WiFi portal are unauthenticated.

## Pairing

Pairing is only possible while the device is in its pairing window (user taps
**Settings → Pair Agent**, 90-second window).

### `POST /pair`

```json
{ "client": "sals-mac-mini" }   // hostname shown on the device for confirmation
```

- If not in pairing mode → `403 {"error":"not in pairing mode"}`
- Else → `200 {"status":"pending"}` and the device shows an Approve/Deny confirm.

### `GET /pairstatus`

Poll after `/pair`:

- `{"status":"pending"}` — waiting for the user to tap on the device
- `{"status":"approved","token":"<64 hex>"}` — save this token
- `{"status":"denied"}` — user tapped Deny or the window expired
- `{"status":"waiting"|"closed"}` — pairing window open / not open

## `POST /request`  *(auth required)*

Push a new prompt. The device stores it, wakes the screen, chimes, and renders
one button per option.

```json
{
  "id": "b1f2...",                      // unique id (uuid4 hex)
  "title": "Bash",                      // short header (tool name)
  "summary": "git push origin main",    // the thing being approved
  "detail": "cwd: ~/proj",              // optional smaller-print context
  "options": ["Approve", "Deny"]        // 1–4 buttons; labels drive colors
}
```

Button colors are inferred from the label: approve/allow/yes/ok → green,
deny/block/no/reject → red, else grey.

Response: `{"ok": true, "id": "b1f2..."}`

## `GET /status?id=<id>`  *(auth required)*

The hook polls this until the user taps.

- `{"state": "pending"}`
- `{"state": "answered", "choice": "Approve"}` — `choice` is the exact label
- `{"state": "none"}` — no matching active request

## `POST /clear`  *(auth required)*

Reset to idle: `{"ok": true}`

## `GET /ping`

Health: `{"ok": true, "device": "agent-remote", "paired": true, "wifi": true}`

## WiFi provisioning

WiFi is configured **on the device** (Settings → WiFi Setup), which starts a
temporary SoftAP + captive portal:

- `GET /` (while in AP mode) → HTML form (network dropdown + password)
- `POST /wifisave` (form-encoded `ssid` / `ssid_manual` / `pass`) → saves to
  NVS, connects, and tears down the AP on success.

Credentials persist in NVS and auto-connect on boot.

## Timeout behavior

If the hook polls past its timeout without an answer, it `POST /clear`s and
fails open — falling back to normal on-screen prompting on the Mac — so a device
that's off or asleep never blocks you. Same for unreachable / 401.
