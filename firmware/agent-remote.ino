// agent-remote — M5Stack Core2 firmware
// A touchscreen "approval bell" for AI coding agents (Claude Code, etc.).
//
// The device runs a small HTTP server on your LAN. When an agent needs your
// input, the Mac-side hook POSTs a request here; the Core2 chimes and shows
// the prompt with tappable option buttons. When you tap, the choice is stored
// and the hook (which is polling) reads it back and returns it to the agent.
//
// Protocol (see docs/PROTOCOL.md):
//   POST /request  {id,title,summary,detail,options[]}  -> {"ok":true}
//   GET  /status?id=<id>                                 -> {"state","choice"}
//   POST /clear                                          -> {"ok":true}
//   GET  /ping                                           -> {"ok":true,...}
//
// Build: Arduino IDE or PlatformIO. Requires libraries:
//   M5Unified, ArduinoJson  (ESP32 core provides WiFi/WebServer/ESPmDNS)

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

#include "config.h"

WebServer server(HTTP_PORT);

// ---- Request state ----
enum State { IDLE, PENDING, ANSWERED };
State  state = IDLE;
String reqId;
String reqTitle;
String reqSummary;
String reqDetail;
String options[4];
int    optionCount = 0;
String choice;          // set when ANSWERED
uint32_t lastActivity = 0;
bool   dimmed = false;

// Button hit-boxes, computed at draw time.
struct Box { int x, y, w, h; };
Box optBox[4];

// ---- Colors ----
const uint16_t COL_BG      = 0x0000;   // black
const uint16_t COL_APPROVE = 0x2605;   // green
const uint16_t COL_DENY    = 0xC000;   // red
const uint16_t COL_NEUTRAL = 0x4208;   // grey
const uint16_t COL_TEXT    = 0xFFFF;   // white
const uint16_t COL_DIM     = 0x8410;   // dim grey

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------
uint16_t colorForOption(const String& label) {
  String l = label; l.toLowerCase();
  if (l.indexOf("approve") >= 0 || l.indexOf("allow") >= 0 ||
      l.indexOf("yes") >= 0 || l.indexOf("ok") >= 0)   return COL_APPROVE;
  if (l.indexOf("deny") >= 0 || l.indexOf("block") >= 0 ||
      l.indexOf("no") >= 0 || l.indexOf("reject") >= 0) return COL_DENY;
  return COL_NEUTRAL;
}

void drawStatusBar() {
  M5.Display.fillRect(0, 0, 320, 18, 0x1082);
  M5.Display.setTextColor(COL_DIM, 0x1082);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(4, 5);
  M5.Display.print(WiFi.isConnected() ? WiFi.localIP().toString() : "no wifi");
  int batt = M5.Power.getBatteryLevel();
  M5.Display.setCursor(280, 5);
  M5.Display.printf("%d%%", batt);
}

void drawIdle() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  M5.Display.setTextColor(COL_DIM, COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("waiting for agent...", 160, 120);
  M5.Display.setTextDatum(top_left);
}

void drawPending() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();

  // Title
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 26);
  M5.Display.print(reqTitle);

  // Summary (wraps naturally via println within width)
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(0xC618, COL_BG);
  M5.Display.setCursor(8, 58);
  M5.Display.println(reqSummary);

  // Detail (smaller, optional)
  if (reqDetail.length()) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(8, 100);
    M5.Display.println(reqDetail);
  }

  // Buttons along the bottom, split evenly.
  int n = optionCount > 0 ? optionCount : 1;
  int gap = 8;
  int totalW = 320 - gap * (n + 1);
  int w = totalW / n;
  int h = 70;
  int y = 240 - h - 10;
  for (int i = 0; i < n; i++) {
    int x = gap + i * (w + gap);
    optBox[i] = { x, y, w, h };
    M5.Display.fillRoundRect(x, y, w, h, 10, colorForOption(options[i]));
    M5.Display.setTextColor(COL_TEXT);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(options[i], x + w / 2, y + h / 2);
  }
  M5.Display.setTextDatum(top_left);
}

void drawAnswered() {
  M5.Display.fillScreen(COL_BG);
  drawStatusBar();
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("sent:", 160, 100);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(colorForOption(choice), COL_BG);
  M5.Display.drawString(choice, 160, 140);
  M5.Display.setTextDatum(top_left);
}

void redraw() {
  switch (state) {
    case IDLE:     drawIdle();     break;
    case PENDING:  drawPending();  break;
    case ANSWERED: drawAnswered(); break;
  }
}

void chime() {
  if (!ENABLE_CHIME) return;
  M5.Speaker.tone(880, 120);
  delay(140);
  M5.Speaker.tone(1320, 160);
}

void wake() {
  lastActivity = millis();
  if (dimmed) {
    M5.Display.setBrightness(200);
    dimmed = false;
  }
}

// -----------------------------------------------------------------------------
// HTTP handlers
// -----------------------------------------------------------------------------
void sendJson(int code, const String& body) {
  server.send(code, "application/json", body);
}

void handlePing() {
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["device"] = DEVICE_HOSTNAME;
  doc["state"] = state == IDLE ? "idle" : state == PENDING ? "pending" : "answered";
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

void handleRequest() {
  if (server.method() != HTTP_POST) { sendJson(405, "{\"error\":\"POST only\"}"); return; }
  String body = server.arg("plain");
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) { sendJson(400, "{\"error\":\"bad json\"}"); return; }

  reqId      = doc["id"]      | "";
  reqTitle   = doc["title"]   | "Agent request";
  reqSummary = doc["summary"] | "";
  reqDetail  = doc["detail"]  | "";
  optionCount = 0;
  JsonArray opts = doc["options"].as<JsonArray>();
  if (!opts.isNull()) {
    for (JsonVariant v : opts) {
      if (optionCount < 4) options[optionCount++] = v.as<String>();
    }
  }
  if (optionCount == 0) { options[0] = "Approve"; options[1] = "Deny"; optionCount = 2; }

  choice = "";
  state = PENDING;
  wake();
  redraw();
  chime();

  StaticJsonDocument<128> res;
  res["ok"] = true;
  res["id"] = reqId;
  String out; serializeJson(res, out);
  sendJson(200, out);
}

void handleStatus() {
  String id = server.arg("id");
  StaticJsonDocument<192> res;
  if (state == PENDING && (id == "" || id == reqId)) {
    res["state"] = "pending";
  } else if (state == ANSWERED && (id == "" || id == reqId)) {
    res["state"] = "answered";
    res["choice"] = choice;
  } else {
    res["state"] = "none";
  }
  String out; serializeJson(res, out);
  sendJson(200, out);
}

void handleClear() {
  state = IDLE;
  reqId = choice = "";
  redraw();
  sendJson(200, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// Touch
// -----------------------------------------------------------------------------
void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;
  wake();
  if (state != PENDING) return;
  for (int i = 0; i < optionCount; i++) {
    Box b = optBox[i];
    if (t.x >= b.x && t.x <= b.x + b.w && t.y >= b.y && t.y <= b.y + b.h) {
      choice = options[i];
      state = ANSWERED;
      M5.Speaker.tone(1560, 90);
      redraw();
      return;
    }
  }
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------
void connectWifi() {
  M5.Display.fillScreen(COL_BG);
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 100);
  M5.Display.print("connecting wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    M5.Display.print(".");
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(200);
  M5.Speaker.setVolume(120);

  connectWifi();

  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
  }

  server.on("/ping", handlePing);
  server.on("/request", handleRequest);
  server.on("/status", handleStatus);
  server.on("/clear", HTTP_POST, handleClear);
  server.begin();

  lastActivity = millis();
  state = IDLE;
  redraw();
}

void loop() {
  M5.update();
  server.handleClient();
  handleTouch();

  // Auto-dim after inactivity.
  if (SCREEN_DIM_MS > 0 && !dimmed && millis() - lastActivity > SCREEN_DIM_MS) {
    M5.Display.setBrightness(30);
    dimmed = true;
  }
  delay(5);
}
