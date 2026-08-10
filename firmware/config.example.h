// Copy this file to `config.h` and fill in your values.
// config.h is git-ignored so your WiFi credentials stay private.
#pragma once

// ---- WiFi ----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---- Device identity ----
// The Mac-side hook finds the device at http://<HOSTNAME>.local
// (mDNS/Bonjour). Keep it short and lowercase.
#define DEVICE_HOSTNAME "agent-remote"

// HTTP port the device listens on.
#define HTTP_PORT 80

// ---- UX ----
// Play a chime + screen wake when a new request arrives.
#define ENABLE_CHIME  true
// Screen auto-dims after this many ms of inactivity (0 = never).
#define SCREEN_DIM_MS 30000
