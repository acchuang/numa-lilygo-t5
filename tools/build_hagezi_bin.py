#!/usr/bin/env python3
"""
Downloads Hagezi Pro wildcard domain blocklist (same list used by Numa)
and compiles all unique domains into sorted 40-bit (5-byte) FNV-1a hashes
for O(log N) binary search in ESP32 Flash / PSRAM.
"""

import os
import struct
import urllib.request

HAGEZI_PRO_URL = "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro-onlydomains.txt"
OUT_BIN = os.path.join(os.path.dirname(__file__), "..", "firmware", "include", "hagezi_hashes.bin")
OUT_META = os.path.join(os.path.dirname(__file__), "..", "firmware", "include", "hagezi_meta.h")

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3


def fnv1a_40(domain: str) -> int:
    h = FNV64_OFFSET
    for b in domain.encode("ascii", errors="ignore"):
        h ^= b
        h = (h * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    # Fold top 24 bits into lower 40 bits for maximum entropy
    return ((h >> 40) ^ h) & 0xFFFFFFFFFF


def main():
    print(f"Downloading Hagezi Pro blocklist from {HAGEZI_PRO_URL} ...")
    req = urllib.request.Request(HAGEZI_PRO_URL, headers={"User-Agent": "numa-lilygo-builder/1.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        raw = resp.read().decode("utf-8", errors="ignore")

    domains = set()
    for line in raw.splitlines():
        line = line.strip().lower()
        if not line or line.startswith("#") or line.startswith("!"):
            continue
        if line.startswith("*."):
            line = line[2:]
        line = line.rstrip(".")
        if line and "." in line:
            domains.add(line)

    # Ensure common test/ad domains are always present
    domains.update([
        "doubleclick.net",
        "ad.doubleclick.net",
        "pagead2.googlesyndication.com",
        "googleadservices.com",
        "ads.google.com",
        "ads.yahoo.com",
        "analytics.google.com",
        "app-measurement.com",
        "unityads.unity3d.com",
    ])

    hashes = sorted({fnv1a_40(d) for d in domains})
    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)

    with open(OUT_BIN, "wb") as f:
        for h in hashes:
            # 5-byte little-endian (uint32 low + uint8 high)
            f.write(struct.pack("<IB", h & 0xFFFFFFFF, (h >> 32) & 0xFF))

    with open(OUT_META, "w") as f:
        f.write("#pragma once\n")
        f.write(f"#define HAGEZI_DOMAIN_COUNT {len(domains)}U\n")
        f.write(f"#define HAGEZI_HASH_COUNT {len(hashes)}U\n")
        f.write(f"#define HAGEZI_HASH_BYTES {len(hashes) * 5}U\n")

    print(f"Compiled {len(domains):,} unique Hagezi Pro domains -> {len(hashes):,} sorted 40-bit hashes ({len(hashes)*5:,} bytes)")
    print(f"Wrote: {OUT_BIN}")
    print(f"Wrote: {OUT_META}")


if __name__ == "__main__":
    main()
