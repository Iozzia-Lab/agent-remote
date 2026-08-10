// agent-remote — M5Stack Core2 firmware
// A touchscreen "approval bell" for AI coding agents (Claude Code, etc.).
//
// Screens:
//   HOME      status (wifi + paired) and a Settings button; shows requests here
//   REQUEST   the Approve/Deny prompt pushed by the agent
//   SETTINGS  WiFi Setup / Pair Agent / Unpair
//   WIFI      captive-portal provisioning (join "agent-remote-setup" from phone)
//   PAIR      pairing window + on-device confirm of the pairing computer
//
// Security: after pairing, every /request|/status|/clear must carry the header
//   X-Agent-Token: <token>   matching the token minted during pairing.
//   Unpaired or wrong token  -> 401. /ping is always open (health check).
//
// WiFi + token persist in NVS (Preferences), so setup survives reboots.
//
// Protocol: see docs/PROTOCOL.md
// Build: Arduino IDE (board "M5Core2") or PlatformIO. Libs: M5Unified, ArduinoJson.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_random.h"

#include "config.h"

// NOTE: the loop-task stack is enlarged to 16 KB in loop_stack.cpp — this app
// runs WiFi + WebServer + DNSServer + mDNS + JSON + M5GFX all in the loop task,
// which overflows the default 8 KB stack (Guru Meditation: stack canary).

WebServer  server(HTTP_PORT);
DNSServer  dns;
Preferences prefs;

const IPAddress AP_IP(192, 168, 4, 1);

// ---- Persisted config ----
String wifiSsid, wifiPass, token;
bool   paired = false;

// ---- Screens ----
enum Screen { SCR_HOME, SCR_REQUEST, SCR_SETTINGS, SCR_WIFI, SCR_PAIR };
Screen screen = SCR_HOME;

// ---- Request state ----
enum ReqState { REQ_IDLE, REQ_PENDING, REQ_ANSWERED };
ReqState reqState = REQ_IDLE;
String reqId, reqTitle, reqSummary, reqDetail, reqChoice;
String reqOptions[4];
int    reqOptionCount = 0;

// ---- Pairing state ----
bool   pairingMode = false;      // window open (device waiting for a computer)
uint32_t pairDeadline = 0;
enum PairState { PAIR_NONE, PAIR_PENDING, PAIR_APPROVED, PAIR_DENIED };
PairState pairState = PAIR_NONE;
String pairClient;               // hostname of the computer trying to pair
String pairToken;                // token handed back once approved

// ---- Server / WiFi portal state ----
bool   serverStarted = false;    // server.begin() only after a netif is up
bool   apMode = false;
String scanHtml;                 // cached <option> list
uint32_t exitApAt = 0;           // teardown time after a successful save

// ---- UI bookkeeping ----
uint32_t lastActivity = 0;
bool dimmed = false;

struct Box { int x, y, w, h; };
struct Btn { Box box; int id; };
Btn  btns[8];
int  btnCount = 0;

// Button ids
enum {
  BTN_SETTINGS = 1, BTN_BACK, BTN_WIFI, BTN_PAIR, BTN_UNPAIR,
  BTN_PAIR_APPROVE, BTN_PAIR_DENY,
  BTN_OPTION_BASE = 100      // request options: BASE + index
};

// ---- Colors ----
const uint16_t COL_BG      = 0x0000;
const uint16_t COL_APPROVE = 0x2605;
const uint16_t COL_DENY    = 0xC000;
const uint16_t COL_NEUTRAL = 0x4208;
const uint16_t COL_ACCENT  = 0x04FF;
const uint16_t COL_TEXT    = 0xFFFF;
const uint16_t COL_DIM     = 0x8410;
const uint16_t COL_BAR     = 0x1082;

// ---- Forward declarations (explicit, so we don't rely on the Arduino IDE's
//      auto-prototype generator, which mis-parses this sketch) ----
void drawCentered(const String& s, int cx, int cy, int size, uint16_t fg);
uint16_t colorForLabel(const String& label);
void resetBtns();
void addBtn(int id, const String& label, int x, int y, int w, int h, uint16_t color);
void drawStatusBar();
void drawHome();
void drawRequest();
void drawAnswered();
void drawSettings();
void drawWifi();
void drawPair();
void redraw();
void chime();
void wake();
bool connectSaved(uint32_t timeoutMs);
void startAp();
void stopAp();
void ensureServer();
void startMdns();
void sendJson(int code, const String& body);
bool authed();
String randomToken();
void handlePing();
void handleRequest();
void handleStatus();
void handleClear();
void handlePair();
void handlePairStatus();
void handleRoot();
void handleWifiSave();
void handleNotFound();
void enterPairing();
void handleButton(int id);
void handleTouch();

// =============================================================================
// Small drawing helpers
// =============================================================================
void drawCentered(const String& s, int cx, int cy, int size, uint16_t fg) {
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg);
  int w = M5.Display.textWidth(s);
  int h = M5.Display.fontHeight();
  M5.Display.setCursor(cx - w / 2, cy - h / 2);
  M5.Display.print(s);
}

uint16_t colorForLabel(const String& label) {
  String l = label; l.toLowerCase();
  if (l.indexOf("approve") >= 0 || l.indexOf("allow") >= 0 ||
      l.indexOf("yes") >= 0 || l.indexOf("ok") >= 0)     return COL_APPROVE;
  if (l.indexOf("deny") >= 0 || l.indexOf("block") >= 0 ||
      l.indexOf("no") >= 0 || l.indexOf("reject") >= 0)  return COL_DENY;
  return COL_NEUTRAL;
}

void resetBtns() { btnCount = 0; }

void addBtn(int id, const String& label, int x, int y, int w, int h, uint16_t color) {
  M5.Display.fillRoundRect(x, y, w, h, 10, color);
  drawCentered(label, x + w / 2, y + h / 2, 2, COL_TEXT);
  if (btnCount < 8) btns[btnCount++] = { { x, y, w, h }, id };
}

void drawStatusBar() {
  M5.Display.fillRect(0, 0, 320, 18, COL_BAR);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COL_DIM, COL_BAR);
  M5.Display.setCursor(4, 5);
  if (WiFi.isConnected()) M5.Display.print(WiFi.localIP().toString());
  else if (apMode)        M5.Display.print("setup mode");
  else                    M5.Display.print("offline");
  M5.Display.setCursor(200, 5);
  M5.Display.print(paired ? "paired" : "unpaired");
  M5.Display.setCursor(285, 5);
  M5.Display.printf("%d%%", M5.Power.getBatteryLevel());
}

// =============================================================================
// Screen renderers
// =============================================================================
void drawHome() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  if (!WiFi.isConnected()) {
    drawCentered("No WiFi", 160, 70, 3, COL_DENY);
    drawCentered("Open Settings to connect", 160, 105, 1, COL_DIM);
  } else if (!paired) {
    drawCentered("Not paired", 160, 70, 3, COL_ACCENT);
    drawCentered("Settings > Pair Agent", 160, 105, 1, COL_DIM);
  } else {
    drawCentered("waiting for agent...", 160, 90, 2, COL_DIM);
  }
  resetBtns();
  addBtn(BTN_SETTINGS, "Settings", 90, 175, 140, 50, COL_NEUTRAL);
}

void drawRequest() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 26);
  M5.Display.print(reqTitle);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(0xC618, COL_BG);
  M5.Display.setCursor(8, 58);
  M5.Display.println(reqSummary);
  if (reqDetail.length()) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(8, 100);
    M5.Display.println(reqDetail);
  }
  resetBtns();
  int n = reqOptionCount > 0 ? reqOptionCount : 1;
  int gap = 8, h = 70, y = 240 - h - 10;
  int w = (320 - gap * (n + 1)) / n;
  for (int i = 0; i < n; i++) {
    int x = gap + i * (w + gap);
    addBtn(BTN_OPTION_BASE + i, reqOptions[i], x, y, w, h, colorForLabel(reqOptions[i]));
  }
}

void drawAnswered() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  drawCentered("sent:", 160, 95, 2, COL_TEXT);
  drawCentered(reqChoice, 160, 135, 3, colorForLabel(reqChoice));
  resetBtns();
}

void drawSettings() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  drawCentered("Settings", 160, 34, 3, COL_TEXT);
  resetBtns();
  addBtn(BTN_WIFI, "WiFi Setup", 20, 60, 135, 50, COL_ACCENT);
  addBtn(paired ? BTN_UNPAIR : BTN_PAIR, paired ? "Unpair" : "Pair Agent",
         165, 60, 135, 50, paired ? COL_DENY : COL_NEUTRAL);
  addBtn(BTN_BACK, "Back", 90, 175, 140, 50, COL_NEUTRAL);
}

void drawWifi() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  drawCentered("WiFi Setup", 160, 30, 3, COL_ACCENT);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setCursor(10, 55);
  M5.Display.println("1. On your phone, join WiFi:");
  M5.Display.setTextColor(COL_ACCENT, COL_BG);
  M5.Display.setCursor(24, 70);   M5.Display.println(SETUP_AP_NAME);
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setCursor(10, 88);
  M5.Display.println("2. Open http://192.168.4.1");
  M5.Display.setCursor(10, 103);
  M5.Display.println("3. Pick your network + password");
  M5.Display.setTextColor(COL_DIM, COL_BG);
  M5.Display.setCursor(10, 128);
  M5.Display.println(WiFi.isConnected()
                     ? "Connected! You can go Back."
                     : "Waiting for network...");
  resetBtns();
  addBtn(BTN_BACK, "Back", 90, 175, 140, 50, COL_NEUTRAL);
}

void drawPair() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  drawCentered("Pairing", 160, 34, 3, COL_ACCENT);
  resetBtns();
  if (pairState == PAIR_PENDING) {
    drawCentered("Pair with:", 160, 72, 2, COL_TEXT);
    drawCentered(pairClient, 160, 100, 2, COL_ACCENT);
    addBtn(BTN_PAIR_APPROVE, "Approve", 20, 170, 135, 55, COL_APPROVE);
    addBtn(BTN_PAIR_DENY,    "Deny",    165, 170, 135, 55, COL_DENY);
  } else {
    int left = pairingMode ? (int)((pairDeadline - millis()) / 1000) : 0;
    drawCentered("Run pair.py on your", 160, 78, 2, COL_TEXT);
    drawCentered("computer now", 160, 104, 2, COL_TEXT);
    if (pairingMode) drawCentered(String(left) + "s", 160, 138, 2, COL_DIM);
    addBtn(BTN_BACK, "Cancel", 90, 175, 140, 50, COL_NEUTRAL);
  }
}

void redraw() {
  switch (screen) {
    case SCR_HOME:     drawHome();     break;
    case SCR_REQUEST:  (reqState == REQ_ANSWERED) ? drawAnswered() : drawRequest(); break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_WIFI:     drawWifi();     break;
    case SCR_PAIR:     drawPair();     break;
  }
}

void chime() {
  if (!ENABLE_CHIME) return;
  M5.Speaker.tone(880, 120); delay(140); M5.Speaker.tone(1320, 160);
}
void wake() {
  lastActivity = millis();
  if (dimmed) { M5.Display.setBrightness(200); dimmed = false; }
}

// =============================================================================
// WiFi
// =============================================================================
bool connectSaved(uint32_t timeoutMs) {
  if (wifiSsid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(200);
  if (WiFi.isConnected()) {
    startMdns();
    ensureServer();      // safe now: STA interface is up
    return true;
  }
  return false;
}

void startAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_AP_NAME);
  dns.start(53, "*", AP_IP);
  // Cache a scan for the portal dropdown.
  int n = WiFi.scanNetworks();
  scanHtml = "";
  for (int i = 0; i < n && i < 20; i++) {
    String ss = WiFi.SSID(i);
    ss.replace("\"", "");
    scanHtml += "<option value=\"" + ss + "\">" + ss + "</option>";
  }
  apMode = true;
  ensureServer();        // safe now: SoftAP interface is up (serves the portal)
}

void stopAp() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
}

// Start the HTTP server exactly once, and only after a network interface is up
// (STA connected or SoftAP started). Calling server.begin() with no live netif
// asserts inside lwip (queue.c: null mutex). Routes are registered in setup().
void ensureServer() {
  if (serverStarted) return;
  server.begin();
  serverStarted = true;
  Serial.println("[agent-remote] http server started");
}

void startMdns() {
  MDNS.end();
  if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", HTTP_PORT);
}

// =============================================================================
// HTTP: auth + helpers
// =============================================================================
void sendJson(int code, const String& body) { server.send(code, "application/json", body); }

bool authed() {
  if (!paired || token.isEmpty()) return false;
  return server.hasHeader("X-Agent-Token") && server.header("X-Agent-Token") == token;
}

String randomToken() {
  const char* hex = "0123456789abcdef";
  String t;
  for (int i = 0; i < 8; i++) {
    uint32_t r = esp_random();
    for (int b = 0; b < 8; b++) { t += hex[(r >> (b * 4)) & 0xF]; }
  }
  return t;   // 64 hex chars
}

// =============================================================================
// HTTP: agent endpoints
// =============================================================================
void handlePing() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["device"] = DEVICE_HOSTNAME;
  doc["paired"] = paired;
  doc["wifi"] = WiFi.isConnected();
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

void handleRequest() {
  if (server.method() != HTTP_POST) { sendJson(405, "{\"error\":\"POST only\"}"); return; }
  if (!authed()) { sendJson(401, "{\"error\":\"unauthorized\"}"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"error\":\"bad json\"}"); return; }

  reqId      = doc["id"]      | "";
  reqTitle   = doc["title"]   | "Agent request";
  reqSummary = doc["summary"] | "";
  reqDetail  = doc["detail"]  | "";
  reqOptionCount = 0;
  for (JsonVariant v : doc["options"].as<JsonArray>())
    if (reqOptionCount < 4) reqOptions[reqOptionCount++] = v.as<String>();
  if (reqOptionCount == 0) { reqOptions[0] = "Approve"; reqOptions[1] = "Deny"; reqOptionCount = 2; }

  reqChoice = "";
  reqState = REQ_PENDING;
  screen = SCR_REQUEST;
  wake(); redraw(); chime();

  sendJson(200, "{\"ok\":true,\"id\":\"" + reqId + "\"}");
}

void handleStatus() {
  if (!authed()) { sendJson(401, "{\"error\":\"unauthorized\"}"); return; }
  String id = server.arg("id");
  JsonDocument doc;
  if (reqState == REQ_PENDING && (id == "" || id == reqId)) {
    doc["state"] = "pending";
  } else if (reqState == REQ_ANSWERED && (id == "" || id == reqId)) {
    doc["state"] = "answered";
    doc["choice"] = reqChoice;
  } else {
    doc["state"] = "none";
  }
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

void handleClear() {
  if (!authed()) { sendJson(401, "{\"error\":\"unauthorized\"}"); return; }
  reqState = REQ_IDLE; reqId = ""; reqChoice = "";
  if (screen == SCR_REQUEST) { screen = SCR_HOME; redraw(); }
  sendJson(200, "{\"ok\":true}");
}

// =============================================================================
// HTTP: pairing
// =============================================================================
void handlePair() {
  if (server.method() != HTTP_POST) { sendJson(405, "{\"error\":\"POST only\"}"); return; }
  if (!pairingMode) { sendJson(403, "{\"error\":\"not in pairing mode\"}"); return; }
  if (pairState == PAIR_PENDING) { sendJson(409, "{\"error\":\"pairing busy\"}"); return; }

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  pairClient = doc["client"] | "a computer";
  pairState = PAIR_PENDING;
  screen = SCR_PAIR;
  wake(); redraw(); chime();
  sendJson(200, "{\"status\":\"pending\"}");
}

void handlePairStatus() {
  JsonDocument doc;
  if (pairState == PAIR_APPROVED) {
    doc["status"] = "approved";
    doc["token"] = pairToken;
  } else if (pairState == PAIR_DENIED) {
    doc["status"] = "denied";
  } else if (pairState == PAIR_PENDING) {
    doc["status"] = "pending";
  } else {
    doc["status"] = pairingMode ? "waiting" : "closed";
  }
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

// =============================================================================
// HTTP: WiFi captive portal
// =============================================================================
void handleRoot() {
  if (!apMode) { server.send(200, "text/plain", "agent-remote online"); return; }
  String page =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>agent-remote setup</title><style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
    "input,select,button{width:100%;padding:12px;margin:8px 0;font-size:16px;box-sizing:border-box}"
    "button{background:#0a7;color:#fff;border:0;border-radius:8px}h2{text-align:center}</style></head><body>"
    "<h2>agent-remote WiFi</h2><form method='POST' action='/wifisave'>"
    "<label>Network</label><select name='ssid'>" + scanHtml + "</select>"
    "<label>or type SSID</label><input name='ssid_manual' placeholder='(optional)'>"
    "<label>Password</label><input name='pass' type='password'>"
    "<button type='submit'>Connect</button></form></body></html>";
  server.send(200, "text/html", page);
}

void handleWifiSave() {
  String ssid = server.arg("ssid_manual");
  if (ssid.isEmpty()) ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.isEmpty()) { server.send(400, "text/html", "SSID required. <a href='/'>back</a>"); return; }

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  wifiSsid = ssid; wifiPass = pass;

  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) delay(200);

  if (WiFi.isConnected()) {
    String ip = WiFi.localIP().toString();
    server.send(200, "text/html",
      "<h2>Connected!</h2><p>IP: " + ip + "</p><p>This device is now on your WiFi. "
      "You can close this page.</p>");
    startMdns();
    ensureServer();               // already running from AP, but safe/idempotent
    exitApAt = millis() + 2000;   // tear down AP shortly, then go home
  } else {
    server.send(200, "text/html", "<h2>Could not connect.</h2><p><a href='/'>Try again</a></p>");
  }
}

void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// =============================================================================
// Touch dispatch
// =============================================================================
void enterPairing() {
  pairingMode = true;
  pairState = PAIR_NONE;
  pairDeadline = millis() + 90000;
  screen = SCR_PAIR;
  redraw();
}

void handleButton(int id) {
  M5.Speaker.tone(1560, 60);
  if (id >= BTN_OPTION_BASE) {                       // request option tapped
    int idx = id - BTN_OPTION_BASE;
    if (reqState == REQ_PENDING && idx < reqOptionCount) {
      reqChoice = reqOptions[idx];
      reqState = REQ_ANSWERED;
      redraw();
    }
    return;
  }
  switch (id) {
    case BTN_SETTINGS: screen = SCR_SETTINGS; redraw(); break;
    case BTN_BACK:
      if (screen == SCR_WIFI && apMode) stopAp();
      if (screen == SCR_PAIR) { pairingMode = false; pairState = PAIR_NONE; }
      screen = (screen == SCR_SETTINGS) ? SCR_HOME : SCR_SETTINGS;
      if (screen == SCR_SETTINGS && !pairingMode && !apMode) {} // stay
      screen = SCR_HOME;   // Back always returns home for simplicity
      redraw();
      break;
    case BTN_WIFI: screen = SCR_WIFI; redraw(); startAp(); redraw(); break;
    case BTN_PAIR: enterPairing(); break;
    case BTN_UNPAIR:
      paired = false; token = ""; prefs.remove("token");
      screen = SCR_SETTINGS; redraw();
      break;
    case BTN_PAIR_APPROVE:
      pairToken = randomToken();
      token = pairToken; paired = true;
      prefs.putString("token", token);
      pairState = PAIR_APPROVED;
      pairingMode = false;
      screen = SCR_HOME; redraw();
      break;
    case BTN_PAIR_DENY:
      pairState = PAIR_DENIED;
      pairingMode = false;
      screen = SCR_HOME; redraw();
      break;
  }
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;
  wake();
  for (int i = 0; i < btnCount; i++) {
    Box b = btns[i].box;
    if (t.x >= b.x && t.x <= b.x + b.w && t.y >= b.y && t.y <= b.y + b.h) {
      handleButton(btns[i].id);
      return;
    }
  }
}

// =============================================================================
// Setup / loop
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("[agent-remote] setup: start");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(200);
  M5.Speaker.setVolume(120);
  Serial.println("[agent-remote] M5 begun");

  prefs.begin("agentremote", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  token    = prefs.getString("token", "");
  paired   = !token.isEmpty();
  Serial.printf("[agent-remote] prefs: ssid='%s' paired=%d\n", wifiSsid.c_str(), paired); Serial.flush();

  // Register routes now, but DON'T begin() yet — the server is started later by
  // ensureServer(), once STA is connected or the SoftAP is up. Starting it with
  // no live network interface asserts inside lwip.
  const char* headers[] = { "X-Agent-Token" };
  server.collectHeaders(headers, 1);
  server.on("/ping", handlePing);
  server.on("/request", handleRequest);
  server.on("/status", handleStatus);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/pair", handlePair);
  server.on("/pairstatus", handlePairStatus);
  server.on("/", handleRoot);
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);

  M5.Display.fillScreen(COL_BG);
  drawCentered("agent-remote", 160, 110, 3, COL_TEXT);
  drawCentered(wifiSsid.isEmpty() ? "starting..." : "connecting wifi...", 160, 145, 1, COL_DIM);
  connectSaved(12000);   // starts server + mDNS if it connects
  Serial.printf("[agent-remote] wifi connected=%d ip=%s\n",
                WiFi.isConnected(), WiFi.localIP().toString().c_str());

  lastActivity = millis();
  screen = SCR_HOME;
  redraw();
  Serial.println("[agent-remote] setup: done");
}

void loop() {
  M5.update();
  if (serverStarted) server.handleClient();
  if (apMode) dns.processNextRequest();
  handleTouch();

  // AP teardown after a successful WiFi save.
  if (exitApAt && millis() > exitApAt) {
    exitApAt = 0;
    stopAp();
    screen = SCR_HOME;
    redraw();
  }

  // Pairing window countdown + live redraw of the timer.
  if (pairingMode && pairState != PAIR_PENDING) {
    static uint32_t lastTick = 0;
    if (millis() > pairDeadline) {
      pairingMode = false; pairState = PAIR_DENIED;
      screen = SCR_HOME; redraw();
    } else if (screen == SCR_PAIR && millis() - lastTick > 1000) {
      lastTick = millis();
      redraw();
    }
  }

  // Auto-dim.
  if (SCREEN_DIM_MS > 0 && !dimmed && millis() - lastActivity > SCREEN_DIM_MS) {
    M5.Display.setBrightness(30);
    dimmed = true;
  }
  delay(5);
}
