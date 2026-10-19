#pragma once

// ============================================================================
// LilyGO T5 4.7" E-Paper — Numa DNS Dashboard & Controller Configuration
// ============================================================================
//
// Hardware Revision: LilyGO T5 4.7" (ESP32-D0WD-V3, 16MB Flash, 8MB PSRAM)
// Silkscreen Buttons:
//   - S4 (GPIO 39 / BUTTON_3) : Cycle Dashboard Page (Overview / Query Log / Services)
//   - S3 (GPIO 34 / BUTTON_1) : Toggle Ad Blocking (Short) / Pause 5m (Long)
//   - S2 (GPIO 35 / BUTTON_2) : Flush DNS Cache
//   - S5 (GPIO 0  / BOOT)     : Force Sync Refresh (Short) / Switch USB-WiFi (Long)
//   - S6 (RST)                : Hardware Reset

#define PIN_BTN_PAGE    39  // Silkscreen S4
#define PIN_BTN_BLOCK   34  // Silkscreen S3
#define PIN_BTN_CACHE   35  // Silkscreen S2
#define PIN_BTN_SYNC    0   // Silkscreen S5
#define PIN_BATT_ADC    36  // Battery voltage divider ADC pin

// Default Numa API Host on LAN (used in Wi-Fi fallback mode)
// Can also be updated live at runtime over USB-C Serial without reflashing!
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif

#ifndef DEFAULT_NUMA_HOST
#define DEFAULT_NUMA_HOST "192.168.10.78"
#endif

#ifndef DEFAULT_NUMA_PORT
#define DEFAULT_NUMA_PORT 5380
#endif

// Serial protocol baud rate over USB-C (/dev/cu.usbserial-54250266921)
#define SERIAL_BAUD 115200
