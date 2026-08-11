// agent-remote — M5Stack Core2 firmware
//
// Step 1: start screen.
// Step 2: on-device WiFi setup via captive portal (agent-remote.com + QR).
// Step 3a (this): approval server — POST /request shows an Approve/Deny prompt
//   on the touchscreen; GET /status returns the tap. (Pairing/token auth is 3b.)
//
// Build with PlatformIO:  pio run -t upload   (NOT arduino-cli — see platformio.ini)

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <esp_heap_caps.h>

// ---- config ----
static const char* AP_SSID      = "agent-remote-setup";
static const char* PORTAL_HOST  = "agent-remote.com";
static const char* PORTAL_URL   = "http://agent-remote.com";
static const char* MDNS_NAME    = "agent-remote";          // http://agent-remote.local
static const IPAddress AP_IP(192, 168, 4, 1);

// Voice answer: record on the built-in mic, POST WAV to the local STT server
// (bridge/stt_server.py) running on the machine that sent the request.
static const int      STT_PORT  = 8766;
static const uint32_t VOICE_SR  = 16000;
static const int      VOICE_MAX_SEC = 8;

WebServer   server(80);
DNSServer   dns;
Preferences prefs;

// ---- colors (RGB565) ----
const uint16_t C_BLACK=0x0000, C_WHITE=0xFFFF, C_CYAN=0x07FF, C_YELLOW=0xFFE0,
               C_GREY=0x4208, C_LTGREY=0xC618, C_GREEN=0x2605, C_RED=0xC000,
               C_DGREY=0x2104, C_DIM=0x8410;

// ---- screen state ----
enum Screen { SCR_START, SCR_AP, SCR_QR, SCR_CONNECTED, SCR_REQUEST, SCR_PAIR, SCR_VOICE };
Screen screen = SCR_START;

// Voice-answer flow (streaming speech-to-text)
enum VoiceState { V_LISTENING, V_TRANSCRIBING, V_RESULT, V_ERROR };
VoiceState voiceState = V_LISTENING;
int16_t*  voiceBuf = nullptr;  // one mic chunk
String    voiceTranscript;
String    sttHost;             // IP of the machine that sent the request (runs STT)
WebSocketsClient webSocket;
volatile bool wsConnected = false, wsDirty = false, wsGotFinal = false;
String    wsLiveText, wsFinalText;
// Non-blocking streaming: loop() drives the whole thing so touch/Stop/server stay
// alive and the mic can never hang the device (timeouts + a hard cap guard it).
bool      voiceActive = false;
bool      voiceChunkPending = false;
uint32_t  voiceConnectDeadline = 0, voiceMaxDeadline = 0, voiceFinalDeadline = 0;
uint32_t  voiceChunkStart = 0, voiceLastLive = 0;
String    voiceBaseText;        // text kept before an "Add more" recording appends to it
bool   apActive = false;
bool   apHasClient = false;
bool   serverStarted = false;
String wifiSsid, wifiPass, connectedIP;

// ---- request state ----
enum ReqState { REQ_NONE, REQ_PENDING, REQ_ANSWERED };
ReqState reqState = REQ_NONE;
String   reqId, reqTitle, reqSummary, reqChoice, reqContext;
String   reqOptions[4];         // up to 4 choices (approval or multiple-choice)
String   reqOptDesc[4];         // optional per-option description (scenario text)
int      reqOptionCount = 0;
bool     reqCloud = false;      // request came from a cloud/Cowork session
uint32_t answeredAt = 0;
// carousel mode: one option per screen (big targets + room for descriptions),
// browse with Prev/Next or swipe, then a Confirm step before committing.
bool     carouselMode = false;
int      carIdx = 0;
bool     carConfirm = false;
int      touchStartX = 0, touchStartY = 0;
int      descScroll = 0;        // vertical scroll offset of the description
int      descContentH = 0;      // measured full height of the description text
int      descRegionH = 90;      // visible height of the description area

// ---- pairing / auth state ----
String token;              // stored pairing token; empty = unpaired
bool   paired = false;
bool   pairingMode = false;
uint32_t pairDeadline = 0;
enum PairState { PAIR_IDLE, PAIR_PENDING, PAIR_APPROVED, PAIR_DENIED };
PairState pairState = PAIR_IDLE;
String pairClient, pairToken;

struct Box { int x, y, w, h; };
Box bSetup, bCancel, bNext, bBack, bRedo, bApprove, bDeny, bPair;
Box reqOptBox[4];               // hit-boxes for the request option buttons
Box bPrev, bNextC, bSelect, bConfirm;   // carousel controls
Box bMic, bVoiceStop, bVoiceRedo, bVoiceSend, bVoiceCancel, bVoiceDel, bVoiceAdd;   // voice-answer flow

// forward decls
void centerText(const char* s, int cx, int cy, int size, uint16_t fg);
void bodyText(int x, int y, const char* s, uint16_t fg);
void bodyTextCentered(int cx, int y, const char* s, uint16_t fg);
int  drawWrapped(const String& s, int x, int yTop, int w, int regionH, int scroll, uint16_t fg);
Box  drawButton(int x, int y, int w, int h, const char* label, uint16_t color, bool enabled = true);
void titleBar();
void drawStart(); void drawAP(); void drawQR(); void drawConnected(); void drawRequest(); void drawPair();
void drawCarousel(); void handleCarouselTap(int x, int y);
void drawVoice(); void drawMicButton(Box b); void drawLiveText();
void startVoiceStream(bool append = false); void voiceStreamStep(); void stopVoiceCapture(); void endVoiceCapture();
void wsEvent(WStype_t type, uint8_t* payload, size_t len);
bool hitBox(const Box& b, int x, int y);
void redraw();
bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs);
void ensureServer(); void startMdns(); void startPortal(); void stopPortal();
String portalHtml();
void handleRoot(); void handleSave(); void handleNotFound();
void handlePing(); void handleRequest(); void handleStatus(); void handleClear();
void handlePair(); void handlePairStatus();
bool authed(); String randomToken(); void enterPairing();
void handleTouch();

// =====================================================================
// Drawing helpers
// =====================================================================
void centerText(const char* s, int cx, int cy, int size, uint16_t fg) {
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, C_BLACK);
  int w = M5.Display.textWidth(s), h = M5.Display.fontHeight();
  M5.Display.setCursor(cx - w / 2, cy - h / 2);
  M5.Display.print(s);
}

// Proportional ~13px font for helper/body text. Reset size to 1 first (a prior
// title/button draw may have left it at 2-3, which would balloon this font).
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

// Word-wrap `s` into width `w` and draw it clipped to a region of height
// `regionH` starting at yTop, offset by `scroll`. Returns the full content
// height (so the caller can compute how far it scrolls).
int drawWrapped(const String& s, int x, int yTop, int w, int regionH, int scroll, uint16_t fg) {
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(fg, C_BLACK);
  int lh = M5.Display.fontHeight() + 3;
  M5.Display.setClipRect(x, yTop, w, regionH);
  int line = 0;
  int i = 0, n = s.length();
  String cur = "";
  auto flush = [&](const String& ln) {
    int yy = yTop + line * lh - scroll;
    if (yy + lh > yTop && yy < yTop + regionH) { M5.Display.setCursor(x, yy); M5.Display.print(ln); }
    line++;
  };
  while (i < n) {
    int j = i;
    while (j < n && s[j] != ' ' && s[j] != '\n') j++;
    String word = s.substring(i, j);
    bool nl = (j < n && s[j] == '\n');
    String trial = cur.length() ? cur + " " + word : word;
    if (M5.Display.textWidth(trial.c_str()) > w && cur.length()) { flush(cur); cur = word; }
    else cur = trial;
    if (nl) { flush(cur); cur = ""; }
    i = j + 1;
  }
  if (cur.length()) flush(cur);
  M5.Display.clearClipRect();
  M5.Display.setFont(&fonts::Font0);
  return line * lh;
}

Box drawButton(int x, int y, int w, int h, const char* label, uint16_t color, bool enabled) {
  M5.Display.fillRoundRect(x, y, w, h, 10, color);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(enabled ? C_WHITE : C_DIM, color);
  M5.Display.setTextSize(2);
  int tw = M5.Display.textWidth(label), th = M5.Display.fontHeight();
  M5.Display.setCursor(x + w / 2 - tw / 2, y + h / 2 - th / 2);
  M5.Display.print(label);
  return { x, y, w, h };
}

void titleBar() {
  M5.Display.drawRect(0, 0, 320, 240, C_WHITE);
  M5.Display.drawRect(8, 8, 304, 50, C_CYAN);
  centerText("Agent-remote", 160, 33, 3, C_WHITE);
}

// =====================================================================
// Screens
// =====================================================================
void drawStart() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  bodyText(20, 76,  "This device needs WiFi.", C_LTGREY);
  bodyText(20, 100, "Tap below, then set it up", C_LTGREY);
  bodyText(20, 122, "on your phone.", C_LTGREY);
  bSetup = drawButton(50, 160, 220, 56, "Set up WiFi", C_GREEN);
}

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
  bNext   = drawButton(170, 176, 130, 48, "Next", apHasClient ? C_GREEN : C_DGREY, apHasClient);
}

void drawQR() {
  M5.Display.fillScreen(C_BLACK);
  centerText("Scan to set up WiFi", 160, 16, 2, C_CYAN);
  M5.Display.qrcode(PORTAL_URL, (320 - 130) / 2, 32, 130, 3);
  bodyTextCentered(160, 172, "or open  agent-remote.com", C_LTGREY);
  bBack   = drawButton(20, 194, 130, 40, "Back", C_GREY);
  bCancel = drawButton(170, 194, 130, 40, "Cancel", C_GREY);
}

// Idle screen once on WiFi: waiting for an agent request. Shows the address
// and pairing status, with Pair + Re-do WiFi buttons.
void drawConnected() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  centerText("Waiting for agent", 160, 78, 2, C_CYAN);
  bodyTextCentered(160, 106, ("http://" + connectedIP).c_str(), C_LTGREY);
  if (paired) bodyTextCentered(160, 134, "Paired", C_GREEN);
  else        bodyTextCentered(160, 134, "Not paired - tap Pair", C_YELLOW);
  bPair = drawButton(20, 190, 135, 42, paired ? "Re-pair" : "Pair", paired ? C_GREY : C_GREEN);
  bRedo = drawButton(165, 190, 135, 42, "Re-do WiFi", C_GREY);
}

// Pairing window: run pair.py, then confirm the requesting computer here.
void drawPair() {
  M5.Display.fillScreen(C_BLACK);
  titleBar();
  if (pairState == PAIR_PENDING) {
    centerText("Pair with:", 160, 80, 2, C_WHITE);
    bodyTextCentered(160, 108, pairClient.c_str(), C_CYAN);
    bApprove = drawButton(14, 166, 140, 64, "Approve", C_GREEN);
    bDeny    = drawButton(166, 166, 140, 64, "Deny", C_RED);
  } else {
    bodyTextCentered(160, 82, "Run  pair.py  on your computer", C_LTGREY);
    int left = pairingMode ? (int)((pairDeadline - millis()) / 1000) : 0;
    centerText((String(left) + "s").c_str(), 160, 124, 2, C_DIM);
    bCancel = drawButton(90, 190, 140, 42, "Cancel", C_GREY);
  }
}

// Color a choice by its label: approve-ish green, deny-ish red, else neutral.
uint16_t optColor(const String& label) {
  String l = label; l.toLowerCase();
  if (l.indexOf("approve") >= 0 || l.indexOf("allow") >= 0 || l.indexOf("yes") >= 0 || l.indexOf("ok") >= 0)
    return C_GREEN;
  if (l.indexOf("deny") >= 0 || l.indexOf("reject") >= 0 || l.indexOf("block") >= 0 || l.indexOf("no") == 0)
    return C_RED;
  return 0x3D9F;   // neutral blue
}

// Approval / multiple-choice prompt (or the brief "Sent" confirmation).
void drawRequest() {
  M5.Display.fillScreen(C_BLACK);

  if (reqState != REQ_PENDING) {
    centerText("Sent", 160, 108, 2, C_LTGREY);
    centerText(reqChoice.c_str(), 160, 150, 3, optColor(reqChoice));
    return;
  }
  if (carouselMode) { drawCarousel(); return; }

  // Context label (top-left), cloud marker (top-right).
  String ctx = reqContext.length() ? reqContext : "agent";
  if (ctx.length() > 24) ctx = ctx.substring(0, 23) + "~";
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(2); M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setCursor(10, 8); M5.Display.print(ctx);
  if (reqCloud) {
    M5.Display.setTextSize(1); M5.Display.setTextColor(C_CYAN, C_BLACK);
    M5.Display.setCursor(278, 6); M5.Display.print("CLOUD");
  }

  // Tool / question header + the command / question text.
  M5.Display.setTextSize(2); M5.Display.setTextColor(C_CYAN, C_BLACK);
  M5.Display.setCursor(10, 34); M5.Display.print(reqTitle);
  bodyText(12, 62, reqSummary.c_str(), C_WHITE);   // wraps within screen width

  // Option buttons: 2 -> big side-by-side; 3-4 -> stacked full-width list.
  int n = reqOptionCount;
  if (n <= 2) {
    int y = 168, h = 64;
    reqOptBox[0] = drawButton(14, y, 140, h, reqOptions[0].c_str(), optColor(reqOptions[0]));
    if (n == 2) reqOptBox[1] = drawButton(166, y, 140, h, reqOptions[1].c_str(), optColor(reqOptions[1]));
  } else {
    int top = 104, gap = 6, h = (240 - top - 8 - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++)
      reqOptBox[i] = drawButton(14, top + i * (h + gap), 292, h, reqOptions[i].c_str(), optColor(reqOptions[i]));
  }
}

// Small microphone icon inside a button box.
void drawMicButton(Box b) {
  M5.Display.fillRoundRect(b.x, b.y, b.w, b.h, 5, 0x3D9F);
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2 - 1;
  M5.Display.fillRoundRect(cx - 3, cy - 7, 6, 11, 3, C_WHITE);   // mic body
  M5.Display.fillRect(cx - 1, cy + 4, 2, 4, C_WHITE);            // stem
  M5.Display.drawFastHLine(cx - 4, cy + 8, 8, C_WHITE);          // base
}

// Carousel: one option per screen (browse), then a Confirm step. The last item
// (index == reqOptionCount) is the "Voice answer" option. The "X of N" count
// lives inside the Select button to save vertical space.
void drawCarousel() {
  M5.Display.setFont(&fonts::Font0); M5.Display.setTextSize(1);
  bool isVoice = (carIdx == reqOptionCount);
  int  total   = reqOptionCount + 1;

  String ctx = reqContext.length() ? reqContext : "agent";
  if (ctx.length() > 16) ctx = ctx.substring(0, 15) + "~";
  M5.Display.setTextColor(C_YELLOW, C_BLACK); M5.Display.setCursor(10, 6); M5.Display.print(ctx);
  if (reqCloud) { M5.Display.setTextColor(C_CYAN, C_BLACK); M5.Display.setCursor(150, 6); M5.Display.print("CLOUD"); }

  int descTop;
  String dsc;
  if (!carConfirm) {
    // question (one line)
    M5.Display.setTextSize(1); M5.Display.setTextColor(C_CYAN, C_BLACK);
    M5.Display.setTextWrap(false); M5.Display.setCursor(10, 26); M5.Display.print(reqSummary);
    M5.Display.setTextWrap(true);
    // label (numbered option, or the Voice answer item with a mic)
    if (isVoice) {
      drawMicButton({ 10, 44, 34, 26 });
      M5.Display.setTextSize(2); M5.Display.setTextColor(C_WHITE, C_BLACK);
      M5.Display.setCursor(54, 50); M5.Display.print("Voice answer");
      dsc = "Speak your response instead of picking a listed option.";
      descTop = 82;
    } else {
      M5.Display.setTextSize(2); M5.Display.setTextColor(C_WHITE, C_BLACK);
      M5.Display.setCursor(10, 48); M5.Display.print(String(carIdx + 1) + ". " + reqOptions[carIdx]);
      dsc = reqOptDesc[carIdx];
      descTop = 74;
    }
    bPrev  = drawButton(8, 184, 54, 48, "<", carIdx > 0 ? C_GREY : C_DGREY, carIdx > 0);
    bNextC = drawButton(258, 184, 54, 48, ">", carIdx < reqOptionCount ? C_GREY : C_DGREY, carIdx < reqOptionCount);
    // Select button with the "X of N" count underneath.
    int sx = 70, sy = 184, sw = 180, sh = 48;
    uint16_t sc = isVoice ? 0x3D9F : C_GREEN;
    M5.Display.fillRoundRect(sx, sy, sw, sh, 10, sc);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(C_WHITE, sc); M5.Display.setTextSize(2);
    String mainL = isVoice ? "Speak" : "Select";
    M5.Display.setCursor(sx + sw / 2 - M5.Display.textWidth(mainL.c_str()) / 2, sy + 6);
    M5.Display.print(mainL);
    String cnt = String(carIdx + 1) + " of " + String(total);
    M5.Display.setFont(&fonts::FreeSans9pt7b); M5.Display.setTextSize(1);
    M5.Display.setTextColor(carIdx == total - 1 ? C_YELLOW : C_WHITE, sc);
    M5.Display.setCursor(sx + sw / 2 - M5.Display.textWidth(cnt.c_str()) / 2, sy + 28);
    M5.Display.print(cnt);
    M5.Display.setFont(&fonts::Font0);
    bSelect = { sx, sy, sw, sh };
  } else {
    centerText("Confirm choice", 160, 20, 2, C_CYAN);
    M5.Display.setFont(&fonts::Font0); M5.Display.setTextSize(2);
    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setCursor(10, 48); M5.Display.print(String(carIdx + 1) + ". " + reqOptions[carIdx]);
    dsc = reqOptDesc[carIdx];
    descTop = 76;
    bBack    = drawButton(14, 184, 140, 48, "Back", C_GREY);
    bConfirm = drawButton(166, 184, 140, 48, "Confirm", C_GREEN);
  }

  // Scrollable description in the band between the label and the buttons.
  descRegionH = 178 - descTop;
  if (dsc.length()) {
    descContentH = drawWrapped(dsc, 12, descTop, 292, descRegionH, descScroll, C_LTGREY);
    int maxS = descContentH - descRegionH;
    if (descScroll > 0)    M5.Display.fillTriangle(300, descTop + 8, 292, descTop + 8, 296, descTop, C_CYAN);
    if (maxS > 0 && descScroll < maxS) {
      int by = descTop + descRegionH - 2;
      M5.Display.fillTriangle(292, by - 8, 300, by - 8, 296, by, C_CYAN);
    }
  } else {
    descContentH = 0;
  }
}

// ---- Voice answer: stream mic -> live partials (Vosk) -> final (Whisper) ----
void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED)         wsConnected = true;
  else if (type == WStype_DISCONNECTED) wsConnected = false;
  else if (type == WStype_TEXT) {
    JsonDocument d;
    if (!deserializeJson(d, (const char*)payload, len)) {
      if (!d["partial"].isNull()) { wsLiveText = (const char*)(d["partial"] | ""); wsDirty = true; }
      else if (!d["final"].isNull()) { wsFinalText = (const char*)(d["final"] | ""); wsGotFinal = true; }
    }
  }
}

// Redraw only the live-text band during streaming (avoids full-screen flicker),
// auto-scrolled so the newest words stay visible.
void drawLiveText() {
  int top = 34, regionH = 176 - top;
  M5.Display.fillRect(2, top, 316, regionH, C_BLACK);
  String live = wsLiveText;
  String txt = voiceBaseText.length() ? (live.length() ? voiceBaseText + " " + live : voiceBaseText)
                                      : (live.length() ? live : "listening...");
  int ch = drawWrapped(txt, 12, top, 296, regionH, 0, C_WHITE);
  if (ch > regionH) {
    M5.Display.fillRect(2, top, 316, regionH, C_BLACK);
    drawWrapped(txt, 12, top, 296, regionH, ch - regionH, C_WHITE);
  }
}

void endVoiceCapture() {
  M5.Mic.end();          // aborts any in-flight record (no blocking wait)
  M5.Speaker.begin();
  voiceChunkPending = false;
}

// Stop capturing and ask the server for the accurate Whisper final.
void stopVoiceCapture() {
  endVoiceCapture();
  webSocket.sendTXT("stop");
  voiceState = V_TRANSCRIBING;
  voiceFinalDeadline = millis() + 20000;
  redraw();
}

// NON-BLOCKING: just set things up and return. loop() drives voiceStreamStep().
// append=true keeps the current transcript and appends the new dictation to it.
void startVoiceStream(bool append) {
  if (sttHost.isEmpty()) { voiceState = V_ERROR; voiceActive = false; screen = SCR_VOICE; redraw(); return; }
  voiceBaseText = append ? voiceTranscript : "";
  wsLiveText = ""; wsFinalText = ""; wsGotFinal = false; wsConnected = false; wsDirty = false;
  voiceChunkPending = false; descScroll = 0;
  voiceState = V_LISTENING; screen = SCR_VOICE; redraw();

  const size_t chunk = VOICE_SR / 10;   // 0.1s frames
  if (!voiceBuf) voiceBuf = (int16_t*)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  webSocket.begin(sttHost.c_str(), STT_PORT, "/");
  webSocket.onEvent(wsEvent);
  M5.Speaker.end();
  M5.Mic.begin();
  uint32_t now = millis();
  voiceConnectDeadline = now + 7000;
  voiceMaxDeadline     = now + 60000;   // hard cap: can never run forever
  voiceLastLive = 0;
  voiceActive = true;
}

// One non-blocking step, called every loop() while voiceActive. Never spins.
void voiceStreamStep() {
  webSocket.loop();
  if (wsDirty && millis() - voiceLastLive > 200) {
    wsDirty = false; voiceLastLive = millis();
    if (screen == SCR_VOICE && voiceState == V_LISTENING) drawLiveText();
  }
  if (voiceState == V_LISTENING) {
    if (!wsConnected) {
      if (millis() > voiceConnectDeadline) {
        endVoiceCapture(); webSocket.disconnect(); voiceState = V_ERROR; voiceActive = false; redraw();
      }
      return;
    }
    const size_t chunk = VOICE_SR / 10;
    if (!M5.Mic.isRecording()) {
      if (voiceChunkPending) { webSocket.sendBIN((uint8_t*)voiceBuf, chunk * 2); voiceChunkPending = false; }
      if (voiceBuf && M5.Mic.record(voiceBuf, chunk, VOICE_SR)) { voiceChunkPending = true; voiceChunkStart = millis(); }
    } else if (millis() - voiceChunkStart > 1000) {          // mic wedged -> bail safely
      endVoiceCapture(); webSocket.disconnect(); voiceState = V_ERROR; voiceActive = false; redraw();
      return;
    }
    if (millis() > voiceMaxDeadline) stopVoiceCapture();      // hard cap reached -> finalize
  } else if (voiceState == V_TRANSCRIBING) {
    if (wsGotFinal || millis() > voiceFinalDeadline) {
      webSocket.disconnect();
      String newText = (wsGotFinal && wsFinalText.length()) ? wsFinalText : wsLiveText;
      newText.trim();
      if (voiceBaseText.length())
        voiceTranscript = newText.length() ? voiceBaseText + " " + newText : voiceBaseText;  // append
      else
        voiceTranscript = newText;
      voiceTranscript.trim();
      if (!voiceTranscript.length()) { voiceState = V_ERROR; voiceActive = false; descScroll = 0; redraw(); return; }
      voiceState = V_RESULT; voiceActive = false; descScroll = 0; redraw();
    }
  } else {
    voiceActive = false;   // safety: nothing to do in other states
  }
}

void drawVoice() {
  M5.Display.fillScreen(C_BLACK);
  M5.Display.drawRect(0, 0, 320, 240, C_WHITE);
  if (voiceState == V_LISTENING) {
    M5.Display.setFont(&fonts::Font0); M5.Display.setTextSize(1); M5.Display.setTextColor(C_CYAN, C_BLACK);
    M5.Display.setCursor(12, 10); M5.Display.print("Listening... speak now");
    drawLiveText();
    bVoiceStop = drawButton(90, 184, 140, 48, "Stop", C_RED);
  } else if (voiceState == V_TRANSCRIBING) {
    centerText("Finalizing...", 160, 112, 3, C_CYAN);
  } else if (voiceState == V_RESULT) {
    M5.Display.setFont(&fonts::Font0); M5.Display.setTextSize(2);
    M5.Display.setTextColor(C_CYAN, C_BLACK); M5.Display.setCursor(12, 8); M5.Display.print("You said:");
    descRegionH = 176 - 38;
    String shown = voiceTranscript.length() ? voiceTranscript : "(empty - Add to dictate)";
    descContentH = drawWrapped(shown, 12, 38, 296, descRegionH, descScroll,
                               voiceTranscript.length() ? C_WHITE : C_DIM);
    int maxS = descContentH - descRegionH;
    if (descScroll > 0)                M5.Display.fillTriangle(302, 46, 294, 46, 298, 38, C_CYAN);
    if (maxS > 0 && descScroll < maxS) M5.Display.fillTriangle(294, 168, 302, 168, 298, 176, C_CYAN);
    // [Del] [Add] [Send]
    bVoiceDel = { 14, 184, 92, 48 };
    M5.Display.fillRoundRect(bVoiceDel.x, bVoiceDel.y, bVoiceDel.w, bVoiceDel.h, 10, 0x8800);  // muted red
    { int cx = bVoiceDel.x + bVoiceDel.w / 2, cy = bVoiceDel.y + bVoiceDel.h / 2;   // backspace glyph
      M5.Display.fillTriangle(cx - 16, cy, cx - 6, cy - 10, cx - 6, cy + 10, C_WHITE);
      M5.Display.fillRect(cx - 6, cy - 10, 22, 20, C_WHITE);
      M5.Display.drawLine(cx + 1, cy - 5, cx + 12, cy + 5, 0x8800);
      M5.Display.drawLine(cx + 12, cy - 5, cx + 1, cy + 5, 0x8800); }
    bVoiceAdd  = drawButton(114, 184, 92, 48, "Add", 0x3D9F);
    bVoiceSend = drawButton(214, 184, 92, 48, "Send", C_GREEN);
  } else {  // V_ERROR
    centerText("Could not transcribe", 160, 68, 2, C_RED);
    bodyTextCentered(160, 106, "Is the STT server running?", C_DIM);
    bVoiceRedo   = drawButton(14, 184, 140, 48, "Redo", C_GREY);
    bVoiceCancel = drawButton(166, 184, 140, 48, "Cancel", C_GREY);
  }
}

void handleCarouselTap(int x, int y) {
  if (carConfirm) {
    if (hitBox(bConfirm, x, y)) {
      M5.Speaker.tone(1200, 60);
      reqChoice = reqOptions[carIdx]; reqState = REQ_ANSWERED; answeredAt = millis();
      carConfirm = false; redraw();
    } else if (hitBox(bBack, x, y)) {
      M5.Speaker.tone(1200, 60); carConfirm = false; descScroll = 0; redraw();
    }
  } else {
    if (hitBox(bSelect, x, y)) {
      M5.Speaker.tone(1200, 60);
      if (carIdx == reqOptionCount) { startVoiceStream(); }   // Voice answer item
      else { carConfirm = true; descScroll = 0; redraw(); }
    }
    else if (hitBox(bPrev, x, y) && carIdx > 0) { M5.Speaker.tone(1200, 50); carIdx--; descScroll = 0; redraw(); }
    else if (hitBox(bNextC, x, y) && carIdx < reqOptionCount) { M5.Speaker.tone(1200, 50); carIdx++; descScroll = 0; redraw(); }
  }
}

void redraw() {
  switch (screen) {
    case SCR_START:     drawStart();     break;
    case SCR_AP:        drawAP();        break;
    case SCR_QR:        drawQR();        break;
    case SCR_CONNECTED: drawConnected(); break;
    case SCR_REQUEST:   drawRequest();   break;
    case SCR_PAIR:      drawPair();      break;
    case SCR_VOICE:     drawVoice();     break;
  }
}

// =====================================================================
// WiFi + server lifecycle
// =====================================================================
bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(150);
  return WiFi.isConnected();
}

void ensureServer() {
  if (serverStarted) return;
  server.begin();
  serverStarted = true;
  Serial.println("[agent-remote] http server started");
}

void startMdns() {
  MDNS.end();
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

// =====================================================================
// Captive portal (setup)
// =====================================================================
String portalHtml() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 25; i++) {
    String ss = WiFi.SSID(i); ss.replace("\"", "");
    if (ss.length()) opts += "<option>" + ss + "</option>";
  }
  return
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
}

void handleRoot() {
  if (apActive) server.send(200, "text/html", portalHtml());
  else          server.send(200, "text/plain", "agent-remote online. POST /request to prompt.");
}

void handleSave() {
  String ssid = server.arg("ssid_manual");
  if (ssid.isEmpty()) ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.isEmpty()) { server.send(400, "text/html", "SSID required. <a href='/'>back</a>"); return; }

  if (tryConnect(ssid, pass, 12000)) {
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
    delay(400);
    stopPortal();
    startMdns();
    ensureServer();          // keep serving on STA (for /request)
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

// OS captive probes / unknown URLs: bounce to the portal in AP mode, else 404.
void handleNotFound() {
  if (apActive) {
    server.sendHeader("Location", String("http://") + PORTAL_HOST + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", AP_IP);
  ensureServer();
  apActive = true;
  apHasClient = false;
  screen = SCR_AP;
  redraw();
  Serial.printf("[agent-remote] portal up: join '%s' -> %s\n", AP_SSID, PORTAL_URL);
}

void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
}

// =====================================================================
// Auth + agent endpoints
// =====================================================================
String randomToken() {
  const char* hex = "0123456789abcdef";
  String t;
  for (int i = 0; i < 8; i++) {
    uint32_t r = esp_random();
    for (int b = 0; b < 8; b++) t += hex[(r >> (b * 4)) & 0xF];
  }
  return t;   // 64 hex chars
}

bool authed() {
  if (!paired || token.isEmpty()) return false;
  return server.hasHeader("X-Agent-Token") && server.header("X-Agent-Token") == token;
}

void handlePing() {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  const char* st = reqState == REQ_PENDING ? "pending" : reqState == REQ_ANSWERED ? "answered" : "idle";
  server.send(200, "application/json",
              "{\"ok\":true,\"device\":\"agent-remote\",\"ip\":\"" + ip +
              "\",\"paired\":" + (paired ? "true" : "false") + ",\"state\":\"" + st + "\"}");
}

void handleRequest() {
  if (!authed()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  sttHost = server.client().remoteIP().toString();   // STT server runs on the requester
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  reqId      = (const char*)(doc["id"]      | "");
  reqTitle   = (const char*)(doc["title"]   | "Agent request");
  reqSummary = (const char*)(doc["summary"] | "");
  reqContext = (const char*)(doc["context"] | "");
  reqCloud   = doc["cloud"] | false;
  // options: array of strings, or objects {label, description}.
  reqOptionCount = 0;
  if (doc["options"].is<JsonArray>()) {
    for (JsonVariant v : doc["options"].as<JsonArray>()) {
      if (reqOptionCount >= 4) break;
      if (v.is<JsonObject>()) {
        reqOptions[reqOptionCount] = v["label"].as<String>();
        reqOptDesc[reqOptionCount] = (const char*)(v["description"] | "");
      } else {
        reqOptions[reqOptionCount] = v.as<String>();
        reqOptDesc[reqOptionCount] = "";
      }
      reqOptionCount++;
    }
  }
  if (reqOptionCount == 0) {
    reqOptions[0] = "Approve"; reqOptDesc[0] = "";
    reqOptions[1] = "Deny";    reqOptDesc[1] = "";
    reqOptionCount = 2;
  }
  // Use the carousel (browse + confirm) for 3+ options or any described option;
  // keep the fast two-button layout for simple Approve/Deny.
  carouselMode = reqOptionCount >= 3;
  for (int i = 0; i < reqOptionCount; i++) if (reqOptDesc[i].length()) carouselMode = true;
  carIdx = 0; carConfirm = false; descScroll = 0;
  reqChoice = "";
  reqState = REQ_PENDING;
  screen = SCR_REQUEST;
  redraw();
  M5.Speaker.tone(880, 120); delay(130); M5.Speaker.tone(1320, 160);
  server.send(200, "application/json", "{\"ok\":true,\"id\":\"" + reqId + "\"}");
}

void handleStatus() {
  if (!authed()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  String id = server.arg("id");
  String body;
  if (reqState == REQ_PENDING && (id == "" || id == reqId)) {
    body = "{\"state\":\"pending\"}";
  } else if (reqState == REQ_ANSWERED && (id == "" || id == reqId)) {
    JsonDocument d; d["state"] = "answered"; d["choice"] = reqChoice;   // safe-encodes quotes/newlines
    serializeJson(d, body);
  } else {
    body = "{\"state\":\"none\"}";
  }
  server.send(200, "application/json", body);
}

void handleClear() {
  if (!authed()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  reqState = REQ_NONE; reqId = ""; reqChoice = "";
  if (screen == SCR_REQUEST) { screen = SCR_CONNECTED; redraw(); }
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---- pairing endpoints (unauthenticated; only work during the pairing window) ----
void handlePair() {
  if (!pairingMode) { server.send(403, "application/json", "{\"error\":\"not in pairing mode\"}"); return; }
  if (pairState == PAIR_PENDING) { server.send(409, "application/json", "{\"error\":\"pairing busy\"}"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  pairClient = (const char*)(doc["client"] | "a computer");
  pairState = PAIR_PENDING;
  screen = SCR_PAIR;
  redraw();
  M5.Speaker.tone(880, 120); delay(130); M5.Speaker.tone(1320, 160);
  server.send(200, "application/json", "{\"status\":\"pending\"}");
}

void handlePairStatus() {
  String body;
  if (pairState == PAIR_APPROVED)      body = "{\"status\":\"approved\",\"token\":\"" + pairToken + "\"}";
  else if (pairState == PAIR_DENIED)   body = "{\"status\":\"denied\"}";
  else if (pairState == PAIR_PENDING)  body = "{\"status\":\"pending\"}";
  else                                 body = pairingMode ? "{\"status\":\"waiting\"}" : "{\"status\":\"closed\"}";
  server.send(200, "application/json", body);
}

void enterPairing() {
  pairingMode = true;
  pairState = PAIR_IDLE;
  pairDeadline = millis() + 90000;
  screen = SCR_PAIR;
  redraw();
}

// =====================================================================
// Touch
// =====================================================================
bool hitBox(const Box& b, int x, int y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) { touchStartX = t.x; touchStartY = t.y; }

  // Carousel handles on RELEASE so we can distinguish a tap from a swipe.
  if (screen == SCR_REQUEST && reqState == REQ_PENDING && carouselMode) {
    if (!t.wasReleased()) return;
    int dx = t.x - touchStartX, dy = t.y - touchStartY;
    bool horiz = abs(dx) > 50 && abs(dx) > abs(dy);
    bool vert  = abs(dy) > 28 && abs(dy) > abs(dx);
    if (!carConfirm && horiz) {                        // browse options
      if (dx < 0 && carIdx < reqOptionCount) { M5.Speaker.tone(1200, 50); carIdx++; descScroll = 0; redraw(); }
      else if (dx > 0 && carIdx > 0) { M5.Speaker.tone(1200, 50); carIdx--; descScroll = 0; redraw(); }
    } else if (vert) {                                 // scroll the description
      int maxS = descContentH - descRegionH; if (maxS < 0) maxS = 0;
      descScroll -= dy;
      if (descScroll < 0) descScroll = 0;
      if (descScroll > maxS) descScroll = maxS;
      redraw();
    } else {
      handleCarouselTap(t.x, t.y);
    }
    return;
  }

  // Voice: Stop while listening; scroll/Redo/Send while reviewing.
  if (screen == SCR_VOICE) {
    if (voiceState == V_LISTENING) {
      if (t.wasPressed() && hitBox(bVoiceStop, t.x, t.y)) stopVoiceCapture();
    } else if (voiceState == V_RESULT) {
      if (!t.wasReleased()) return;
      int dx = t.x - touchStartX, dy = t.y - touchStartY;
      if (abs(dy) > 28 && abs(dy) > abs(dx)) {
        int maxS = descContentH - descRegionH; if (maxS < 0) maxS = 0;
        descScroll -= dy; if (descScroll < 0) descScroll = 0; if (descScroll > maxS) descScroll = maxS;
        redraw();
      } else if (hitBox(bVoiceDel, t.x, t.y)) {           // trim the last word
        M5.Speaker.tone(1500, 40);
        voiceTranscript.trim();
        int sp = voiceTranscript.lastIndexOf(' ');
        voiceTranscript = (sp >= 0) ? voiceTranscript.substring(0, sp) : "";
        descScroll = 0; redraw();
      } else if (hitBox(bVoiceAdd, t.x, t.y)) {            // dictate more, append
        M5.Speaker.tone(1200, 60); startVoiceStream(true);
      } else if (voiceTranscript.length() && hitBox(bVoiceSend, t.x, t.y)) {
        M5.Speaker.tone(1200, 60);
        reqChoice = voiceTranscript; reqState = REQ_ANSWERED; answeredAt = millis();
        screen = SCR_REQUEST; redraw();
      }
    } else if (voiceState == V_ERROR && t.wasPressed()) {
      if (hitBox(bVoiceRedo, t.x, t.y)) { M5.Speaker.tone(1200, 60); startVoiceStream(false); }
      else if (hitBox(bVoiceCancel, t.x, t.y)) { M5.Speaker.tone(1200, 60); screen = SCR_REQUEST; redraw(); }
    }
    return;
  }

  if (!t.wasPressed()) return;
  int x = t.x, y = t.y;
  auto tap = [&]() { M5.Speaker.tone(1200, 60); };
  switch (screen) {
    case SCR_START:
      if (hitBox(bSetup, x, y)) { tap(); startPortal(); }
      break;
    case SCR_AP:
      if (apHasClient && hitBox(bNext, x, y)) { tap(); screen = SCR_QR; redraw(); }
      else if (hitBox(bCancel, x, y)) { tap(); stopPortal(); screen = SCR_START; redraw(); }
      break;
    case SCR_QR:
      if (hitBox(bBack, x, y)) { tap(); screen = SCR_AP; redraw(); }
      else if (hitBox(bCancel, x, y)) { tap(); stopPortal(); screen = SCR_START; redraw(); }
      break;
    case SCR_CONNECTED:
      if (hitBox(bPair, x, y)) { tap(); enterPairing(); }
      else if (hitBox(bRedo, x, y)) { tap(); startPortal(); }
      break;
    case SCR_REQUEST:
      if (reqState == REQ_PENDING) {
        for (int i = 0; i < reqOptionCount; i++) {
          if (hitBox(reqOptBox[i], x, y)) {
            tap(); reqChoice = reqOptions[i]; reqState = REQ_ANSWERED; answeredAt = millis(); redraw();
            break;
          }
        }
      }
      break;
    case SCR_PAIR:
      if (pairState == PAIR_PENDING) {
        if (hitBox(bApprove, x, y)) {
          tap();
          pairToken = randomToken();
          token = pairToken; paired = true;
          prefs.putString("token", token);
          pairState = PAIR_APPROVED;
          pairingMode = false;
          screen = SCR_CONNECTED; redraw();
          Serial.println("[agent-remote] paired");
        } else if (hitBox(bDeny, x, y)) {
          tap(); pairState = PAIR_DENIED; pairingMode = false;
          screen = SCR_CONNECTED; redraw();
        }
      } else if (hitBox(bCancel, x, y)) {
        tap(); pairingMode = false; pairState = PAIR_IDLE;
        screen = SCR_CONNECTED; redraw();
      }
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
  token    = prefs.getString("token", "");
  paired   = !token.isEmpty();

  static const char* authHeaders[] = { "X-Agent-Token" };
  server.collectHeaders(authHeaders, 1);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ping", handlePing);
  server.on("/request", HTTP_POST, handleRequest);
  server.on("/status", handleStatus);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/pair", HTTP_POST, handlePair);
  server.on("/pairstatus", handlePairStatus);
  server.onNotFound(handleNotFound);

  if (!wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    Serial.printf("[agent-remote] connecting to saved '%s'...\n", wifiSsid.c_str());
    if (tryConnect(wifiSsid, wifiPass, 12000)) {
      connectedIP = WiFi.localIP().toString();
      startMdns();
      ensureServer();
      screen = SCR_CONNECTED;
      Serial.printf("[agent-remote] online at http://%s/ (http://%s.local)\n",
                    connectedIP.c_str(), MDNS_NAME);
    }
  }
  redraw();
  Serial.println("[agent-remote] setup done");
}

void loop() {
  M5.update();
  if (serverStarted) server.handleClient();
  if (voiceActive) voiceStreamStep();   // non-blocking mic streaming
  if (apActive) {
    dns.processNextRequest();
    if (screen == SCR_AP) {
      bool now = WiFi.softAPgetStationNum() > 0;
      if (now != apHasClient) { apHasClient = now; redraw(); }
    }
  }
  // Auto-return to idle a couple seconds after a tap.
  if (screen == SCR_REQUEST && reqState == REQ_ANSWERED && millis() - answeredAt > 2000) {
    screen = SCR_CONNECTED;
    redraw();
  }
  // Pairing window: tick the countdown and expire it.
  if (pairingMode && pairState != PAIR_PENDING) {
    static uint32_t lastTick = 0;
    if (millis() > pairDeadline) {
      pairingMode = false; pairState = PAIR_IDLE;
      if (screen == SCR_PAIR) { screen = SCR_CONNECTED; redraw(); }
    } else if (screen == SCR_PAIR && millis() - lastTick > 1000) {
      lastTick = millis(); redraw();
    }
  }
  handleTouch();
  delay(5);
}
