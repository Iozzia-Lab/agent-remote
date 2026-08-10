// agent-remote — M5Stack Core2 firmware
//
// Step 1 (this file): the start screen. Shows the device title and the
// instructions for connecting to set up WiFi. Touch + the WiFi captive portal
// and the approval/pairing server are layered on next (see
// firmware/reference/agent-remote-full.ino for the target design).
//
// Build with PlatformIO:  pio run -t upload   (NOT arduino-cli — see platformio.ini)

#include <M5Unified.h>

const uint16_t C_BLACK  = 0x0000;
const uint16_t C_WHITE  = 0xFFFF;
const uint16_t C_CYAN   = 0x07FF;
const uint16_t C_YELLOW = 0xFFE0;
const uint16_t C_GREY   = 0x4208;
const uint16_t C_LTGREY = 0xC618;

void centerText(const char* s, int cx, int cy, int size, uint16_t fg) {
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, C_BLACK);
  int w = M5.Display.textWidth(s);
  int h = M5.Display.fontHeight();
  M5.Display.setCursor(cx - w / 2, cy - h / 2);
  M5.Display.print(s);
}

void drawStartScreen() {
  M5.Display.fillScreen(C_BLACK);

  // Outer border
  M5.Display.drawRect(0, 0, 320, 240, C_WHITE);
  M5.Display.drawRect(1, 1, 318, 238, C_WHITE);

  // Title box
  M5.Display.drawRect(8, 8, 304, 58, C_CYAN);
  centerText("Agent-remote", 160, 37, 4, C_WHITE);

  // Instructions box
  M5.Display.drawRect(8, 74, 304, 158, C_GREY);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(C_CYAN, C_BLACK);
  M5.Display.setCursor(18, 84);
  M5.Display.print("Set up WiFi");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(18, 116); M5.Display.print("1. On your phone, join WiFi:");
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setCursor(34, 132); M5.Display.print("agent-remote-setup");
  M5.Display.setTextColor(C_LTGREY, C_BLACK);
  M5.Display.setCursor(18, 158); M5.Display.print("2. Open  http://192.168.4.1");
  M5.Display.setCursor(18, 182); M5.Display.print("3. Enter your WiFi password");
  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(18, 210); M5.Display.print("(server starts after WiFi)");
}

void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println("[agent-remote] setup start");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(180);
  Serial.println("[agent-remote] M5 begun");

  drawStartScreen();
  Serial.println("[agent-remote] start screen drawn");
}

void loop() {
  M5.update();
  static uint32_t t = 0;
  if (millis() - t > 5000) { t = millis(); Serial.println("[agent-remote] alive"); }
  delay(10);
}
