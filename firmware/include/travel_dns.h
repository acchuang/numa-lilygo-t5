#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "hagezi_meta.h"

extern "C" {
    extern const uint8_t hagezi_hashes_start[];
    extern const uint8_t hagezi_hashes_end[];
}

// ============================================================================
// 40-bit FNV-1a Hash & Binary Search Engine (202,302 Hagezi Pro Domains in PSRAM)
// ============================================================================

static uint8_t *g_psramHashes = nullptr;
static uint32_t g_hashCount = HAGEZI_HASH_COUNT;

static inline uint64_t fnv1a_40_hash(const char *str, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)str[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= c;
        h *= 0x100000001B3ULL;
    }
    return ((h >> 40) ^ h) & 0xFFFFFFFFFFULL;
}

static inline uint64_t read_hash40(const uint8_t *base, uint32_t idx) {
    const uint8_t *p = base + (idx * 5);
    uint64_t lo = (uint32_t)p[0] |
                  ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) |
                  ((uint32_t)p[3] << 24);
    uint64_t hi = (uint64_t)p[4];
    return lo | (hi << 32);
}

static bool binarySearchHash40(uint64_t target) {
    const uint8_t *table = g_psramHashes ? g_psramHashes : hagezi_hashes_start;
    int32_t lo = 0;
    int32_t hi = (int32_t)g_hashCount - 1;
    while (lo <= hi) {
        int32_t mid = lo + ((hi - lo) >> 1);
        uint64_t val = read_hash40(table, (uint32_t)mid);
        if (val == target) return true;
        if (val < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

static void initPsramBlocklist() {
    g_psramHashes = (uint8_t *)ps_malloc(HAGEZI_HASH_BYTES);
    if (g_psramHashes) {
        memcpy(g_psramHashes, hagezi_hashes_start, HAGEZI_HASH_BYTES);
    }
}

// ============================================================================
// Custom Allowlist / Blocklist + PSRAM Wire Cache
// ============================================================================

#define MAX_CUSTOM_DOMAINS 32
#define DNS_CACHE_SLOTS    256
#define DNS_MAX_PACKET     512

struct DnsCacheSlot {
    bool valid = false;
    uint64_t key_hash = 0;
    uint16_t qtype = 0;
    uint32_t expire_ms = 0;
    uint16_t resp_len = 0;
    uint8_t packet[DNS_MAX_PACKET];
};

class TravelDnsEngine {
public:
    WiFiUDP udpClient;
    WiFiUDP udpUpstream;
    DnsCacheSlot *cache = nullptr;
    IPAddress upstreamIp = IPAddress(9, 9, 9, 9);
    IPAddress fallbackIp = IPAddress(1, 1, 1, 1);

    String customBlock[MAX_CUSTOM_DOMAINS];
    int customBlockCount = 0;
    String customAllow[MAX_CUSTOM_DOMAINS];
    int customAllowCount = 0;

    bool running = false;

    void begin(Preferences &prefs) {
        initPsramBlocklist();
        if (!cache) {
            cache = (DnsCacheSlot *)ps_calloc(DNS_CACHE_SLOTS, sizeof(DnsCacheSlot));
        }
        String upStr = prefs.getString("upstream", "9.9.9.9");
        upstreamIp.fromString(upStr);
        loadCustomLists(prefs);
        udpUpstream.begin(55053);
    }

    void startServer() {
        if (!running) {
            udpClient.begin(53);
            running = true;
        }
    }

    void loadCustomLists(Preferences &prefs) {
        customBlockCount = 0;
        customAllowCount = 0;
        String bCsv = prefs.getString("c_block", "");
        String aCsv = prefs.getString("c_allow", "");
        parseCsvList(bCsv, customBlock, customBlockCount);
        parseCsvList(aCsv, customAllow, customAllowCount);
    }

    void saveCustomLists(Preferences &prefs) {
        prefs.putString("c_block", joinCsvList(customBlock, customBlockCount));
        prefs.putString("c_allow", joinCsvList(customAllow, customAllowCount));
    }

    bool addCustomBlock(const String &dom, Preferences &prefs) {
        String d = dom;
        d.trim();
        d.toLowerCase();
        if (d.length() == 0 || customBlockCount >= MAX_CUSTOM_DOMAINS) return false;
        for (int i = 0; i < customBlockCount; i++) {
            if (customBlock[i] == d) return true;
        }
        customBlock[customBlockCount++] = d;
        saveCustomLists(prefs);
        return true;
    }

    bool addCustomAllow(const String &dom, Preferences &prefs) {
        String d = dom;
        d.trim();
        d.toLowerCase();
        if (d.length() == 0 || customAllowCount >= MAX_CUSTOM_DOMAINS) return false;
        for (int i = 0; i < customAllowCount; i++) {
            if (customAllow[i] == d) return true;
        }
        customAllow[customAllowCount++] = d;
        saveCustomLists(prefs);
        return true;
    }

    uint32_t activeCacheCount() const {
        if (!cache) return 0;
        uint32_t cnt = 0;
        uint32_t now = millis();
        for (int i = 0; i < DNS_CACHE_SLOTS; i++) {
            if (cache[i].valid && (int32_t)(cache[i].expire_ms - now) > 0) cnt++;
        }
        return cnt;
    }

    void flushCache() {
        if (!cache) return;
        for (int i = 0; i < DNS_CACHE_SLOTS; i++) {
            cache[i].valid = false;
        }
    }

    // Checks exact domain AND all parent suffixes against Allowlist, Custom Blocklist, and 202.3K Hagezi Pro
    bool isDomainBlocked(const String &domain, String *matchedRule = nullptr) const {
        String d = domain;
        d.toLowerCase();
        if (d.endsWith(".")) d.remove(d.length() - 1);

        // 1. Check Custom Allowlist (suffix-aware)
        for (int i = 0; i < customAllowCount; i++) {
            if (d == customAllow[i] || d.endsWith("." + customAllow[i])) {
                if (matchedRule) *matchedRule = "allow:" + customAllow[i];
                return false;
            }
        }

        // 2. Check Custom Blocklist (suffix-aware)
        for (int i = 0; i < customBlockCount; i++) {
            if (d == customBlock[i] || d.endsWith("." + customBlock[i])) {
                if (matchedRule) *matchedRule = "custom:" + customBlock[i];
                return true;
            }
        }

        // 3. Walk domain labels and binary-search 202,302 Hagezi Pro 40-bit hashes in PSRAM
        const char *cstr = d.c_str();
        size_t len = d.length();
        const char *cur = cstr;
        while (cur && *cur) {
            const char *dot = strchr(cur, '.');
            if (!dot) break; // reached TLD alone
            size_t subLen = (size_t)(cstr + len - cur);
            uint64_t h = fnv1a_40_hash(cur, subLen);
            if (binarySearchHash40(h)) {
                if (matchedRule) *matchedRule = String(cur);
                return true;
            }
            cur = dot + 1;
        }
        return false;
    }

    static const char *qtypeToString(uint16_t qtype) {
        switch (qtype) {
            case 1: return "A";
            case 28: return "AAAA";
            case 5: return "CNAME";
            case 15: return "MX";
            case 16: return "TXT";
            case 33: return "SRV";
            case 64: return "SVCB";
            case 65: return "HTTPS";
            default: return "OTHER";
        }
    }

    // Parse DNS question name and qtype/qclass from wire packet
    static bool parseDnsQuestion(const uint8_t *buf, int len, String &outDomain, uint16_t &outQtype, int &outQuestionEnd) {
        if (len < 12) return false;
        uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
        if (qdcount == 0) return false;

        int pos = 12;
        outDomain = "";
        while (pos < len) {
            uint8_t labelLen = buf[pos++];
            if (labelLen == 0) break;
            if ((labelLen & 0xC0) != 0) return false; // compression in first question unsupported
            if (pos + labelLen > len) return false;
            if (outDomain.length() > 0) outDomain += '.';
            for (uint8_t i = 0; i < labelLen; i++) {
                char c = (char)buf[pos++];
                if (c >= 'A' && c <= 'Z') c += 32;
                outDomain += c;
            }
        }
        if (pos + 4 > len) return false;
        outQtype = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        outQuestionEnd = pos + 4;
        return outDomain.length() > 0;
    }

    // Build an A record (or NODATA for non-A) DNS response
    static int buildIpResponse(const uint8_t *req, int qEnd, uint16_t qtype, IPAddress ip, uint32_t ttl, uint8_t *outBuf) {
        memcpy(outBuf, req, qEnd);
        // Header flags: QR=1, Opcode=0, AA=1, TC=0, RD=1, RA=1, RCODE=0 (0x8580)
        outBuf[2] = 0x85;
        outBuf[3] = 0x80;
        // QDCOUNT = 1
        outBuf[4] = 0x00;
        outBuf[5] = 0x01;
        // NSCOUNT = 0, ARCOUNT = 0
        outBuf[8] = 0x00; outBuf[9] = 0x00;
        outBuf[10] = 0x00; outBuf[11] = 0x00;

        if (qtype == 1) {
            // ANCOUNT = 1 (IPv4 A record)
            outBuf[6] = 0x00;
            outBuf[7] = 0x01;
            int p = qEnd;
            outBuf[p++] = 0xC0; outBuf[p++] = 0x0C; // pointer to QNAME at offset 12
            outBuf[p++] = 0x00; outBuf[p++] = 0x01; // TYPE A
            outBuf[p++] = 0x00; outBuf[p++] = 0x01; // CLASS IN
            outBuf[p++] = (ttl >> 24) & 0xFF;
            outBuf[p++] = (ttl >> 16) & 0xFF;
            outBuf[p++] = (ttl >> 8) & 0xFF;
            outBuf[p++] = ttl & 0xFF;
            outBuf[p++] = 0x00; outBuf[p++] = 0x04; // RDLENGTH 4
            outBuf[p++] = ip[0];
            outBuf[p++] = ip[1];
            outBuf[p++] = ip[2];
            outBuf[p++] = ip[3];
            return p;
        } else if (qtype == 28 && ip == IPAddress(0, 0, 0, 0)) {
            // Blocked AAAA -> :: (16 zero bytes)
            outBuf[6] = 0x00;
            outBuf[7] = 0x01;
            int p = qEnd;
            outBuf[p++] = 0xC0; outBuf[p++] = 0x0C;
            outBuf[p++] = 0x00; outBuf[p++] = 0x1C; // TYPE AAAA
            outBuf[p++] = 0x00; outBuf[p++] = 0x01; // CLASS IN
            outBuf[p++] = (ttl >> 24) & 0xFF;
            outBuf[p++] = (ttl >> 16) & 0xFF;
            outBuf[p++] = (ttl >> 8) & 0xFF;
            outBuf[p++] = ttl & 0xFF;
            outBuf[p++] = 0x00; outBuf[p++] = 0x10; // RDLENGTH 16
            memset(outBuf + p, 0, 16);
            return p + 16;
        } else {
            // NODATA (ANCOUNT = 0, NOERROR) for HTTPS/SVCB or AAAA on IPv4 local names
            outBuf[6] = 0x00;
            outBuf[7] = 0x00;
            return qEnd;
        }
    }

    int lookupCache(const String &domain, uint16_t qtype, uint16_t txid, uint8_t *outBuf) {
        if (!cache) return 0;
        uint64_t h = fnv1a_40_hash(domain.c_str(), domain.length());
        uint32_t slotIdx = (uint32_t)(h % DNS_CACHE_SLOTS);
        DnsCacheSlot &slot = cache[slotIdx];
        if (!slot.valid || slot.key_hash != h || slot.qtype != qtype) return 0;
        if ((int32_t)(slot.expire_ms - millis()) <= 0) {
            slot.valid = false;
            return 0;
        }
        memcpy(outBuf, slot.packet, slot.resp_len);
        outBuf[0] = (txid >> 8) & 0xFF;
        outBuf[1] = txid & 0xFF;
        return slot.resp_len;
    }

    void storeCache(const String &domain, uint16_t qtype, const uint8_t *resp, int respLen, uint32_t ttlSecs = 120) {
        if (!cache || respLen <= 12 || respLen > DNS_MAX_PACKET) return;
        uint64_t h = fnv1a_40_hash(domain.c_str(), domain.length());
        uint32_t slotIdx = (uint32_t)(h % DNS_CACHE_SLOTS);
        DnsCacheSlot &slot = cache[slotIdx];
        slot.valid = true;
        slot.key_hash = h;
        slot.qtype = qtype;
        slot.expire_ms = millis() + (ttlSecs * 1000UL);
        slot.resp_len = (uint16_t)respLen;
        memcpy(slot.packet, resp, respLen);
    }

private:
    static void parseCsvList(const String &csv, String *arr, int &count) {
        count = 0;
        int start = 0;
        while (start < (int)csv.length() && count < MAX_CUSTOM_DOMAINS) {
            int comma = csv.indexOf(',', start);
            String token = (comma == -1) ? csv.substring(start) : csv.substring(start, comma);
            token.trim();
            if (token.length() > 0) arr[count++] = token;
            if (comma == -1) break;
            start = comma + 1;
        }
    }

    static String joinCsvList(const String *arr, int count) {
        String out = "";
        for (int i = 0; i < count; i++) {
            if (i > 0) out += ",";
            out += arr[i];
        }
        return out;
    }
};
