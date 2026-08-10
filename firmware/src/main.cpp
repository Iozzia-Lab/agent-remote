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
static const IPAddress AP_IP(192, 168, 4, 1);

WebServer   server(80);
DNSServer   dns;
Preferences prefs;

// ---- colors (RGB565) ----
const uint16_t C_BLACK=0x0000, C_WHITE=0xFFFF, C_CYAN=0x07FF, C_YELLOW=0xFFE0,
               C_GREY=0x4208, C_LTGREY=0xC618, C_GREEN=0x2605, C_RED=0xC000;

// ---- state ----
enum Screen { SCR_START, SCR_AP, SCR_CONNECTED };
Screen screen = SCR_START;
bool   apActive = false;
bool   serverStarted = false;
String wifiSsid, wifiPass, connectedIP;

struct Box { int x, y, w, h; };
Box btnPrimary;   // the one button on the current screen

// forward decls
void centerText(const char* s, int cx, int cy, int size, uint16_t fg);
Box  drawButton(int x, int y, int w, int h, const char* label, uint16_t color);
void drawStart();
void drawAP();
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
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, C_BLACK);
  int w = M5.Display.textWidth(s);
  int h = M5.Display.fontHeight();
  M5.Display.setCursor(cx - w / 2, cy - h / 2);
  M5.Display.print(s);
}

Box drawButton(int x, int y, int w, int h, const char* label, uint16_t color) {
  M5.Display.fillRoundRect(x, y, w, h, 10, color);
  M5.Display.setTextColor(C_WHITE, color);
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
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(20, 78);
  M5.Display.print("This device needs WiFi.");
  M5.Display.setCursor(20, 96);
  M5.Display.print("Tap below, then follow the steps");
  M5.Display.setCursor(20, 112);
  M5.Display.print("on your phone.");
  btnPrimary = drawButton(50, 160, 220, 56, "Set up WiFi", C_GREEN);
}

void drawAP() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(18, 74);  M5.Display.print("1. On your phone, join WiFi:");
  M5.Display.setTextColor(C_YELLOW, C_BLACK); M5.Display.setTextSize(2);
  M5.Display.setCursor(28, 90);  M5.Display.print(AP_SSID);
  M5.Display.setTextSize(1); M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(18, 118); M5.Display.print("2. The setup page opens by itself.");
  M5.Display.setCursor(18, 134); M5.Display.print("   Or open in a browser:");
  M5.Display.setTextColor(C_CYAN, C_BLACK); M5.Display.setTextSize(2);
  M5.Display.setCursor(28, 150); M5.Display.print(PORTAL_HOST);
  btnPrimary = drawButton(90, 186, 140, 44, "Cancel", C_GREY);
}

void drawConnected() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  centerText("WiFi connected", 160, 96, 2, C_GREEN);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(20, 128); M5.Display.print("Network: "); M5.Display.print(wifiSsid);
  M5.Display.setCursor(20, 146); M5.Display.print("IP: "); M5.Display.print(connectedIP);
  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(20, 176); M5.Display.print("(pairing + server come next)");
  btnPrimary = drawButton(60, 196, 200, 36, "Re-do WiFi", C_GREY);
}

void redraw() {
  switch (screen) {
    case SCR_START:     drawStart();     break;
    case SCR_AP:        drawAP();        break;
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
      "<html><body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:40px'>"
      "<h2 style='color:#0a7'>Connected!</h2><p>IP: " + connectedIP +
      "</p><p>You can close this page. The device is now on your WiFi.</p></body></html>");
    // tear down the setup hotspot shortly, then show the connected screen
    delay(400);
    stopPortal();
    screen = SCR_CONNECTED;
    redraw();
  } else {
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:40px'>"
      "<h2 style='color:#c33'>Could not connect</h2><p>Check the password and try again.</p>"
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
void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;
  Box b = btnPrimary;
  bool hit = (t.x >= b.x && t.x <= b.x + b.w && t.y >= b.y && t.y <= b.y + b.h);
  if (!hit) return;
  M5.Speaker.tone(1200, 60);
  switch (screen) {
    case SCR_START:     startPortal(); break;
    case SCR_AP:        stopPortal(); screen = SCR_START; redraw(); break;
    case SCR_CONNECTED: startPortal(); break;   // "Re-do WiFi"
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
  }
  handleTouch();
  delay(5);
}
