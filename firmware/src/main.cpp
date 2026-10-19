#ifndef BOARD_HAS_PSRAM
#error "PSRAM must be enabled for LilyGO T5 4.7 E-Paper!"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Button2.h>
#include <esp_adc_cal.h>

#include "epd_driver.h"
#include "firasans.h"
#include "roboto12.h"
#include "roboto18.h"
#include "roboto32.h"
#include "config.h"
#include "travel_dns.h"
#include "travel_webui.h"

// ============================================================================
// Data Models
// ============================================================================

struct QueryLogItem {
    String domain;
    String qtype;
    String path;
    String rcode;
    float latency_ms;
    String src;
};

struct ServiceItem {
    String name;
    uint16_t port;
    String url;
};

struct OverrideItem {
    String domain;
    String target;
    String rtype;
    uint32_t ttl;
};

struct NumaState {
    bool connected = true;
    String transport_mode = "TRAVEL DNS"; // "TRAVEL DNS" or "USB-C"
    String version = "0.14.3";
    uint64_t uptime_secs = 0;
    String upstream = "9.9.9.9:53";
    String mode = "standalone";
    String lan_ip = "192.168.4.1";
    uint16_t dns_port = 53;
    uint16_t api_port = 80;
    bool dnssec = false;
    bool srtt = true;

    // Query counters
    uint64_t q_total = 0;
    uint64_t q_cached = 0;
    uint64_t q_upstream = 0;
    uint64_t q_local = 0;
    uint64_t q_blocked = 0;
    uint64_t q_overridden = 0;
    uint64_t q_errors = 0;

    // Transport counters
    uint64_t t_udp = 0;
    uint64_t t_tcp = 0;
    uint64_t t_dot = 0;
    uint64_t t_doh = 0;

    // Cache & Blocking
    uint32_t cache_entries = 0;
    uint32_t cache_max = DNS_CACHE_SLOTS;
    bool blocking_enabled = true;
    bool blocking_paused = false;
    uint32_t domains_loaded = HAGEZI_DOMAIN_COUNT;
    uint32_t allowlist_size = 0;

    // Memory
    uint32_t mem_process_mb = 4;
    uint32_t mem_blocklist_mb = 1;

    // Lists
    QueryLogItem logs[8];
    int log_count = 0;

    ServiceItem services[6];
    int service_count = 0;

    OverrideItem overrides[6];
    int override_count = 0;
};

// ============================================================================
// Globals
// ============================================================================

static uint8_t *framebuffer = nullptr;
static NumaState state;
static NumaState standaloneState;
static Preferences prefs;
static TravelDnsEngine travelDns;
static WebServer webServer(80);

static Button2 btnPage(PIN_BTN_PAGE);
static Button2 btnBlock(PIN_BTN_BLOCK);
static Button2 btnCache(PIN_BTN_CACHE);
static Button2 btnSync(PIN_BTN_SYNC);

static int currentPage = 0; // 0=Overview, 1=Query Log, 2=Travel Setup, 3=.numa Services
static const int TOTAL_PAGES = 4;

static String toastMessage = "";
static unsigned long toastUntilMs = 0;
static unsigned long pauseUntilMs = 0;

static unsigned long lastSerialRxMs = 0;
static unsigned long lastHelloTxMs = 0;
static unsigned long lastRenderMs = 0;
static int partialRefreshCount = 0;
static bool needsRender = true;
static bool forceFullClear = true;
static bool forceStandaloneMode = false; // toggled by holding S5

static String wifiSsid = DEFAULT_WIFI_SSID;
static String wifiPass = DEFAULT_WIFI_PASS;
static String numaHost = DEFAULT_NUMA_HOST;
static uint16_t numaPort = DEFAULT_NUMA_PORT;
static const char *TRAVEL_AP_SSID = "Numa-Travel-DNS";

static String serialLineBuf = "";
static int vref = 1100;

static bool isStandaloneActive() {
    if (forceStandaloneMode) return true;
    // If USB-C bridge hasn't pushed state in the last 12 seconds, run in Standalone Travel Mode!
    if (lastSerialRxMs == 0 || (millis() - lastSerialRxMs > 12000)) return true;
    return false;
}

static NumaState &activeState() {
    return isStandaloneActive() ? standaloneState : state;
}

static String currentDeviceIp() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return WiFi.softAPIP().toString();
}

static void pushStandaloneLog(const String &domain, const String &qtype, const String &path, const String &rcode, float latencyMs, const String &src) {
    for (int i = 7; i > 0; i--) {
        standaloneState.logs[i] = standaloneState.logs[i - 1];
    }
    standaloneState.logs[0] = {domain, qtype, path, rcode, latencyMs, src};
    if (standaloneState.log_count < 8) standaloneState.log_count++;
}

// ============================================================================
// Drawing Helpers (Vercel Monochrome High-Contrast E-Paper System)
// ============================================================================

static void drawText(const GFXfont *font, int32_t x, int32_t y, const char *text, uint8_t fg = 0, uint8_t bg = 15) {
    if (!text || !text[0]) return;
    int32_t cx = x;
    int32_t cy = y;
    FontProperties props = {
        .fg_color = fg,
        .bg_color = bg,
        .fallback_glyph = 0,
        .flags = 0
    };
    write_mode(font, text, &cx, &cy, framebuffer, BLACK_ON_WHITE, &props);
}

static int32_t textWidth(const GFXfont *font, const char *text) {
    if (!text || !text[0]) return 0;
    int32_t x = 0, y = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    get_text_bounds(font, text, &x, &y, &x1, &y1, &w, &h, NULL);
    return w;
}

static void drawTextRight(const GFXfont *font, int32_t right_x, int32_t y, const char *text, uint8_t fg = 0, uint8_t bg = 15) {
    int32_t w = textWidth(font, text);
    drawText(font, right_x - w, y, text, fg, bg);
}

static void drawCard(int32_t x, int32_t y, int32_t w, int32_t h, bool doubleBorder = false) {
    epd_fill_rect(x, y, w, h, 0xFF, framebuffer);
    epd_draw_rect(x, y, w, h, 0x00, framebuffer);
    if (doubleBorder) {
        epd_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x00, framebuffer);
    }
}

static void drawBadge(int32_t x, int32_t y, int32_t w, int32_t h, const char *label, bool inverted) {
    if (inverted) {
        epd_fill_rect(x, y, w, h, 0x00, framebuffer);
        int32_t tw = textWidth(&Roboto12, label);
        int32_t tx = x + (w - tw) / 2;
        int32_t ty = y + (h / 2) + 8;
        drawText(&Roboto12, tx, ty, label, 15, 0);
    } else {
        epd_fill_rect(x, y, w, h, 0xF0, framebuffer);
        epd_draw_rect(x, y, w, h, 0x00, framebuffer);
        epd_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x00, framebuffer);
        int32_t tw = textWidth(&Roboto12, label);
        int32_t tx = x + (w - tw) / 2;
        int32_t ty = y + (h / 2) + 8;
        drawText(&Roboto12, tx, ty, label, 0, 15);
    }
}

static String formatUptime(uint64_t secs) {
    if (secs < 60) return String((unsigned long)secs) + "s";
    if (secs < 3600) return String((unsigned long)(secs / 60)) + "m " + String((unsigned long)(secs % 60)) + "s";
    uint64_t hrs = secs / 3600;
    uint64_t mins = (secs % 3600) / 60;
    if (hrs < 24) return String((unsigned long)hrs) + "h " + String((unsigned long)mins) + "m";
    return String((unsigned long)(hrs / 24)) + "d " + String((unsigned long)(hrs % 24)) + "h";
}

static String formatNumber(uint64_t n) {
    if (n >= 1000000) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fM", (double)n / 1000000.0);
        return String(buf);
    }
    if (n >= 10000) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fK", (double)n / 1000.0);
        return String(buf);
    }
    return String((unsigned long)n);
}

static float readBatteryVoltage() {
    uint16_t raw = analogRead(PIN_BATT_ADC);
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, vref, &adc_chars);
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
    return (mv * 2.0f) / 1000.0f;
}

static String truncateStr(const String &s, size_t maxChars) {
    if (s.length() <= maxChars) return s;
    return s.substring(0, maxChars - 2) + "..";
}

// ============================================================================
// Dashboard Page Renderers (960 x 540)
// ============================================================================

static void renderHeader(const NumaState &st) {
    epd_fill_rect(0, 0, EPD_WIDTH, 64, 0x00, framebuffer);
    drawText(&FiraSans, 20, 46, "NUMA", 15, 0);

    String modeUpper = st.mode;
    modeUpper.toUpperCase();
    String sub = (isStandaloneActive() ? String("TRAVEL STANDALONE  |  ") : ("v" + st.version + "  |  ")) +
                 modeUpper + " -> " + st.upstream;
    drawText(&Roboto12, 168, 28, sub.c_str(), 13, 0);

    String netLine = "DNS " + st.lan_ip + ":" + String(st.dns_port) +
                     "   WEB :" + String(st.api_port) +
                     "   UP " + formatUptime(st.uptime_secs);
    drawText(&Roboto12, 168, 52, netLine.c_str(), 11, 0);

    int32_t bx = 610;
    if (st.blocking_enabled && !st.blocking_paused) {
        epd_fill_rect(bx, 14, 190, 36, 0xFF, framebuffer);
        String blText = "BLOCKING ON (" + formatNumber(st.domains_loaded) + ")";
        int32_t tw = textWidth(&Roboto12, blText.c_str());
        drawText(&Roboto12, bx + (190 - tw) / 2, 39, blText.c_str(), 0, 15);
    } else if (st.blocking_paused) {
        epd_draw_rect(bx, 14, 190, 36, 0xFF, framebuffer);
        epd_draw_rect(bx + 1, 15, 188, 34, 0xFF, framebuffer);
        drawText(&Roboto12, bx + 22, 39, "BLOCKING PAUSED", 15, 0);
    } else {
        epd_draw_rect(bx, 14, 190, 36, 0xFF, framebuffer);
        epd_draw_rect(bx + 1, 15, 188, 34, 0xFF, framebuffer);
        drawText(&Roboto12, bx + 32, 39, "BLOCKING OFF", 15, 0);
    }

    int32_t lx = 810;
    epd_draw_rect(lx, 14, 130, 36, 0xFF, framebuffer);
    String linkLabel = isStandaloneActive() ? "TRAVEL :53" : "USB-C MAC";
    int32_t ltw = textWidth(&Roboto12, linkLabel.c_str());
    drawText(&Roboto12, lx + (130 - ltw) / 2, 39, linkLabel.c_str(), 15, 0);
}

static void renderPageOverview(const NumaState &st) {
    const int32_t cardW = 220;
    const int32_t cardH = 168;
    const int32_t cardY = 78;
    const int32_t gap = 13;

    // Card 1: Total Queries
    int32_t x1 = 20;
    drawCard(x1, cardY, cardW, cardH, true);
    drawText(&Roboto12, x1 + 16, cardY + 28, "TOTAL QUERIES");
    epd_draw_hline(x1 + 16, cardY + 36, cardW - 32, 0x80, framebuffer);
    drawText(&Roboto32, x1 + 16, cardY + 104, formatNumber(st.q_total).c_str());
    String dnsIpStr = "DNS IP: " + st.lan_ip;
    drawText(&Roboto12, x1 + 16, cardY + 136, dnsIpStr.c_str(), 4, 15);
    String portStr = "Port :" + String(st.dns_port) + (isStandaloneActive() ? " (On-ESP32)" : " (Mac Numa)");
    drawText(&Roboto12, x1 + 16, cardY + 158, portStr.c_str(), 6, 15);

    // Card 2: Ad Blocking
    int32_t x2 = x1 + cardW + gap;
    drawCard(x2, cardY, cardW, cardH, true);
    drawText(&Roboto12, x2 + 16, cardY + 28, "ADS & TRACKERS");
    epd_draw_hline(x2 + 16, cardY + 36, cardW - 32, 0x80, framebuffer);
    drawText(&Roboto32, x2 + 16, cardY + 104, formatNumber(st.q_blocked).c_str());
    float blkPct = (st.q_total > 0) ? (100.0f * (float)st.q_blocked / (float)st.q_total) : 0.0f;
    char pctBuf[32];
    snprintf(pctBuf, sizeof(pctBuf), "%.1f%% of queries", blkPct);
    drawText(&Roboto12, x2 + 16, cardY + 136, pctBuf, 4, 15);
    String rulesStr = formatNumber(st.domains_loaded) + " Hagezi rules";
    drawText(&Roboto12, x2 + 16, cardY + 158, rulesStr.c_str(), 6, 15);

    // Card 3: DNS Cache
    int32_t x3 = x2 + cardW + gap;
    drawCard(x3, cardY, cardW, cardH, true);
    drawText(&Roboto12, x3 + 16, cardY + 28, "CACHE PERFORMANCE");
    epd_draw_hline(x3 + 16, cardY + 36, cardW - 32, 0x80, framebuffer);
    float hitPct = (st.q_total > 0) ? (100.0f * (float)st.q_cached / (float)st.q_total) : 0.0f;
    char hitBuf[24];
    snprintf(hitBuf, sizeof(hitBuf), "%.0f%%", hitPct);
    drawText(&Roboto32, x3 + 16, cardY + 104, hitBuf);
    String cHitsStr = "Hits: " + formatNumber(st.q_cached);
    drawText(&Roboto12, x3 + 16, cardY + 136, cHitsStr.c_str(), 4, 15);
    String cEntStr = "Stored: " + String(st.cache_entries) + " in PSRAM";
    drawText(&Roboto12, x3 + 16, cardY + 158, cEntStr.c_str(), 6, 15);

    // Card 4: Local .numa & Custom Rules
    int32_t x4 = x3 + cardW + gap;
    drawCard(x4, cardY, cardW, cardH, true);
    drawText(&Roboto12, x4 + 16, cardY + 28, ".NUMA & CUSTOM");
    epd_draw_hline(x4 + 16, cardY + 36, cardW - 32, 0x80, framebuffer);
    uint64_t localTotal = st.q_local + st.q_overridden;
    drawText(&Roboto32, x4 + 16, cardY + 104, formatNumber(localTotal).c_str());
    String svcStr = String(travelDns.customBlockCount) + " custom blocked";
    drawText(&Roboto12, x4 + 16, cardY + 136, svcStr.c_str(), 4, 15);
    String ovrStr = String(travelDns.customAllowCount) + " custom allowed";
    drawText(&Roboto12, x4 + 16, cardY + 158, ovrStr.c_str(), 6, 15);

    // Bottom Split Panel (y = 260..466)
    int32_t panelY = 260;
    int32_t panelH = 206;

    int32_t lpX = 20, lpW = 450;
    drawCard(lpX, panelY, lpW, panelH, false);
    drawText(&Roboto18, lpX + 16, panelY + 34, "RESOLUTION PIPELINE");
    epd_draw_hline(lpX + 16, panelY + 44, lpW - 32, 0x00, framebuffer);

    struct BarRow {
        const char *label;
        uint64_t count;
        uint8_t shade;
    };
    BarRow rows[4] = {
        {"CACHED",   st.q_cached,   0x20},
        {"UPSTREAM", st.q_upstream, 0x60},
        {"LOCAL",    st.q_local + st.q_overridden, 0x40},
        {"BLOCKED",  st.q_blocked,  0x00}
    };
    uint64_t maxVal = 1;
    for (int i = 0; i < 4; i++) {
        if (rows[i].count > maxVal) maxVal = rows[i].count;
    }

    int32_t barStartX = lpX + 118;
    int32_t barMaxW = 240;
    for (int i = 0; i < 4; i++) {
        int32_t ry = panelY + 72 + i * 30;
        drawText(&Roboto12, lpX + 16, ry, rows[i].label);
        epd_fill_rect(barStartX, ry - 16, barMaxW, 18, 0xE0, framebuffer);
        epd_draw_rect(barStartX, ry - 16, barMaxW, 18, 0x80, framebuffer);
        int32_t fillW = (int32_t)((rows[i].count * barMaxW) / maxVal);
        if (rows[i].count > 0 && fillW < 6) fillW = 6;
        if (fillW > 0) {
            epd_fill_rect(barStartX, ry - 16, fillW, 18, rows[i].shade, framebuffer);
        }
        drawTextRight(&Roboto12, lpX + lpW - 16, ry, formatNumber(rows[i].count).c_str());
    }

    epd_draw_hline(lpX + 16, panelY + 174, lpW - 32, 0xB0, framebuffer);
    float vbat = readBatteryVoltage();
    char sysBuf[64];
    snprintf(sysBuf, sizeof(sysBuf), "PSRAM Free: %uKB  |  BAT: %.2fV  |  [S4] Travel Info",
             ESP.getFreePsram() / 1024, vbat);
    drawText(&Roboto12, lpX + 16, panelY + 198, sysBuf, 4, 15);

    // Right Panel: Recent Queries
    int32_t rpX = 483, rpW = 457;
    drawCard(rpX, panelY, rpW, panelH, false);
    drawText(&Roboto18, rpX + 16, panelY + 34, "RECENT QUERIES");
    drawTextRight(&Roboto12, rpX + rpW - 16, panelY + 32, "[S4] Next Page ->", 5, 15);
    epd_draw_hline(rpX + 16, panelY + 44, rpW - 32, 0x00, framebuffer);

    int showCount = min(st.log_count, 5);
    if (showCount == 0) {
        drawText(&Roboto12, rpX + 16, panelY + 100, "Point your phone/laptop DNS to:", 4, 15);
        drawText(&Roboto18, rpX + 16, panelY + 140, st.lan_ip.c_str(), 0, 15);
    } else {
        for (int i = 0; i < showCount; i++) {
            int32_t ly = panelY + 74 + i * 30;
            String dom = truncateStr(st.logs[i].domain, 22);
            drawText(&Roboto12, rpX + 16, ly, dom.c_str());

            String badge = st.logs[i].path;
            bool isBlocked = (badge == "BLOCKED" || st.logs[i].rcode == "NXDOMAIN");
            if (isBlocked) {
                epd_fill_rect(rpX + 275, ly - 18, 92, 22, 0x00, framebuffer);
                drawText(&Roboto12, rpX + 283, ly - 1, badge.c_str(), 15, 0);
            } else {
                epd_draw_rect(rpX + 275, ly - 18, 92, 22, 0x40, framebuffer);
                drawText(&Roboto12, rpX + 283, ly - 1, badge.c_str(), 2, 15);
            }

            char latBuf[20];
            if (st.logs[i].latency_ms < 1.0f) {
                snprintf(latBuf, sizeof(latBuf), "%.2fms", st.logs[i].latency_ms);
            } else {
                snprintf(latBuf, sizeof(latBuf), "%.0fms", st.logs[i].latency_ms);
            }
            drawTextRight(&Roboto12, rpX + rpW - 16, ly, latBuf, 4, 15);
        }
    }
}

static void renderPageQueryLog(const NumaState &st) {
    int32_t x = 20, y = 78, w = 920, h = 388;
    drawCard(x, y, w, h, true);

    drawText(&Roboto18, x + 20, y + 36, "LIVE DNS QUERY STREAM");
    String sub = "Showing latest " + String(st.log_count) + " queries  |  Total: " + formatNumber(st.q_total);
    drawTextRight(&Roboto12, x + w - 20, y + 34, sub.c_str(), 5, 15);

    epd_fill_rect(x + 12, y + 48, w - 24, 30, 0x20, framebuffer);
    drawText(&Roboto12, x + 24,  y + 70, "DOMAIN", 15, 2);
    drawText(&Roboto12, x + 430, y + 70, "TYPE", 15, 2);
    drawText(&Roboto12, x + 520, y + 70, "PIPELINE", 15, 2);
    drawText(&Roboto12, x + 665, y + 70, "STATUS", 15, 2);
    drawTextRight(&Roboto12, x + w - 24, y + 70, "LATENCY", 15, 2);

    if (st.log_count == 0) {
        drawText(&Roboto18, x + 260, y + 220, "Waiting for DNS queries on port 53...", 6, 15);
        return;
    }

    int rows = min(st.log_count, 8);
    for (int i = 0; i < rows; i++) {
        int32_t ry = y + 108 + i * 36;
        if (i % 2 == 1) {
            epd_fill_rect(x + 12, ry - 24, w - 24, 34, 0xF0, framebuffer);
        }
        String dom = truncateStr(st.logs[i].domain, 34);
        drawText(&Roboto12, x + 24, ry, dom.c_str());
        drawText(&Roboto12, x + 430, ry, st.logs[i].qtype.c_str(), 4, 15);

        String path = st.logs[i].path;
        if (path == "BLOCKED") {
            epd_fill_rect(x + 516, ry - 19, 110, 24, 0x00, framebuffer);
            drawText(&Roboto12, x + 526, ry - 1, "BLOCKED", 15, 0);
        } else {
            drawText(&Roboto12, x + 520, ry, path.c_str(), 2, 15);
        }

        drawText(&Roboto12, x + 665, ry, st.logs[i].rcode.c_str(), 4, 15);

        char latBuf[24];
        if (st.logs[i].latency_ms < 1.0f) {
            snprintf(latBuf, sizeof(latBuf), "%.2f ms", st.logs[i].latency_ms);
        } else {
            snprintf(latBuf, sizeof(latBuf), "%.1f ms", st.logs[i].latency_ms);
        }
        drawTextRight(&Roboto12, x + w - 24, ry, latBuf);
    }
}

static void renderPageTravelSetup(const NumaState &st) {
    int32_t y = 78, h = 388;

    // Left Card: Portable Travel Adblocker Instructions & DNS IP
    int32_t lx = 20, lw = 480;
    drawCard(lx, y, lw, h, true);
    drawText(&Roboto18, lx + 20, y + 36, "PORTABLE TRAVEL ADBLOCKER");
    epd_draw_hline(lx + 20, y + 48, lw - 40, 0x00, framebuffer);

    drawText(&Roboto12, lx + 20, y + 78, "1. SET PHONE / LAPTOP DNS SERVER TO:", 4, 15);
    epd_fill_rect(lx + 20, y + 90, lw - 40, 74, 0x00, framebuffer);
    String ipStr = currentDeviceIp();
    int32_t ipW = textWidth(&Roboto32, ipStr.c_str());
    drawText(&Roboto32, lx + 20 + ((lw - 40 - ipW) / 2), y + 146, ipStr.c_str(), 15, 0);

    drawText(&Roboto12, lx + 20, y + 194, "2. TRAVEL WIFI SETUP HOTSPOT (ALWAYS ON):", 4, 15);
    String apLine = String("   Wi-Fi SSID:  ") + TRAVEL_AP_SSID + "  (Open)";
    String uiLine = String("   Setup URL:   http://192.168.4.1  or  http://numa.numa");
    drawText(&Roboto12, lx + 20, y + 224, apLine.c_str(), 0, 15);
    drawText(&Roboto12, lx + 20, y + 252, uiLine.c_str(), 0, 15);

    epd_draw_hline(lx + 20, y + 272, lw - 40, 0xB0, framebuffer);
    drawText(&Roboto12, lx + 20, y + 300, "3. ON-DEVICE HAGEZI PRO ENGINE:", 4, 15);
    drawText(&Roboto12, lx + 20, y + 328, "   202,302 domains in PSRAM (40-bit FNV-1a binary search)", 2, 15);
    drawText(&Roboto12, lx + 20, y + 356, "   Hold [S5] 1s to switch Travel Standalone <-> USB-C Mode", 2, 15);

    // Right Card: Live Hardware & Network Node Diagnostics
    int32_t rx = 515, rw = 425;
    drawCard(rx, y, rw, h, true);
    drawText(&Roboto18, rx + 20, y + 36, "NODE & RADIO STATUS");
    epd_draw_hline(rx + 20, y + 48, rw - 40, 0x00, framebuffer);

    String modeStr = isStandaloneActive() ? "Mode:      STANDALONE TRAVEL DNS (:53)" : "Mode:      USB-C MAC NUMA BRIDGE";
    String staStr  = (WiFi.status() == WL_CONNECTED)
                     ? ("Wi-Fi STA: Connected (" + WiFi.SSID() + ")")
                     : ("Wi-Fi STA: Not connected (Use AP 192.168.4.1)");
    String apStr   = "SoftAP:    " + String(TRAVEL_AP_SSID) + " (192.168.4.1)";
    String upStr   = "Upstream:  " + travelDns.upstreamIp.toString() + ":53";
    char memStr[64];
    snprintf(memStr, sizeof(memStr), "PSRAM:     %u KB free / 4310 KB total", ESP.getFreePsram() / 1024);
    float vbat = readBatteryVoltage();
    int batPct = constrain((int)((vbat - 3.3f) * 100.0f / (4.2f - 3.3f)), 0, 100);
    char batStr[64];
    snprintf(batStr, sizeof(batStr), "Battery:   %.2f V (%d%%)", vbat, batPct);

    drawText(&Roboto12, rx + 20, y + 84,  modeStr.c_str(), 0, 15);
    drawText(&Roboto12, rx + 20, y + 118, staStr.c_str(),  2, 15);
    drawText(&Roboto12, rx + 20, y + 152, apStr.c_str(),   2, 15);
    drawText(&Roboto12, rx + 20, y + 186, upStr.c_str(),   2, 15);
    drawText(&Roboto12, rx + 20, y + 220, memStr,          2, 15);
    drawText(&Roboto12, rx + 20, y + 254, batStr,          2, 15);

    epd_fill_rect(rx + 20, y + 282, rw - 40, 88, 0xF0, framebuffer);
    epd_draw_rect(rx + 20, y + 282, rw - 40, 88, 0x00, framebuffer);
    drawText(&Roboto12, rx + 34, y + 312, "QUICK TRAVEL WORKFLOW:", 0, 15);
    drawText(&Roboto12, rx + 34, y + 338, "Join 'Numa-Travel-DNS' on phone ->", 3, 15);
    drawText(&Roboto12, rx + 34, y + 360, "Open http://192.168.4.1 to pair hotel Wi-Fi", 3, 15);
}

static void renderPageServices(const NumaState &st) {
    int32_t y = 78, h = 388;

    int32_t lx = 20, lw = 450;
    drawCard(lx, y, lw, h, true);
    drawText(&Roboto18, lx + 20, y + 36, "LOCAL .NUMA DOMAINS");
    epd_draw_hline(lx + 20, y + 48, lw - 40, 0x00, framebuffer);

    String myIp = currentDeviceIp();
    drawText(&Roboto12, lx + 20, y + 80, "http://numa.numa", 0, 15);
    drawTextRight(&Roboto12, lx + lw - 20, y + 80, myIp.c_str(), 5, 15);
    epd_draw_hline(lx + 20, y + 92, lw - 40, 0xD0, framebuffer);

    drawText(&Roboto12, lx + 20, y + 122, "http://travel.numa", 0, 15);
    drawTextRight(&Roboto12, lx + lw - 20, y + 122, myIp.c_str(), 5, 15);
    epd_draw_hline(lx + 20, y + 134, lw - 40, 0xD0, framebuffer);

    for (int i = 0; i < min(st.service_count, 4); i++) {
        int32_t sy = y + 166 + i * 42;
        String label = "https://" + st.services[i].name + ".numa";
        String target = "localhost:" + String(st.services[i].port);
        drawText(&Roboto12, lx + 20, sy, label.c_str(), 0, 15);
        drawTextRight(&Roboto12, lx + lw - 20, sy, target.c_str(), 4, 15);
        epd_draw_hline(lx + 20, sy + 12, lw - 40, 0xD0, framebuffer);
    }

    drawText(&Roboto12, lx + 20, y + h - 48, "Built-in .numa local domain resolution", 5, 15);
    drawText(&Roboto12, lx + 20, y + h - 22, "Works offline on any hotel/cafe network", 6, 15);

    int32_t rx = 490, rw = 450;
    drawCard(rx, y, rw, h, true);
    drawText(&Roboto18, rx + 20, y + 36, "CUSTOM BLOCK / ALLOW RULES");
    epd_draw_hline(rx + 20, y + 48, rw - 40, 0x00, framebuffer);

    int rowY = y + 82;
    if (travelDns.customBlockCount == 0 && travelDns.customAllowCount == 0) {
        drawText(&Roboto12, rx + 20, rowY, "No custom rules added yet.", 5, 15);
        String hint = "Add domains via http://" + myIp;
        drawText(&Roboto12, rx + 20, rowY + 30, hint.c_str(), 6, 15);
    } else {
        for (int i = 0; i < min(travelDns.customBlockCount, 4); i++) {
            String line = "[BLOCK] " + travelDns.customBlock[i];
            drawText(&Roboto12, rx + 20, rowY, truncateStr(line, 34).c_str(), 0, 15);
            rowY += 32;
        }
        for (int i = 0; i < min(travelDns.customAllowCount, 4); i++) {
            String line = "[ALLOW] " + travelDns.customAllow[i];
            drawText(&Roboto12, rx + 20, rowY, truncateStr(line, 34).c_str(), 4, 15);
            rowY += 32;
        }
    }
}

static void renderControlBar(const NumaState &st) {
    int32_t barY = 478;
    epd_fill_rect(0, barY, EPD_WIDTH, 62, 0xF0, framebuffer);
    epd_draw_hline(0, barY, EPD_WIDTH, 0x00, framebuffer);
    epd_draw_hline(0, barY + 1, EPD_WIDTH, 0x00, framebuffer);

    if (toastMessage.length() > 0 && millis() < toastUntilMs) {
        epd_fill_rect(20, barY + 10, 920, 42, 0x00, framebuffer);
        int32_t tw = textWidth(&Roboto18, toastMessage.c_str());
        drawText(&Roboto18, (EPD_WIDTH - tw) / 2, barY + 41, toastMessage.c_str(), 15, 0);
        return;
    }

    String pageLabel = "[S4] PAGE " + String(currentPage + 1) + "/" + String(TOTAL_PAGES);
    String blockLabel = (st.blocking_enabled && !st.blocking_paused)
                        ? "[S3] BLOCK: ON"
                        : "[S3] BLOCK: OFF";
    String cacheLabel = "[S2] FLUSH CACHE";
    String syncLabel  = isStandaloneActive() ? "[S5] TRAVEL MODE" : "[S5] USB-C MODE";

    drawBadge(20,  barY + 12, 218, 38, pageLabel.c_str(),  false);
    drawBadge(254, barY + 12, 218, 38, blockLabel.c_str(), st.blocking_enabled && !st.blocking_paused);
    drawBadge(488, barY + 12, 218, 38, cacheLabel.c_str(), false);
    drawBadge(722, barY + 12, 218, 38, syncLabel.c_str(),  isStandaloneActive());
}

static void renderDashboard(bool fullClear = false) {
    if (!framebuffer) return;
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    standaloneState.uptime_secs = millis() / 1000ULL;
    standaloneState.lan_ip = currentDeviceIp();
    standaloneState.cache_entries = travelDns.activeCacheCount();
    standaloneState.upstream = travelDns.upstreamIp.toString() + ":53";

    const NumaState &st = activeState();
    renderHeader(st);

    if (currentPage == 0) {
        renderPageOverview(st);
    } else if (currentPage == 1) {
        renderPageQueryLog(st);
    } else if (currentPage == 2) {
        renderPageTravelSetup(st);
    } else {
        renderPageServices(st);
    }

    renderControlBar(st);

    epd_poweron();
    if (fullClear || partialRefreshCount >= 8) {
        epd_clear();
        partialRefreshCount = 0;
    } else {
        epd_clear_area_cycles(epd_full_screen(), 2, 40);
        partialRefreshCount++;
    }
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    lastRenderMs = millis();
    needsRender = false;
    forceFullClear = false;
}

static void showToast(const String &msg, unsigned long durationMs = 2200) {
    toastMessage = msg;
    toastUntilMs = millis() + durationMs;
    needsRender = true;
}

// ============================================================================
// Standalone UDP Port 53 DNS Packet Processor
// ============================================================================

static void processStandaloneDns() {
    int pktSize = travelDns.udpClient.parsePacket();
    if (pktSize < 12 || pktSize > DNS_MAX_PACKET) return;

    uint32_t t0 = micros();
    IPAddress clientIp = travelDns.udpClient.remoteIP();
    uint16_t clientPort = travelDns.udpClient.remotePort();

    uint8_t reqBuf[DNS_MAX_PACKET];
    int len = travelDns.udpClient.read(reqBuf, DNS_MAX_PACKET);
    if (len < 12) return;

    uint16_t txid = ((uint16_t)reqBuf[0] << 8) | reqBuf[1];
    String domain;
    uint16_t qtype = 0;
    int qEnd = 0;
    if (!TravelDnsEngine::parseDnsQuestion(reqBuf, len, domain, qtype, qEnd)) {
        return;
    }

    standaloneState.q_total++;
    standaloneState.t_udp++;
    String qtypeStr = TravelDnsEngine::qtypeToString(qtype);
    uint8_t respBuf[DNS_MAX_PACKET];

    // 1. Check built-in .numa local domains
    if (domain == "numa.numa" || domain == "travel.numa" || domain == "adblock.numa" || domain == "epaper.numa") {
        IPAddress myIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
        int rLen = TravelDnsEngine::buildIpResponse(reqBuf, qEnd, qtype, myIp, 60, respBuf);
        travelDns.udpClient.beginPacket(clientIp, clientPort);
        travelDns.udpClient.write(respBuf, rLen);
        travelDns.udpClient.endPacket();

        standaloneState.q_local++;
        float latMs = (micros() - t0) / 1000.0f;
        pushStandaloneLog(domain, qtypeStr, "LOCAL", "NOERROR", latMs, clientIp.toString());
        needsRender = true;
        return;
    }

    // 2. Check Ad-Block Engine (202,302 Hagezi Pro domains in PSRAM + custom rules)
    bool blockingActive = standaloneState.blocking_enabled && !standaloneState.blocking_paused;
    if (blockingActive && travelDns.isDomainBlocked(domain)) {
        int rLen = TravelDnsEngine::buildIpResponse(reqBuf, qEnd, qtype, IPAddress(0, 0, 0, 0), 60, respBuf);
        travelDns.udpClient.beginPacket(clientIp, clientPort);
        travelDns.udpClient.write(respBuf, rLen);
        travelDns.udpClient.endPacket();

        standaloneState.q_blocked++;
        float latMs = (micros() - t0) / 1000.0f;
        pushStandaloneLog(domain, qtypeStr, "BLOCKED", "0.0.0.0", latMs, clientIp.toString());
        needsRender = true;
        return;
    }

    // 3. Check PSRAM Wire-Level DNS Cache
    int cachedLen = travelDns.lookupCache(domain, qtype, txid, respBuf);
    if (cachedLen > 0) {
        travelDns.udpClient.beginPacket(clientIp, clientPort);
        travelDns.udpClient.write(respBuf, cachedLen);
        travelDns.udpClient.endPacket();

        standaloneState.q_cached++;
        float latMs = (micros() - t0) / 1000.0f;
        pushStandaloneLog(domain, qtypeStr, "CACHED", "NOERROR", latMs, clientIp.toString());
        needsRender = true;
        return;
    }

    // 4. Forward to Upstream DNS (9.9.9.9 / 1.1.1.1) if connected to Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
        travelDns.udpUpstream.beginPacket(travelDns.upstreamIp, 53);
        travelDns.udpUpstream.write(reqBuf, len);
        travelDns.udpUpstream.endPacket();

        unsigned long waitStart = millis();
        int uLen = 0;
        while (millis() - waitStart < 1200) {
            int p = travelDns.udpUpstream.parsePacket();
            if (p >= 12) {
                uLen = travelDns.udpUpstream.read(respBuf, DNS_MAX_PACKET);
                uint16_t rxTxid = ((uint16_t)respBuf[0] << 8) | respBuf[1];
                if (rxTxid == txid) break;
                uLen = 0;
            }
            delay(1);
        }

        if (uLen > 0) {
            travelDns.udpClient.beginPacket(clientIp, clientPort);
            travelDns.udpClient.write(respBuf, uLen);
            travelDns.udpClient.endPacket();

            travelDns.storeCache(domain, qtype, respBuf, uLen, 180);
            standaloneState.q_upstream++;
            float latMs = (micros() - t0) / 1000.0f;
            pushStandaloneLog(domain, qtypeStr, "UPSTREAM", "NOERROR", latMs, clientIp.toString());
            needsRender = true;
            return;
        }
    }

    // In captive-portal AP-only mode (no upstream internet yet), resolve captive checks to AP IP
    IPAddress apIp = WiFi.softAPIP();
    int rLen = TravelDnsEngine::buildIpResponse(reqBuf, qEnd, qtype, apIp, 15, respBuf);
    travelDns.udpClient.beginPacket(clientIp, clientPort);
    travelDns.udpClient.write(respBuf, rLen);
    travelDns.udpClient.endPacket();
}

// ============================================================================
// Command Dispatch & Web Server Routes
// ============================================================================

static void sendCommand(const char *cmd, int arg = 0) {
    JsonDocument doc;
    doc["cmd"] = cmd;
    if (arg != 0) doc["arg"] = arg;
    doc["page"] = currentPage;
    String out;
    serializeJson(doc, out);
    Serial.println(out);
}

static void toggleAdBlockAction() {
    bool nextState = !(standaloneState.blocking_enabled && !standaloneState.blocking_paused);
    standaloneState.blocking_enabled = nextState;
    standaloneState.blocking_paused = false;
    state.blocking_enabled = nextState;
    state.blocking_paused = false;
    showToast(nextState ? ">> AD BLOCKING: ENABLED <<" : ">> AD BLOCKING: DISABLED <<", 2200);
    sendCommand("toggle_blocking", nextState ? 1 : 0);
}

static void pauseAdBlockAction(int secs = 300) {
    standaloneState.blocking_paused = true;
    state.blocking_paused = true;
    pauseUntilMs = millis() + (unsigned long)secs * 1000UL;
    showToast(">> AD BLOCKING PAUSED (5 MIN) <<", 2500);
    sendCommand("pause_blocking", secs);
}

static void flushCacheAction() {
    travelDns.flushCache();
    standaloneState.cache_entries = 0;
    state.cache_entries = 0;
    showToast(">> DNS CACHE FLUSHED <<", 2200);
    sendCommand("flush_cache", 0);
}

static void setupWebServer() {
    webServer.on("/", HTTP_GET, []() {
        webServer.send_P(200, "text/html; charset=utf-8", TRAVEL_WEBUI_HTML);
    });

    // Captive portal auto-popup endpoints for iOS / Android / macOS
    webServer.on("/generate_204", HTTP_GET, []() {
        webServer.sendHeader("Location", "http://192.168.4.1/", true);
        webServer.send(302, "text/plain", "");
    });
    webServer.on("/hotspot-detect.html", HTTP_GET, []() {
        webServer.send_P(200, "text/html; charset=utf-8", TRAVEL_WEBUI_HTML);
    });

    webServer.on("/api/status", HTTP_GET, []() {
        const NumaState &st = activeState();
        JsonDocument doc;
        doc["mode"] = isStandaloneActive() ? "standalone" : "usb_bridge";
        doc["ip"] = currentDeviceIp();
        doc["total"] = st.q_total;
        doc["blocked"] = st.q_blocked;
        doc["cached"] = st.q_cached;
        doc["cache_entries"] = st.cache_entries;
        doc["blocking"] = st.blocking_enabled && !st.blocking_paused;
        doc["battery_v"] = readBatteryVoltage();
        doc["free_psram_kb"] = ESP.getFreePsram() / 1024;
        JsonArray arr = doc["logs"].to<JsonArray>();
        for (int i = 0; i < st.log_count; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["domain"] = st.logs[i].domain;
            o["qtype"] = st.logs[i].qtype;
            o["path"] = st.logs[i].path;
            o["latency_ms"] = st.logs[i].latency_ms;
        }
        String out;
        serializeJson(doc, out);
        webServer.send(200, "application/json", out);
    });

    webServer.on("/api/cmd", HTTP_POST, []() {
        String c = webServer.arg("c");
        if (c == "toggle") toggleAdBlockAction();
        else if (c == "pause") pauseAdBlockAction(300);
        else if (c == "flush") flushCacheAction();
        else if (c == "page") {
            currentPage = (currentPage + 1) % TOTAL_PAGES;
            needsRender = true;
        } else if (c == "refresh") {
            forceFullClear = true;
            needsRender = true;
        }
        webServer.send(200, "application/json", "{\"ok\":true}");
    });

    webServer.on("/api/check", HTTP_GET, []() {
        String d = webServer.arg("d");
        String rule = "";
        bool blk = travelDns.isDomainBlocked(d, &rule);
        JsonDocument doc;
        doc["domain"] = d;
        doc["blocked"] = blk;
        doc["rule"] = rule;
        String out;
        serializeJson(doc, out);
        webServer.send(200, "application/json", out);
    });

    webServer.on("/api/rule", HTTP_POST, []() {
        String kind = webServer.arg("kind");
        String d = webServer.arg("d");
        if (kind == "block") {
            travelDns.addCustomBlock(d, prefs);
            showToast("BLOCKED: " + truncateStr(d, 20), 2000);
        } else if (kind == "allow") {
            travelDns.addCustomAllow(d, prefs);
            showToast("ALLOWED: " + truncateStr(d, 20), 2000);
        }
        webServer.send(200, "application/json", "{\"ok\":true}");
    });

    webServer.on("/api/wifi", HTTP_POST, []() {
        String s = webServer.arg("ssid");
        String p = webServer.arg("pass");
        String u = webServer.arg("up");
        if (s.length() > 0) {
            wifiSsid = s;
            wifiPass = p;
            prefs.putString("ssid", wifiSsid);
            prefs.putString("pass", wifiPass);
        }
        if (u.length() > 0) {
            prefs.putString("upstream", u);
            travelDns.upstreamIp.fromString(u);
        }
        webServer.send(200, "application/json", "{\"ok\":true}");
        delay(200);
        if (wifiSsid.length() > 0) {
            WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
            unsigned long t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 7000) {
                delay(100);
            }
            if (WiFi.status() == WL_CONNECTED) {
                showToast("WIFI CONNECTED: " + WiFi.localIP().toString(), 2500);
            } else {
                showToast("WIFI CONNECT FAILED (AP ACTIVE)", 2500);
            }
        }
    });

    webServer.begin();
}

// ============================================================================
// USB-C Serial Bridge Parser
// ============================================================================

static bool parseBridgePayload(const String &jsonLine) {
    JsonDocument doc;
    if (deserializeJson(doc, jsonLine)) return false;

    const char *msgType = doc["type"] | "state";
    if (strcmp(msgType, "config") == 0) {
        if (doc["wifi_ssid"].is<const char*>()) {
            wifiSsid = doc["wifi_ssid"].as<String>();
            prefs.putString("ssid", wifiSsid);
        }
        if (doc["wifi_pass"].is<const char*>()) {
            wifiPass = doc["wifi_pass"].as<String>();
            prefs.putString("pass", wifiPass);
        }
        if (doc["numa_host"].is<const char*>()) {
            numaHost = doc["numa_host"].as<String>();
            prefs.putString("host", numaHost);
        }
        if (wifiSsid.length() > 0) {
            WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        }
        showToast("TRAVEL WIFI SAVED TO FLASH");
        return true;
    }

    if (strcmp(msgType, "page") == 0) {
        currentPage = (doc["page"] | 0) % TOTAL_PAGES;
        needsRender = true;
        return true;
    }

    if (strcmp(msgType, "state") != 0) return false;

    state.connected = true;
    state.transport_mode = "USB-C";
    state.version = doc["version"] | state.version;
    state.uptime_secs = doc["uptime_secs"] | state.uptime_secs;
    state.upstream = doc["upstream"] | state.upstream;
    state.mode = doc["mode"] | state.mode;
    state.lan_ip = doc["lan_ip"] | state.lan_ip;
    state.dns_port = doc["dns_port"] | state.dns_port;
    state.api_port = doc["api_port"] | state.api_port;

    JsonObject q = doc["queries"];
    if (!q.isNull()) {
        state.q_total      = q["total"] | 0ULL;
        state.q_cached     = q["cached"] | 0ULL;
        state.q_upstream   = q["upstream"] | 0ULL;
        state.q_local      = q["local"] | 0ULL;
        state.q_blocked    = q["blocked"] | 0ULL;
        state.q_overridden = q["overridden"] | 0ULL;
        state.q_errors     = q["errors"] | 0ULL;
    }

    JsonObject c = doc["cache"];
    if (!c.isNull()) {
        state.cache_entries = c["entries"] | 0U;
    }

    JsonObject bl = doc["blocking"];
    if (!bl.isNull()) {
        state.blocking_enabled = bl["enabled"] | true;
        state.blocking_paused  = bl["paused"] | false;
        state.domains_loaded   = bl["domains_loaded"] | HAGEZI_DOMAIN_COUNT;
    }

    JsonArray logsArr = doc["logs"].as<JsonArray>();
    if (!logsArr.isNull()) {
        state.log_count = 0;
        for (JsonObject item : logsArr) {
            if (state.log_count >= 8) break;
            state.logs[state.log_count].domain     = item["domain"] | "";
            state.logs[state.log_count].qtype      = item["query_type"] | "A";
            state.logs[state.log_count].path       = item["path"] | "";
            state.logs[state.log_count].rcode      = item["rescode"] | "NOERROR";
            state.logs[state.log_count].latency_ms = item["latency_ms"] | 0.0f;
            state.log_count++;
        }
    }

    JsonArray svcArr = doc["services"].as<JsonArray>();
    if (!svcArr.isNull()) {
        state.service_count = 0;
        for (JsonObject s : svcArr) {
            if (state.service_count >= 6) break;
            state.services[state.service_count].name = s["name"] | "";
            state.services[state.service_count].port = s["target_port"] | 0;
            state.service_count++;
        }
    }

    if (doc["force_render"] | false) {
        needsRender = true;
    }
    return true;
}

// ============================================================================
// Physical Button Handlers
// ============================================================================

static void setupButtons() {
    // S4 (GPIO 39): Cycle Page (1..4)
    btnPage.setClickHandler([](Button2 &b) {
        currentPage = (currentPage + 1) % TOTAL_PAGES;
        const char *names[4] = {
            "PAGE 1/4: OVERVIEW",
            "PAGE 2/4: QUERY LOG",
            "PAGE 3/4: TRAVEL SETUP",
            "PAGE 4/4: .NUMA & RULES"
        };
        showToast(names[currentPage], 1800);
        sendCommand("page_changed", currentPage);
    });

    // S3 (GPIO 34): Short press = Toggle Ad Blocking, Long press = Pause 5m
    btnBlock.setClickHandler([](Button2 &b) {
        toggleAdBlockAction();
    });
    btnBlock.setLongClickTime(800);
    btnBlock.setLongClickDetectedHandler([](Button2 &b) {
        pauseAdBlockAction(300);
    });

    // S2 (GPIO 35): Flush DNS Cache
    btnCache.setClickHandler([](Button2 &b) {
        flushCacheAction();
    });

    // S5 (GPIO 0): Short press = Refresh E-Ink, Long press = Toggle Travel Standalone vs USB-C Mode
    btnSync.setClickHandler([](Button2 &b) {
        forceFullClear = true;
        showToast(">> REFRESHING E-PAPER... <<", 1500);
        sendCommand("refresh", 1);
    });
    btnSync.setLongClickTime(800);
    btnSync.setLongClickDetectedHandler([](Button2 &b) {
        forceStandaloneMode = !forceStandaloneMode;
        if (forceStandaloneMode) {
            currentPage = 2; // Jump to Travel Setup page to show DNS IP & Hotspot info
            showToast(">> MODE: TRAVEL STANDALONE DNS (:53) <<", 2500);
        } else {
            showToast(">> MODE: AUTO (USB-C / TRAVEL) <<", 2500);
        }
    });
}

// ============================================================================
// Setup & Main Loop
// ============================================================================

void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.setRxBufferSize(8192);

    prefs.begin("numa-epd", false);
    wifiSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
    wifiPass = prefs.getString("pass", DEFAULT_WIFI_PASS);
    numaHost = prefs.getString("host", DEFAULT_NUMA_HOST);
    numaPort = prefs.getUShort("port", DEFAULT_NUMA_PORT);

    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }

    epd_init();
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (framebuffer) {
        memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    }

    // Initialize 202,302 Hagezi Pro domains in PSRAM + 256-slot PSRAM wire cache
    travelDns.begin(prefs);

    // Start Dual-Mode Wi-Fi: Travel Setup AP ("Numa-Travel-DNS" @ 192.168.4.1) + Station
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(TRAVEL_AP_SSID);
    if (wifiSsid.length() > 0) {
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    MDNS.begin("numa-travel");

    // Start Standalone UDP Port 53 DNS Server & Port 80 Web UI
    travelDns.startServer();
    setupWebServer();

    setupButtons();
    renderDashboard(true);

    Serial.println("{\"event\":\"boot\",\"board\":\"LILYGO_T5_47_ESP32\",\"mode\":\"hybrid_travel_dns\",\"domains\":202302}");
}

void loop() {
    btnPage.loop();
    btnBlock.loop();
    btnCache.loop();
    btnSync.loop();

    // Always serve Standalone UDP Port 53 DNS queries & Port 80 Web UI!
    processStandaloneDns();
    webServer.handleClient();

    // Auto-unpause ad blocking when pause timer expires
    if (standaloneState.blocking_paused && pauseUntilMs > 0 && millis() >= pauseUntilMs) {
        standaloneState.blocking_paused = false;
        pauseUntilMs = 0;
        needsRender = true;
    }

    // Read incoming JSON lines from Mac USB-C Serial Bridge
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            serialLineBuf.trim();
            if (serialLineBuf.startsWith("{") && serialLineBuf.endsWith("}")) {
                uint64_t prevTotal = state.q_total;
                bool prevBlock = state.blocking_enabled;
                bool prevPaused = state.blocking_paused;
                uint32_t prevCache = state.cache_entries;
                bool wasStandalone = isStandaloneActive();

                if (parseBridgePayload(serialLineBuf)) {
                    lastSerialRxMs = millis();
                    if (wasStandalone ||
                        state.q_total != prevTotal ||
                        state.blocking_enabled != prevBlock ||
                        state.blocking_paused != prevPaused ||
                        state.cache_entries != prevCache ||
                        millis() - lastRenderMs > 30000) {
                        needsRender = true;
                    }
                }
            }
            serialLineBuf = "";
        } else if (serialLineBuf.length() < 12000) {
            serialLineBuf += c;
        } else {
            serialLineBuf = "";
        }
    }

    if (toastMessage.length() > 0 && millis() >= toastUntilMs) {
        toastMessage = "";
        needsRender = true;
    }

    if (millis() - lastHelloTxMs > 5000) {
        lastHelloTxMs = millis();
        Serial.print("{\"event\":\"heartbeat\",\"page\":");
        Serial.print(currentPage);
        Serial.print(",\"standalone\":");
        Serial.print(isStandaloneActive() ? "true" : "false");
        Serial.println("}");
    }

    if (needsRender) {
        renderDashboard(forceFullClear);
    }

    delay(2);
}
