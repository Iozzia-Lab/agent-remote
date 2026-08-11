// agent-remote — M5Stack Core2 firmware
//
// Step 2: start screen + on-device WiFi setup via a captive portal.
//   - Tap "Set up WiFi" -> device starts a hotspot "agent-remote-setup".
//   - Join it on your phone. A DNS server answers EVERY lookup with the device,
//     so the settings page auto-pops (like a "sign in to WiFi" screen) and
//     http://agent-remote.com also lands on it.
//   - Pick your network + password -> saved to flash (NVS), device connects.
//
// Next step (③): pairing + the Approve/Deny approval server.
//
// Build with PlatformIO:  pio run -t upload   (NOT arduino-cli — see platformio.ini)

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ---- config ----
static const char* AP_SSID      = "agent-remote-setup";
static const char* PORTAL_HOST  = "agent-remote.com";   // shown to the user; DNS maps it here
static const char* PORTAL_URL   = "http://agent-remote.com";   // encoded in the QR code
static const IPAddress AP_IP(192, 168, 4, 1);

WebServer   server(80);
DNSServer   dns;
Preferences prefs;

// ---- colors (RGB565) ----
const uint16_t C_BLACK=0x0000, C_WHITE=0xFFFF, C_CYAN=0x07FF, C_YELLOW=0xFFE0,
               C_GREY=0x4208, C_LTGREY=0xC618, C_GREEN=0x2605, C_RED=0xC000,
               C_DGREY=0x2104, C_DIM=0x8410;

// ---- state ----
enum Screen { SCR_START, SCR_AP, SCR_QR, SCR_CONNECTED };
Screen screen = SCR_START;
bool   apActive = false;
bool   apHasClient = false;      // a phone has joined the setup hotspot
bool   serverStarted = false;
String wifiSsid, wifiPass, connectedIP;

struct Box { int x, y, w, h; };
Box bSetup, bCancel, bNext, bBack, bRedo;   // per-screen buttons (only some set at a time)

// forward decls
void centerText(const char* s, int cx, int cy, int size, uint16_t fg);
void bodyText(int x, int y, const char* s, uint16_t fg);
void bodyTextCentered(int cx, int y, const char* s, uint16_t fg);
Box  drawButton(int x, int y, int w, int h, const char* label, uint16_t color, bool enabled = true);
void drawStart();
void drawAP();
void drawQR();
void drawConnected();
void redraw();
void startPortal();
void stopPortal();
bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs);
String portalHtml();
void handleRoot();
void handleSave();
void handleCaptive();
void handleTouch();

// =====================================================================
// Drawing
// =====================================================================
void centerText(const char* s, int cx, int cy, int size, uint16_t fg) {
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, C_BLACK);
  int w = M5.Display.textWidth(s);
  int h = M5.Display.fontHeight();
  M5.Display.setCursor(cx - w / 2, cy - h / 2);
  M5.Display.print(s);
}

// Slightly-larger, nicer proportional font for helper/body text (~13px tall).
// NOTE: reset size to 1 up front — a prior title/button draw may have left it
// at 2 or 3, which would scale this font into giant blobs.
void bodyText(int x, int y, const char* s, uint16_t fg) {
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(fg, C_BLACK);
  M5.Display.setCursor(x, y);
  M5.Display.print(s);
  M5.Display.setFont(&fonts::Font0);
}

void bodyTextCentered(int cx, int y, const char* s, uint16_t fg) {
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(fg, C_BLACK);
  int w = M5.Display.textWidth(s);
  M5.Display.setCursor(cx - w / 2, y);
  M5.Display.print(s);
  M5.Display.setFont(&fonts::Font0);
}

Box drawButton(int x, int y, int w, int h, const char* label, uint16_t color, bool enabled) {
  M5.Display.fillRoundRect(x, y, w, h, 10, color);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(enabled ? C_WHITE : C_DIM, color);
  M5.Display.setTextSize(2);
  int tw = M5.Display.textWidth(label);
  int th = M5.Display.fontHeight();
  M5.Display.setCursor(x + w / 2 - tw / 2, y + h / 2 - th / 2);
  M5.Display.print(label);
  return { x, y, w, h };
}

void titleBar() {
  M5.Display.drawRect(0, 0, 320, 240, C_WHITE);
  M5.Display.drawRect(8, 8, 304, 50, C_CYAN);
  centerText("Agent-remote", 160, 33, 3, C_WHITE);
}

void drawStart() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  bodyText(20, 76,  "This device needs WiFi.", C_LTGREY);
  bodyText(20, 100, "Tap below, then set it up", C_LTGREY);
  bodyText(20, 122, "on your phone.", C_LTGREY);
  bSetup = drawButton(50, 160, 220, 56, "Set up WiFi", C_GREEN);
}

// Step 1: join the hotspot, then tap Next (enabled once a phone has joined).
void drawAP() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  bodyText(18, 72, "1. On your phone, join WiFi:", C_LTGREY);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(C_YELLOW, C_BLACK); M5.Display.setTextSize(2);
  M5.Display.setCursor(28, 96); M5.Display.print(AP_SSID);
  M5.Display.setTextSize(1);
  if (apHasClient) bodyText(18, 132, "2. Connected! Tap Next.", C_GREEN);
  else             bodyText(18, 132, "2. Connect above, then Next.", C_LTGREY);
  bCancel = drawButton(20, 176, 130, 48, "Cancel", C_GREY);
  bNext   = drawButton(170, 176, 130, 48, "Next",
                       apHasClient ? C_GREEN : C_DGREY, apHasClient);
}

// Step 2: scan the QR to open the setup page.
void drawQR() {
  M5.Display.fillScreen(C_BLACK);
  centerText("Scan to set up WiFi", 160, 16, 2, C_CYAN);
  // M5GFX draws the QR on its own white background. 130px, centered.
  M5.Display.qrcode(PORTAL_URL, (320 - 130) / 2, 32, 130, 3);
  bodyTextCentered(160, 172, "or open  agent-remote.com", C_LTGREY);
  bBack   = drawButton(20, 194, 130, 40, "Back", C_GREY);
  bCancel = drawButton(170, 194, 130, 40, "Cancel", C_GREY);
}

void drawConnected() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  centerText("WiFi connected", 160, 90, 2, C_GREEN);
  bodyText(20, 116, ("Network: " + wifiSsid).c_str(), C_LTGREY);
  bodyText(20, 140, ("IP: " + connectedIP).c_str(), C_LTGREY);
  bodyText(20, 170, "(pairing + server come next)", C_GREY);
  bRedo = drawButton(60, 196, 200, 36, "Re-do WiFi", C_GREY);
}

void redraw() {
  switch (screen) {
    case SCR_START:     drawStart();     break;
    case SCR_AP:        drawAP();        break;
    case SCR_QR:        drawQR();        break;
    case SCR_CONNECTED: drawConnected(); break;
  }
}

// =====================================================================
// WiFi + captive portal
// =====================================================================
bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(150);
  return WiFi.isConnected();
}

String portalHtml() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 25; i++) {
    String ss = WiFi.SSID(i);
    ss.replace("\"", "");
    if (ss.length()) opts += "<option>" + ss + "</option>";
  }
  String h =
    "<!doctype html><html><head><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>agent-remote setup</title>"
    "<style>body{font-family:-apple-system,sans-serif;max-width:460px;margin:0 auto;"
    "padding:24px 16px;background:#111;color:#eee;font-size:20px}"
    "h2{text-align:center;color:#0cf;font-size:30px}"
    "label{display:block;margin:18px 0 6px;font-size:20px;color:#bbb}"
    "input,select,button{width:100%;padding:16px;font-size:22px;box-sizing:border-box;"
    "border-radius:8px;border:1px solid #333;background:#1c1c1c;color:#eee}"
    "button{background:#0a7;color:#fff;border:0;margin-top:22px;font-weight:600}"
    "</style></head><body><h2>Agent-remote WiFi</h2>"
    "<form method='POST' action='/save'>"
    "<label>Network</label><select name='ssid'>" + opts + "</select>"
    "<label>or type a hidden network name</label>"
    "<input name='ssid_manual' placeholder='(optional)'>"
    "<label>Password</label>"
    "<input name='pass' type='text' autocomplete='off' autocapitalize='none' "
    "autocorrect='off' spellcheck='false'>"
    "<button type='submit'>Connect</button></form>"
    "<p style='text-align:center;color:#888;font-size:15px;margin-top:22px'>"
    "agent-remote.com</p></body></html>";
  return h;
}

void handleRoot() { server.send(200, "text/html", portalHtml()); }

void handleSave() {
  String ssid = server.arg("ssid_manual");
  if (ssid.isEmpty()) ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.isEmpty()) { server.send(400, "text/html", "SSID required. <a href='/'>back</a>"); return; }

  bool ok = tryConnect(ssid, pass, 12000);
  if (ok) {
    wifiSsid = ssid; wifiPass = pass;
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    connectedIP = WiFi.localIP().toString();
    server.send(200, "text/html",
      "<!doctype html><html><head><meta name=viewport "
      "content='width=device-width,initial-scale=1'></head>"
      "<body style='font-family:-apple-system,sans-serif;background:#111;color:#eee;"
      "text-align:center;padding:40px 20px;font-size:22px'>"
      "<h2 style='color:#0a7;font-size:34px'>Connected!</h2><p>IP: " + connectedIP +
      "</p><p>You can close this page. The device is now on your WiFi.</p></body></html>");
    // tear down the setup hotspot shortly, then show the connected screen
    delay(400);
    stopPortal();
    screen = SCR_CONNECTED;
    redraw();
  } else {
    server.send(200, "text/html",
      "<!doctype html><html><head><meta name=viewport "
      "content='width=device-width,initial-scale=1'></head>"
      "<body style='font-family:-apple-system,sans-serif;background:#111;color:#eee;"
      "text-align:center;padding:40px 20px;font-size:22px'>"
      "<h2 style='color:#c33;font-size:34px'>Could not connect</h2>"
      "<p>Check the password and try again.</p>"
      "<a style='color:#0cf' href='/'>Back</a></body></html>");
  }
}

// Any other URL (incl. OS captive-portal probes) -> bounce to the portal so the
// "sign in to WiFi" sheet pops up automatically.
void handleCaptive() {
  server.sendHeader("Location", String("http://") + PORTAL_HOST + "/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", AP_IP);            // hijack every lookup -> this device
  if (!serverStarted) { server.begin(); serverStarted = true; }
  apActive = true;
  apHasClient = false;                  // Next stays disabled until a phone joins
  screen = SCR_AP;
  redraw();
  Serial.printf("[agent-remote] portal up: join '%s' -> http://%s/\n", AP_SSID, PORTAL_HOST);
}

void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
}

// =====================================================================
// Touch
// =====================================================================
static bool hitBox(const Box& b, int x, int y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;
  int x = t.x, y = t.y;
  auto tap = [&]() { M5.Speaker.tone(1200, 60); };
  switch (screen) {
    case SCR_START:
      if (hitBox(bSetup, x, y))  { tap(); startPortal(); }
      break;
    case SCR_AP:
      if (apHasClient && hitBox(bNext, x, y)) { tap(); screen = SCR_QR; redraw(); }
      else if (hitBox(bCancel, x, y)) { tap(); stopPortal(); screen = SCR_START; redraw(); }
      break;
    case SCR_QR:
      if (hitBox(bBack, x, y))   { tap(); screen = SCR_AP; redraw(); }
      else if (hitBox(bCancel, x, y)) { tap(); stopPortal(); screen = SCR_START; redraw(); }
      break;
    case SCR_CONNECTED:
      if (hitBox(bRedo, x, y))   { tap(); startPortal(); }
      break;
  }
}

// =====================================================================
// Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println("[agent-remote] setup start");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(180);
  M5.Speaker.setVolume(120);

  prefs.begin("agentremote", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");

  // Register portal routes now; the server is started when the portal opens.
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleCaptive);

  // If we already have saved WiFi, connect and show the connected screen.
  if (!wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    Serial.printf("[agent-remote] connecting to saved '%s'...\n", wifiSsid.c_str());
    if (tryConnect(wifiSsid, wifiPass, 12000)) {
      connectedIP = WiFi.localIP().toString();
      screen = SCR_CONNECTED;
    }
  }
  redraw();
  Serial.println("[agent-remote] setup done");
}

void loop() {
  M5.update();
  if (apActive) {
    dns.processNextRequest();
    server.handleClient();
    // Enable "Next" once a phone joins the hotspot; redraw on change.
    if (screen == SCR_AP) {
      bool now = WiFi.softAPgetStationNum() > 0;
      if (now != apHasClient) { apHasClient = now; redraw(); }
    }
  }
  handleTouch();
  delay(5);
}
