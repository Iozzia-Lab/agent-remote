// Copy this file to `config.h` before building.
// WiFi credentials are NOT here anymore — you set those on the device itself
// (Settings > WiFi Setup). This file is just device identity + UX.
#pragma once

// mDNS name: the Mac-side hook finds the device at http://<HOSTNAME>.local
#define DEVICE_HOSTNAME "agent-remote"

// HTTP port the device listens on.
#define HTTP_PORT 80

// SSID of the temporary setup hotspot shown during WiFi Setup.
#define SETUP_AP_NAME "agent-remote-setup"

// Play a chime + wake the screen when a new request/pair prompt arrives.
#define ENABLE_CHIME true

// Screen auto-dims after this many ms of inactivity (0 = never).
#define SCREEN_DIM_MS 30000
