#!/usr/bin/env python3
"""
Numa <-> LilyGO T5 4.7" E-Paper USB-C Serial Bridge & Companion Controller
==========================================================================
- Polls local Numa API (http://127.0.0.1:5380) for stats, query logs, .numa services, and overrides
- Streams compact JSON state updates over USB-C Serial (/dev/cu.usbserial-*) to the LilyGO T5 4.7"
- Listens for physical button presses (S4 Page, S3 Ad-Block, S2 Flush Cache, S5 Sync/Mode) from the display
  and triggers the corresponding Numa REST API actions immediately
- Hosts a companion Web Controller + Live 960x540 4-Page E-Paper Mirror at http://localhost:5381
  (and http://epaper.numa:8080 via Numa's reverse proxy)
"""

import glob
import json
import os
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    serial = None

NUMA_API_BASE = os.environ.get("NUMA_API_URL", "http://127.0.0.1:5380")
NUMA_DNS_PORT = int(os.environ.get("NUMA_DNS_PORT", "5553"))
BRIDGE_HTTP_PORT = int(os.environ.get("BRIDGE_HTTP_PORT", "5381"))
SERIAL_BAUD = 115200
TOTAL_PAGES = 4


def detect_lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def find_serial_port() -> str | None:
    explicit = os.environ.get("LILYGO_PORT")
    if explicit and os.path.exists(explicit):
        return explicit
    candidates = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*"))
    return candidates[0] if candidates else None


class NumaBridge:
    def __init__(self):
        self.lock = threading.Lock()
        self.ser = None
        self.serial_port = None
        self.serial_connected = False
        self.last_rx_time = 0.0
        self.last_rx_event = "waiting"
        self.display_page = 0
        self.standalone_mode = False
        self.latest_state = {}
        self.event_log = []
        self.lan_ip = detect_lan_ip()
        self.force_next_push = True

    def log_event(self, source: str, msg: str):
        ts = time.strftime("%H:%M:%S")
        with self.lock:
            self.event_log.insert(0, {"time": ts, "source": source, "message": msg})
            self.event_log = self.event_log[:30]
        print(f"[{ts}] [{source}] {msg}", flush=True)

    def numa_request(self, path: str, method: str = "GET", body: dict | None = None):
        url = f"{NUMA_API_BASE}{path}"
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=2.5) as resp:
                raw = resp.read().decode("utf-8")
                return json.loads(raw) if raw.strip() else {}
        except Exception:
            return None

    def fetch_consolidated_state(self, force_render: bool = False) -> dict | None:
        stats = self.numa_request("/stats")
        if not stats:
            return None
        logs = self.numa_request("/query-log?limit=8") or []
        services = self.numa_request("/services") or []
        overrides = self.numa_request("/overrides") or []

        payload = {
            "type": "state",
            "force_render": force_render,
            "version": stats.get("version", "0.14.3"),
            "uptime_secs": stats.get("uptime_secs", 0),
            "upstream": stats.get("upstream", ""),
            "mode": stats.get("mode", "forward"),
            "lan_ip": self.lan_ip,
            "dns_port": NUMA_DNS_PORT,
            "api_port": 5380,
            "dnssec": stats.get("dnssec", False),
            "srtt": stats.get("srtt", True),
            "queries": stats.get("queries", {}),
            "transport": stats.get("transport", {}),
            "cache": stats.get("cache", {}),
            "blocking": stats.get("blocking", {}),
            "memory": stats.get("memory", {}),
            "logs": logs[:8],
            "services": services[:6],
            "overrides": overrides[:6],
        }
        with self.lock:
            self.latest_state = payload
        return payload

    def send_serial_json(self, data: dict) -> bool:
        with self.lock:
            if not self.ser or not self.serial_connected:
                return False
            try:
                line = json.dumps(data, separators=(",", ":")) + "\n"
                self.ser.write(line.encode("utf-8"))
                self.ser.flush()
                return True
            except Exception:
                self.serial_connected = False
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                return False

    def sync_and_push_now(self):
        st = self.fetch_consolidated_state(force_render=True)
        if st:
            self.send_serial_json(st)
        self.force_next_push = True

    def handle_device_command(self, msg: dict):
        cmd = msg.get("cmd")
        arg = msg.get("arg", 0)
        if "page" in msg:
            self.display_page = int(msg["page"]) % TOTAL_PAGES

        if cmd == "toggle_blocking":
            # Query live /blocking/stats from Numa so rapid toggles never read stale cache
            live_bl = self.numa_request("/blocking/stats") or self.latest_state.get("blocking", {})
            currently_active = live_bl.get("enabled", True) and not live_bl.get("paused", False)
            new_enabled = not currently_active

            if live_bl.get("paused", False):
                self.numa_request("/blocking/unpause", method="POST", body=None)
            self.numa_request("/blocking/toggle", method="PUT", body={"enabled": new_enabled})
            self.log_event("BUTTON S3", f"Ad Blocking {'ENABLED' if new_enabled else 'DISABLED'}")
            self.sync_and_push_now()

        elif cmd == "pause_blocking":
            secs = int(arg) if arg else 300
            # Ensure blocking is enabled before pausing so unpause/toggle behaves predictably
            self.numa_request("/blocking/toggle", method="PUT", body={"enabled": True})
            self.numa_request("/blocking/pause", method="POST", body={"duration_secs": secs})
            self.log_event("BUTTON S3 (HOLD)", f"Ad Blocking paused for {secs}s")
            self.sync_and_push_now()

        elif cmd == "unpause_blocking":
            self.numa_request("/blocking/unpause", method="POST", body=None)
            self.numa_request("/blocking/toggle", method="PUT", body={"enabled": True})
            self.log_event("BUTTON S3", "Ad Blocking unpaused & ENABLED")
            self.sync_and_push_now()

        elif cmd == "flush_cache":
            self.numa_request("/cache", method="DELETE")
            self.log_event("BUTTON S2", "DNS Cache Flushed")
            self.sync_and_push_now()

        elif cmd == "refresh":
            self.log_event("BUTTON S5", "Manual Telemetry Sync & Clean E-Ink Refresh")
            self.sync_and_push_now()

        elif cmd == "toggle_mode":
            self.standalone_mode = not self.standalone_mode
            if self.standalone_mode:
                self.display_page = 2
            self.send_serial_json({"type": "mode", "standalone": self.standalone_mode})
            mode_label = "STANDALONE TRAVEL DNS (:53)" if self.standalone_mode else "USB-C MAC NUMA BRIDGE"
            self.log_event("BUTTON S5 (HOLD)", f"Switched operating mode to {mode_label}")
            self.sync_and_push_now()

        elif cmd == "page_changed":
            page_names = [
                "1/4 Overview",
                "2/4 Query Log",
                "3/4 Travel Setup",
                "4/4 .numa & Custom Rules",
            ]
            p = int(arg) % TOTAL_PAGES
            self.display_page = p
            self.log_event("BUTTON S4", f"Switched display to {page_names[p]}")

    def serial_worker(self):
        last_signature = None
        last_push_time = 0.0

        while True:
            # 1. Connect to USB-C Serial port if not connected
            if not self.serial_connected and serial is not None:
                port = find_serial_port()
                if port:
                    try:
                        s = serial.Serial(port, SERIAL_BAUD, timeout=0.1)
                        with self.lock:
                            self.ser = s
                            self.serial_port = port
                            self.serial_connected = True
                        self.log_event("USB-C", f"Connected to LilyGO T5 4.7\" on {port}")
                        self.force_next_push = True
                    except Exception:
                        time.sleep(1.5)
                else:
                    time.sleep(1.5)

            # 2. Read any incoming lines from LilyGO T5 4.7"
            if self.serial_connected and self.ser:
                try:
                    while self.ser.in_waiting:
                        raw_line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                        if not raw_line:
                            continue
                        if raw_line.startswith("{") and raw_line.endswith("}"):
                            msg = json.loads(raw_line)
                            self.last_rx_time = time.time()
                            if "cmd" in msg:
                                self.handle_device_command(msg)
                            elif "event" in msg:
                                ev = msg["event"]
                                self.last_rx_event = ev
                                if "page" in msg:
                                    self.display_page = int(msg["page"]) % TOTAL_PAGES
                                if "standalone" in msg:
                                    self.standalone_mode = bool(msg["standalone"])
                                if ev == "boot":
                                    self.log_event("DISPLAY", f"LilyGO booted ({msg.get('board')})")
                                    self.force_next_push = True
                except Exception:
                    with self.lock:
                        self.serial_connected = False
                        try:
                            self.ser.close()
                        except Exception:
                            pass
                        self.ser = None

            # 3. Poll Numa & push state when changed or forced
            now = time.time()
            if self.force_next_push or (now - last_push_time >= 1.5):
                force = self.force_next_push
                self.force_next_push = False
                state = self.fetch_consolidated_state(force_render=force)
                if state:
                    sig = (
                        state["queries"].get("total"),
                        state["blocking"].get("enabled"),
                        state["blocking"].get("paused"),
                        state["cache"].get("entries"),
                        len(state["services"]),
                        len(state["overrides"]),
                    )
                    if force or sig != last_signature or (now - last_push_time >= 8.0):
                        if self.send_serial_json(state):
                            last_signature = sig
                            last_push_time = now

            time.sleep(0.08)


BRIDGE = NumaBridge()

COMPANION_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Numa × LilyGO T5 4.7" E-Paper Controller</title>
<style>
  :root {
    --bg: #fafafa;
    --surface: #ffffff;
    --ink: #171717;
    --muted: #525252;
    --subtle: #737373;
    --border: rgba(0, 0, 0, 0.08);
    --epd-paper: #f4f1ea;
    --epd-ink: #141413;
    --font-sans: -apple-system, BlinkMacSystemFont, "Geist", "DM Sans", "Inter", sans-serif;
    --font-mono: "Geist Mono", "JetBrains Mono", "SF Mono", ui-monospace, monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: var(--font-sans);
    background: var(--bg);
    color: var(--ink);
    padding: 28px 24px 60px;
    line-height: 1.45;
  }
  .container { max-width: 1200px; margin: 0 auto; }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding-bottom: 20px;
    margin-bottom: 24px;
    border-bottom: 1px solid #e5e5e5;
    flex-wrap: wrap;
    gap: 12px;
  }
  .brand { display: flex; align-items: baseline; gap: 14px; }
  .brand h1 { font-size: 26px; font-weight: 700; letter-spacing: -1.1px; }
  .brand span { font-family: var(--font-mono); font-size: 12px; color: var(--muted); }
  .pills { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
  .pill {
    font-family: var(--font-mono);
    font-size: 12px;
    font-weight: 500;
    padding: 5px 12px;
    border-radius: 9999px;
    box-shadow: 0 0 0 1px rgba(0,0,0,0.1);
    background: var(--surface);
  }
  .pill.live { background: #171717; color: #fff; }
  .grid {
    display: grid;
    grid-template-columns: 1fr 340px;
    gap: 24px;
    align-items: start;
  }
  @media (max-width: 1024px) { .grid { grid-template-columns: 1fr; } }
  .card {
    background: var(--surface);
    border-radius: 10px;
    box-shadow: 0 0 0 1px rgba(0,0,0,0.08), 0 2px 4px rgba(0,0,0,0.03);
    padding: 20px;
    margin-bottom: 20px;
  }
  .card-title {
    font-size: 13px;
    font-family: var(--font-mono);
    text-transform: uppercase;
    letter-spacing: 0.6px;
    color: var(--muted);
    margin-bottom: 14px;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .bezel {
    background: #1f1f1f;
    padding: 18px 18px 14px;
    border-radius: 14px;
    box-shadow: 0 12px 32px rgba(0,0,0,0.16);
  }
  .epaper-screen {
    background: var(--epd-paper);
    color: var(--epd-ink);
    aspect-ratio: 960 / 540;
    width: 100%;
    border-radius: 4px;
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    overflow: hidden;
    border: 2px solid #000;
    font-family: var(--font-mono);
  }
  .epd-header {
    background: #141413;
    color: #f4f1ea;
    padding: 10px 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .epd-header-left { display: flex; align-items: center; gap: 14px; }
  .epd-logo { font-size: 22px; font-weight: 800; letter-spacing: -1px; }
  .epd-meta { font-size: 11px; line-height: 1.35; color: #d4d0c8; }
  .epd-badges { display: flex; gap: 8px; }
  .epd-badge {
    padding: 5px 10px;
    font-size: 11px;
    font-weight: 700;
    border: 1.5px solid #f4f1ea;
  }
  .epd-badge.inverted { background: #f4f1ea; color: #141413; }
  .epd-body {
    flex: 1;
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .epd-kpis { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
  .epd-box {
    background: #faf8f3;
    border: 2px solid #141413;
    padding: 8px 10px;
  }
  .epd-box-label {
    font-size: 10px;
    font-weight: 700;
    border-bottom: 1px solid #888;
    padding-bottom: 3px;
    margin-bottom: 4px;
  }
  .epd-box-val { font-size: 24px; font-weight: 800; letter-spacing: -1px; margin: 2px 0; }
  .epd-box-sub { font-size: 10px; color: #444; }
  .epd-split { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; flex: 1; }
  .epd-bar-row {
    display: grid;
    grid-template-columns: 72px 1fr 44px;
    align-items: center;
    gap: 8px;
    font-size: 11px;
    margin: 5px 0;
  }
  .epd-bar-track { height: 12px; background: #dedad0; border: 1px solid #141413; }
  .epd-bar-fill { height: 100%; background: #141413; }
  .epd-footer {
    background: #e6e2d8;
    border-top: 2px solid #141413;
    padding: 8px 12px;
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 8px;
  }
  .hw-btn {
    font-family: var(--font-mono);
    font-size: 11px;
    font-weight: 700;
    padding: 7px 6px;
    border: 2px solid #141413;
    background: #faf8f3;
    color: #141413;
    cursor: pointer;
    text-align: center;
    transition: transform 0.08s ease, background 0.12s ease;
  }
  .hw-btn:hover, .hw-btn.active { background: #141413; color: #faf8f3; }
  .btn-stack { display: flex; flex-direction: column; gap: 8px; }
  button.ctrl {
    font-family: var(--font-sans);
    font-size: 13px;
    font-weight: 500;
    padding: 10px 14px;
    border-radius: 6px;
    border: none;
    box-shadow: 0 0 0 1px rgba(0,0,0,0.12);
    background: #fff;
    color: #171717;
    cursor: pointer;
    display: flex;
    justify-content: space-between;
    align-items: center;
    transition: background 0.12s;
  }
  button.ctrl:hover { background: #f3f3f3; }
  button.ctrl.primary { background: #171717; color: #fff; box-shadow: none; }
  button.ctrl.primary:hover { background: #333; }
  button.ctrl kbd {
    font-family: var(--font-mono);
    font-size: 11px;
    padding: 2px 6px;
    border-radius: 4px;
    background: rgba(128,128,128,0.15);
  }
  input.field {
    width: 100%;
    padding: 8px 10px;
    border-radius: 6px;
    border: none;
    box-shadow: 0 0 0 1px rgba(0,0,0,0.15);
    font-family: var(--font-mono);
    font-size: 12px;
    margin-bottom: 8px;
  }
  .log-list { font-family: var(--font-mono); font-size: 11px; max-height: 190px; overflow-y: auto; }
  .log-item { padding: 5px 0; border-bottom: 1px solid #f0f0f0; display: flex; gap: 8px; }
  .log-time { color: var(--subtle); }
  .log-src { font-weight: 700; }
  table.qtable { width: 100%; border-collapse: collapse; font-size: 11px; }
  table.qtable th, table.qtable td { text-align: left; padding: 4px 6px; border-bottom: 1px solid #ccc; }
  table.qtable th { background: #141413; color: #fff; }
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="brand">
      <h1>Numa × LilyGO T5 4.7"</h1>
      <span>Portable Travel AdBlocker &amp; E-Paper Dashboard</span>
    </div>
    <div class="pills">
      <span class="pill live" id="usbBadge">USB-C SERIAL: CHECKING</span>
      <span class="pill" id="modeBadge">MODE: AUTO</span>
      <a href="http://localhost:5380" target="_blank" class="pill" style="text-decoration:none;color:inherit;">Open Numa Web UI (:5380) ↗</a>
    </div>
  </header>

  <div class="grid">
    <div>
      <div class="card">
        <div class="card-title">
          <span>Live 960×540 E-Paper Framebuffer Mirror</span>
          <span id="pageIndicator">Page 1/4 · Overview</span>
        </div>
        <div class="bezel">
          <div class="epaper-screen">
            <div class="epd-header">
              <div class="epd-header-left">
                <div class="epd-logo">NUMA</div>
                <div class="epd-meta">
                  <div id="epdSub1">v0.14.3 | FORWARD -> 192.168.10.47:53</div>
                  <div id="epdSub2">DNS :5553  API 192.168.10.78:5380  UP 0s</div>
                </div>
              </div>
              <div class="epd-badges">
                <div class="epd-badge inverted" id="epdBlockBadge">BLOCKING ON (202.3K)</div>
                <div class="epd-badge" id="epdLinkBadge">USB-C MAC</div>
              </div>
            </div>

            <div class="epd-body" id="epdBody"></div>

            <div class="epd-footer">
              <button class="hw-btn" id="btnS4" onclick="triggerAction('page')">[S4] PAGE 1/4</button>
              <button class="hw-btn active" id="btnS3" onclick="triggerAction('toggle_blocking')">[S3] BLOCK: ON</button>
              <button class="hw-btn" onclick="triggerAction('flush_cache')">[S2] FLUSH CACHE</button>
              <button class="hw-btn" id="btnS5" onclick="triggerAction('refresh')">[S5] SYNC NOW</button>
            </div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">
          <span>Hardware &amp; Bridge Event Stream</span>
          <span>/dev/cu.usbserial-54250266921 @ 115200 baud</span>
        </div>
        <div class="log-list" id="eventLog"></div>
      </div>
    </div>

    <div>
      <div class="card">
        <div class="card-title"><span>Display &amp; DNS Controls</span></div>
        <div class="btn-stack">
          <button class="ctrl primary" onclick="triggerAction('toggle_blocking')">
            <span id="lblToggleBtn">Toggle Ad Blocking (ON/OFF)</span>
            <kbd>Button S3</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('pause_blocking')">
            <span>Pause Ad Blocking (5m)</span>
            <kbd>Hold S3</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('flush_cache')">
            <span>Flush Numa DNS Cache</span>
            <kbd>Button S2</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('page')">
            <span>Cycle Display Page (1/2/3/4)</span>
            <kbd>Button S4</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('toggle_mode')">
            <span>Switch Travel (:53) / USB-C Mode</span>
            <kbd>Hold S5</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('refresh')">
            <span>Force E-Paper Full Refresh</span>
            <kbd>Button S5</kbd>
          </button>
          <button class="ctrl" onclick="triggerAction('simulate_queries')">
            <span>Simulate Sample DNS Traffic</span>
            <kbd>dig :5553</kbd>
          </button>
        </div>
      </div>

      <div class="card">
        <div class="card-title"><span>Quick Register .numa Service</span></div>
        <input class="field" id="svcName" placeholder="Service name (e.g. frontend)" />
        <input class="field" id="svcPort" placeholder="Target localhost port (e.g. 3000)" type="number" />
        <button class="ctrl primary" style="width:100%;justify-content:center;" onclick="addService()">
          Register https://&lt;name&gt;.numa
        </button>
      </div>

      <div class="card">
        <div class="card-title"><span>Travel Wi-Fi Provisioning (NVS)</span></div>
        <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
          Push Wi-Fi credentials over USB-C so the LilyGO T5 connects automatically while travelling.
        </p>
        <input class="field" id="wifiSsid" placeholder="Wi-Fi SSID (2.4GHz)" />
        <input class="field" id="wifiPass" placeholder="Wi-Fi Password" type="password" />
        <input class="field" id="numaHost" placeholder="Mac LAN IP (default: auto)" />
        <button class="ctrl" style="width:100%;justify-content:center;" onclick="saveWifi()">
          Save Wi-Fi Config to ESP32 Flash
        </button>
      </div>
    </div>
  </div>
</div>

<script>
function fmtNum(n) {
  n = Number(n || 0);
  if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
  if (n >= 10000) return (n / 1000).toFixed(1) + 'K';
  return String(n);
}

function fmtUptime(s) {
  s = Number(s || 0);
  if (s < 60) return s + 's';
  if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
  return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
}

function renderMirror(st, page, standalone) {
  if (!st || !st.queries) return;
  const q = st.queries || {};
  const bl = st.blocking || {};
  const c = st.cache || {};
  const logs = st.logs || [];
  const svcs = st.services || [];
  const ovrs = st.overrides || [];

  document.getElementById('epdSub1').textContent = standalone
    ? `TRAVEL STANDALONE  |  STANDALONE -> 9.9.9.9:53`
    : `v${st.version}  |  ${(st.mode || 'forward').toUpperCase()} -> ${st.upstream}`;
  document.getElementById('epdSub2').textContent =
    `DNS ${st.lan_ip}:${standalone ? 53 : st.dns_port}   WEB :${standalone ? 80 : st.api_port}   UP ${fmtUptime(st.uptime_secs)}`;

  const blockActive = bl.enabled && !bl.paused;
  const blkBadge = document.getElementById('epdBlockBadge');
  blkBadge.textContent = bl.paused
    ? 'BLOCKING PAUSED'
    : (bl.enabled ? `BLOCKING ON (${fmtNum(bl.domains_loaded)})` : 'BLOCKING OFF');
  blkBadge.className = 'epd-badge' + (blockActive ? ' inverted' : '');

  document.getElementById('epdLinkBadge').textContent = standalone ? 'TRAVEL :53' : 'USB-C MAC';
  document.getElementById('btnS4').textContent = `[S4] PAGE ${(page % 4) + 1}/4`;

  const btnS3 = document.getElementById('btnS3');
  btnS3.textContent = `[S3] BLOCK: ${blockActive ? 'ON' : 'OFF'}`;
  btnS3.className = 'hw-btn' + (blockActive ? ' active' : '');

  const btnS5 = document.getElementById('btnS5');
  btnS5.textContent = standalone ? '[S5] TRAVEL MODE' : '[S5] USB-C MODE';
  btnS5.className = 'hw-btn' + (standalone ? ' active' : '');

  const pageNames = [
    'Page 1/4 · Overview & Telemetry',
    'Page 2/4 · Live DNS Query Stream',
    'Page 3/4 · Portable Travel AdBlocker Setup',
    'Page 4/4 · .numa Domains & Custom Rules'
  ];
  document.getElementById('pageIndicator').textContent = pageNames[page % 4];

  const body = document.getElementById('epdBody');
  if (page % 4 === 0) {
    const total = q.total || 0;
    const blkPct = total > 0 ? ((q.blocked || 0) * 100 / total).toFixed(1) : '0.0';
    const hitPct = total > 0 ? Math.round((q.cached || 0) * 100 / total) : 0;
    const maxBar = Math.max(1, q.cached || 0, q.upstream || 0, (q.local || 0) + (q.overridden || 0), q.blocked || 0);

    const recentRows = logs.slice(0, 5).map(l => `
      <div style="display:flex;justify-content:space-between;font-size:11px;padding:3px 0;border-bottom:1px dotted #ccc;">
        <span style="max-width:190px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${l.domain}</span>
        <span><b>${l.path}</b> · ${Number(l.latency_ms).toFixed(2)}ms</span>
      </div>
    `).join('') || '<div style="font-size:11px;color:#666;">No queries yet</div>';

    body.innerHTML = `
      <div class="epd-kpis">
        <div class="epd-box">
          <div class="epd-box-label">TOTAL QUERIES</div>
          <div class="epd-box-val">${fmtNum(q.total)}</div>
          <div class="epd-box-sub">DNS IP: ${st.lan_ip}</div>
        </div>
        <div class="epd-box">
          <div class="epd-box-label">ADS &amp; TRACKERS</div>
          <div class="epd-box-val">${fmtNum(q.blocked)}</div>
          <div class="epd-box-sub">${blkPct}% · ${fmtNum(bl.domains_loaded)} rules</div>
        </div>
        <div class="epd-box">
          <div class="epd-box-label">CACHE PERFORMANCE</div>
          <div class="epd-box-val">${hitPct}%</div>
          <div class="epd-box-sub">Hits: ${fmtNum(q.cached)} · Stored: ${c.entries || 0}</div>
        </div>
        <div class="epd-box">
          <div class="epd-box-label">.NUMA &amp; CUSTOM</div>
          <div class="epd-box-val">${fmtNum((q.local || 0) + (q.overridden || 0))}</div>
          <div class="epd-box-sub">${svcs.length} services · ${ovrs.length} overrides</div>
        </div>
      </div>
      <div class="epd-split">
        <div class="epd-box">
          <div class="epd-box-label">RESOLUTION PIPELINE</div>
          <div class="epd-bar-row"><span>CACHED</span><div class="epd-bar-track"><div class="epd-bar-fill" style="width:${((q.cached||0)*100/maxBar)}%"></div></div><span>${q.cached||0}</span></div>
          <div class="epd-bar-row"><span>UPSTREAM</span><div class="epd-bar-track"><div class="epd-bar-fill" style="width:${((q.upstream||0)*100/maxBar)}%"></div></div><span>${q.upstream||0}</span></div>
          <div class="epd-bar-row"><span>LOCAL</span><div class="epd-bar-track"><div class="epd-bar-fill" style="width:${(((q.local||0)+(q.overridden||0))*100/maxBar)}%"></div></div><span>${(q.local||0)+(q.overridden||0)}</span></div>
          <div class="epd-bar-row"><span>BLOCKED</span><div class="epd-bar-track"><div class="epd-bar-fill" style="width:${((q.blocked||0)*100/maxBar)}%"></div></div><span>${q.blocked||0}</span></div>
        </div>
        <div class="epd-box">
          <div class="epd-box-label">RECENT QUERIES</div>
          ${recentRows}
        </div>
      </div>
    `;
  } else if (page % 4 === 1) {
    const rows = logs.slice(0, 8).map(l => `
      <tr>
        <td><b>${l.domain}</b></td>
        <td>${l.query_type}</td>
        <td>${l.path}</td>
        <td>${l.rescode}</td>
        <td>${Number(l.latency_ms).toFixed(2)} ms</td>
      </tr>
    `).join('');
    body.innerHTML = `
      <div class="epd-box" style="flex:1;">
        <div class="epd-box-label">LIVE DNS QUERY STREAM (LATEST 8)</div>
        <table class="qtable">
          <thead><tr><th>DOMAIN</th><th>TYPE</th><th>PIPELINE</th><th>STATUS</th><th>LATENCY</th></tr></thead>
          <tbody>${rows}</tbody>
        </table>
      </div>
    `;
  } else if (page % 4 === 2) {
    body.innerHTML = `
      <div class="epd-split" style="flex:1;">
        <div class="epd-box">
          <div class="epd-box-label">PORTABLE TRAVEL ADBLOCKER</div>
          <div style="font-size:11px;margin:6px 0;">1. SET PHONE / LAPTOP DNS SERVER TO:</div>
          <div style="background:#141413;color:#faf8f3;font-size:24px;font-weight:800;text-align:center;padding:10px;margin-bottom:10px;">
            ${st.lan_ip}
          </div>
          <div style="font-size:11px;margin:4px 0;"><b>2. TRAVEL WIFI SETUP HOTSPOT:</b></div>
          <div style="font-size:11px;">SSID: <b>Numa-Travel-DNS</b> (Open)</div>
          <div style="font-size:11px;">Portal: <b>http://192.168.4.1</b> or <b>http://numa.numa</b></div>
          <div style="font-size:11px;margin-top:8px;"><b>3. HAGEZI PRO ENGINE:</b> 202,302 domains in PSRAM</div>
        </div>
        <div class="epd-box">
          <div class="epd-box-label">NODE &amp; RADIO STATUS</div>
          <div style="font-size:11px;line-height:1.8;">
            <div><b>Mode:</b> ${standalone ? 'STANDALONE TRAVEL DNS (:53)' : 'USB-C MAC NUMA BRIDGE'}</div>
            <div><b>SoftAP:</b> Numa-Travel-DNS (192.168.4.1)</div>
            <div><b>Upstream:</b> 9.9.9.9:53 / ${st.upstream}</div>
            <div><b>PSRAM Table:</b> 202,302 40-bit FNV-1a hashes (1.01 MB)</div>
          </div>
        </div>
      </div>
    `;
  } else {
    const svcHtml = [
      `<div style="display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px dotted #ccc;font-size:11px;"><b>http://numa.numa</b><span>${st.lan_ip}</span></div>`,
      `<div style="display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px dotted #ccc;font-size:11px;"><b>http://travel.numa</b><span>${st.lan_ip}</span></div>`,
      ...svcs.map(s => `<div style="display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px dotted #ccc;font-size:11px;"><b>https://${s.name}.numa</b><span>localhost:${s.target_port}</span></div>`)
    ].join('');
    const ovrHtml = ovrs.map(o => `<div style="font-size:11px;padding:3px 0;"><b>${o.domain}</b> → ${o.target} (${o.record_type})</div>`).join('')
      || '<div style="font-size:11px;color:#555;">No custom rules / overrides active.</div>';
    body.innerHTML = `
      <div class="epd-split" style="flex:1;">
        <div class="epd-box">
          <div class="epd-box-label">LOCAL .NUMA DOMAINS</div>
          ${svcHtml}
        </div>
        <div class="epd-box">
          <div class="epd-box-label">CUSTOM BLOCK / ALLOW RULES</div>
          ${ovrHtml}
        </div>
      </div>
    `;
  }
}

async function pollBridge() {
  try {
    const res = await fetch('/api/bridge-state');
    const data = await res.json();
    const currentPage = data.display_page || 0;
    const standalone = Boolean(data.standalone_mode);

    const usbBadge = document.getElementById('usbBadge');
    usbBadge.textContent = data.serial_connected
      ? `USB-C LIVE (${data.serial_port})`
      : 'STANDALONE / UNPLUGGED';

    document.getElementById('modeBadge').textContent = standalone
      ? 'MODE: TRAVEL STANDALONE (:53)'
      : 'MODE: USB-C BRIDGE';

    renderMirror(data.state, currentPage, standalone);

    const logEl = document.getElementById('eventLog');
    logEl.innerHTML = (data.events || []).map(e => `
      <div class="log-item">
        <span class="log-time">${e.time}</span>
        <span class="log-src">[${e.source}]</span>
        <span>${e.message}</span>
      </div>
    `).join('');
  } catch (e) {}
}

async function triggerAction(action) {
  await fetch('/api/action', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({action})
  });
  await pollBridge();
}

async function addService() {
  const name = document.getElementById('svcName').value.trim();
  const port = parseInt(document.getElementById('svcPort').value, 10);
  if (!name || !port) return;
  await fetch('/api/action', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({action: 'add_service', name, port})
  });
  document.getElementById('svcName').value = '';
  document.getElementById('svcPort').value = '';
  await pollBridge();
}

async function saveWifi() {
  const ssid = document.getElementById('wifiSsid').value.trim();
  const pass = document.getElementById('wifiPass').value;
  const host = document.getElementById('numaHost').value.trim();
  await fetch('/api/action', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({action: 'set_wifi', ssid, pass, host})
  });
  await pollBridge();
}

setInterval(pollBridge, 1200);
pollBridge();
</script>
</body>
</html>
"""


class BridgeHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def _send_json(self, obj: dict, status: int = 200):
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/?"):
            raw = COMPANION_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        elif self.path == "/api/bridge-state":
            state = BRIDGE.latest_state or BRIDGE.fetch_consolidated_state()
            self._send_json({
                "serial_connected": BRIDGE.serial_connected,
                "serial_port": BRIDGE.serial_port,
                "display_page": BRIDGE.display_page,
                "standalone_mode": BRIDGE.standalone_mode,
                "state": state,
                "events": BRIDGE.event_log,
            })
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/api/action":
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            action = body.get("action")

            if action == "toggle_blocking":
                BRIDGE.handle_device_command({"cmd": "toggle_blocking"})
            elif action == "pause_blocking":
                BRIDGE.handle_device_command({"cmd": "pause_blocking", "arg": 300})
            elif action == "unpause_blocking":
                BRIDGE.handle_device_command({"cmd": "unpause_blocking"})
            elif action == "flush_cache":
                BRIDGE.handle_device_command({"cmd": "flush_cache"})
            elif action == "refresh":
                BRIDGE.handle_device_command({"cmd": "refresh"})
            elif action == "toggle_mode":
                BRIDGE.handle_device_command({"cmd": "toggle_mode"})
            elif action == "page":
                next_page = (BRIDGE.display_page + 1) % TOTAL_PAGES
                BRIDGE.display_page = next_page
                BRIDGE.send_serial_json({"type": "page", "page": next_page})
                BRIDGE.log_event("WEB UI", f"Switched E-Paper page to {next_page + 1}/{TOTAL_PAGES}")
            elif action == "add_service":
                name = body.get("name", "").strip()
                port = int(body.get("port", 0))
                if name and port:
                    BRIDGE.numa_request("/services", method="POST", body={"name": name, "target_port": port})
                    BRIDGE.log_event("WEB UI", f"Registered https://{name}.numa -> localhost:{port}")
                    BRIDGE.sync_and_push_now()
            elif action == "set_wifi":
                cfg = {
                    "type": "config",
                    "wifi_ssid": body.get("ssid", ""),
                    "wifi_pass": body.get("pass", ""),
                    "numa_host": body.get("host") or BRIDGE.lan_ip,
                    "numa_port": 5380,
                }
                BRIDGE.send_serial_json(cfg)
                BRIDGE.log_event("WEB UI", f"Pushed Wi-Fi config (SSID: {cfg['wifi_ssid']}, Host: {cfg['numa_host']}) to ESP32 NVS")
            elif action == "simulate_queries":
                domains = ["github.com", "pagead2.googlesyndication.com", "epaper.numa", "cloudflare.com", "numa.numa"]
                for d in domains:
                    subprocess.Popen(
                        ["dig", "@127.0.0.1", "-p", str(NUMA_DNS_PORT), d, "+short"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                BRIDGE.log_event("SIMULATOR", f"Sent {len(domains)} DNS queries to Numa (:{NUMA_DNS_PORT})")
                time.sleep(0.35)
                BRIDGE.sync_and_push_now()

            self._send_json({"ok": True})
        else:
            self.send_error(404)


def main():
    BRIDGE.log_event("BRIDGE", f"Starting Numa <-> LilyGO T5 4.7\" Bridge (LAN IP: {BRIDGE.lan_ip})")
    t = threading.Thread(target=BRIDGE.serial_worker, daemon=True)
    t.start()

    server = ThreadingHTTPServer(("0.0.0.0", BRIDGE_HTTP_PORT), BridgeHTTPHandler)
    BRIDGE.log_event("HTTP", f"Companion Controller listening on http://localhost:{BRIDGE_HTTP_PORT} (and http://epaper.numa:8080)")
    server.serve_forever()


if __name__ == "__main__":
    main()
