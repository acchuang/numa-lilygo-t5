# Numa × LilyGO T5 4.7" E-Paper — Portable Travel DNS AdBlocker & Dashboard

A dual-mode **Standalone Portable Travel DNS AdBlocker (`UDP :53`)** and **Real-Time E-Paper Hardware Dashboard & Controller** built for the **LilyGO T5 4.7" E-Paper (`ESP32-D0WD-V3`, 16MB Flash, 8MB PSRAM, 960×540 16-grayscale)** paired with [Numa DNS](https://github.com/razvandimescu/numa).

---

## Key Features

### 1. Standalone Travel DNS Sinkhole (No Computer Needed)
- **202,302 Hagezi Pro Domains Embedded On-Device**: Compiles the full [Hagezi Pro](https://github.com/hagezi/dns-blocklists) wildcard blocklist into a **1.01 MB sorted 40-bit FNV-1a hash table** (`firmware/include/hagezi_hashes.bin`) embedded in Flash and copied into **8MB PSRAM** at boot.
- **`< 5 µs` Wildcard Subdomain Matching**: Binary-searches exact domains and parent suffixes (`ads.sub.example.com` → `sub.example.com` → `example.com`) in PSRAM in ~18 steps with zero collisions.
- **256-Slot PSRAM Wire-Level DNS Cache**: Serves cached DNS responses in `< 1 ms` with in-place transaction ID (`txid`) patching.
- **Always-On Travel Setup Hotspot (`Numa-Travel-DNS`)**: Connect from your phone or laptop (`http://192.168.4.1` or `http://numa.numa`) to join hotel/cafe/hotspot Wi-Fi networks, test domains, or add persistent custom block/allow rules (stored in ESP32 NVS Flash).

### 2. Mac USB-C Serial Bridge + Live E-Paper Mirror (`bridge.py`)
- When plugged into your Mac via USB-C, `bridge.py` streams real-time telemetry from a local [Numa](https://github.com/razvandimescu/numa) daemon (`http://localhost:5380`) over `/dev/cu.usbserial-*` (`115200` baud).
- Hosts a **Live 960×540 E-Paper Mirror & Controller Web UI** at `http://localhost:5381` (and `http://epaper.numa:8080`).

---

## Physical Button Controls (LilyGO T5 4.7" Silkscreen)

| Silkscreen | ESP32 GPIO | Short Press | Long Press (0.8s) |
| :--- | :--- | :--- | :--- |
| **`S4`** | `GPIO 39` | **Cycle Page** (`1/4 Overview` → `2/4 Query Log` → `3/4 Travel Setup` → `4/4 .numa & Custom Rules`) | — |
| **`S3`** | `GPIO 34` | **Toggle Ad Blocking** (`ON` / `OFF`) | **Pause Ad Blocking (5 min)** |
| **`S2`** | `GPIO 35` | **Flush PSRAM DNS Cache** | — |
| **`S5`** | `GPIO 0` | **Force Full Clean E-Ink Refresh** | **Toggle Standalone Travel Mode (`:53`) vs Mac USB-C Mode** |
| **`S6`** | `RST` | **Hardware Reboot** | — |

---

## Project Structure

```text
numa-lilygo-t5/
├── bridge.py                        # macOS USB-C Serial Bridge + Live 960x540 Web Mirror (:5381)
├── numa.toml                        # Numa DNS & Reverse Proxy configuration (:5553 DNS, :5380 API)
├── start.sh                         # One-command launcher for Numa + USB-C Bridge
├── tools/
│   └── build_hagezi_bin.py          # Compiles 202.3K Hagezi Pro domains -> sorted 40-bit FNV-1a hashes
└── firmware/
    ├── platformio.ini               # PlatformIO config for LilyGO T5 4.7" (ESP32-D0WD-V3, 16MB Flash, PSRAM)
    ├── boards/T5-ePaper.json        # Board definition
    ├── include/
    │   ├── config.h                 # GPIO pinout (S4/S3/S2/S5) & network defaults
    │   ├── travel_dns.h             # On-device UDP :53 DNS sinkhole, PSRAM cache & FNV-1a matcher
    │   ├── travel_webui.h           # Mobile Web UI & Captive Portal served on ESP32 port 80
    │   ├── hagezi_hashes.bin        # 202,302 sorted 5-byte (40-bit) FNV-1a domain hashes
    │   └── hagezi_meta.h            # Hash table metadata
    └── src/
        ├── hagezi_blob.S            # Assembler .incbin directive embedding hash table into .rodata
        └── main.cpp                 # 4-page E-Paper renderer, button handlers & hybrid controller
```

---

## Quick Start

### 1. Build & Flash Firmware to LilyGO T5 4.7"

```zsh
# Optional: Refresh the 202K+ Hagezi Pro 40-bit hash table
python3 tools/build_hagezi_bin.py

# Compile & upload firmware via PlatformIO
cd firmware
uvx --from platformio pio run -t upload
```

### 2. Run on macOS (USB-C Bridge + Numa Daemon)

```zsh
./start.sh
```
- **Numa Dashboard**: `http://localhost:5380`
- **LilyGO Companion & Live 960×540 Mirror**: `http://localhost:5381` (or `http://epaper.numa:8080`)

### 3. Use Standalone While Travelling
1. Power the LilyGO T5 4.7" from any USB-C power bank, phone charger, or 3.7V LiPo battery.
2. Connect your phone/laptop to the **`Numa-Travel-DNS`** Wi-Fi hotspot and open **`http://192.168.4.1`**.
3. Pair the board to your hotel, cafe, or phone hotspot Wi-Fi.
4. Press **`S4`** on the board to view **Page 3/4 (`PORTABLE TRAVEL ADBLOCKER`)** and set your phone/laptop DNS to the IP address displayed on the E-Paper screen.
