#pragma once

#include <Arduino.h>

static const char TRAVEL_WEBUI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Numa Travel AdBlocker · LilyGO T5 4.7"</title>
<style>
  :root {
    --bg: #fafafa;
    --card: #ffffff;
    --ink: #171717;
    --muted: #525252;
    --border: rgba(0,0,0,0.09);
    --font-sans: -apple-system, BlinkMacSystemFont, "Geist", "Inter", sans-serif;
    --font-mono: "Geist Mono", "SF Mono", ui-monospace, monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: var(--font-sans);
    background: var(--bg);
    color: var(--ink);
    padding: 18px 16px 48px;
    max-width: 820px;
    margin: 0 auto;
  }
  header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding-bottom: 14px;
    margin-bottom: 18px;
    border-bottom: 1px solid #e5e5e5;
    flex-wrap: wrap;
    gap: 10px;
  }
  h1 { font-size: 22px; font-weight: 700; letter-spacing: -0.8px; }
  .badge {
    font-family: var(--font-mono);
    font-size: 11px;
    font-weight: 600;
    padding: 5px 10px;
    border-radius: 999px;
    background: #171717;
    color: #fff;
  }
  .kpis {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 10px;
    margin-bottom: 16px;
  }
  @media (min-width: 600px) {
    .kpis { grid-template-columns: repeat(4, 1fr); }
  }
  .card {
    background: var(--card);
    border-radius: 10px;
    box-shadow: 0 0 0 1px var(--border), 0 2px 4px rgba(0,0,0,0.02);
    padding: 14px;
    margin-bottom: 14px;
  }
  .label {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--muted);
    text-transform: uppercase;
    margin-bottom: 6px;
  }
  .val {
    font-family: var(--font-mono);
    font-size: 24px;
    font-weight: 700;
    letter-spacing: -1px;
  }
  .sub { font-size: 11px; color: var(--muted); margin-top: 4px; }
  .btn-row {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 8px;
  }
  @media (min-width: 600px) {
    .btn-row { grid-template-columns: repeat(4, 1fr); }
  }
  button {
    font-family: var(--font-sans);
    font-size: 13px;
    font-weight: 600;
    padding: 10px 12px;
    border-radius: 7px;
    border: 1px solid #d4d4d4;
    background: #fff;
    color: #171717;
    cursor: pointer;
  }
  button.primary {
    background: #171717;
    color: #fff;
    border-color: #171717;
  }
  input, select {
    width: 100%;
    padding: 9px 10px;
    border-radius: 7px;
    border: 1px solid #d4d4d4;
    font-family: var(--font-mono);
    font-size: 13px;
    margin-bottom: 8px;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--font-mono);
    font-size: 11px;
  }
  th, td {
    text-align: left;
    padding: 6px 4px;
    border-bottom: 1px solid #eee;
  }
</style>
</head>
<body>
  <header>
    <div>
      <h1>Numa Travel AdBlocker</h1>
      <div class="sub" id="hdrSub">LilyGO T5 4.7" Standalone DNS Sinkhole · 202.3K Hagezi Pro Rules</div>
    </div>
    <span class="badge" id="hdrBadge">STANDALONE DNS :53</span>
  </header>

  <div class="kpis">
    <div class="card">
      <div class="label">Total Queries</div>
      <div class="val" id="kTotal">0</div>
      <div class="sub" id="kDnsIp">DNS IP: --</div>
    </div>
    <div class="card">
      <div class="label">Blocked Ads</div>
      <div class="val" id="kBlocked">0</div>
      <div class="sub" id="kRules">202,302 rules</div>
    </div>
    <div class="card">
      <div class="label">PSRAM Cache</div>
      <div class="val" id="kCached">0</div>
      <div class="sub" id="kCacheEntries">0 stored</div>
    </div>
    <div class="card">
      <div class="label">Battery / RAM</div>
      <div class="val" id="kBatt">--V</div>
      <div class="sub" id="kPsram">PSRAM OK</div>
    </div>
  </div>

  <div class="card">
    <div class="label">Quick Hardware &amp; DNS Controls</div>
    <div class="btn-row">
      <button class="primary" id="btnToggle" onclick="act('toggle')">Toggle AdBlock</button>
      <button onclick="act('pause')">Pause 5m</button>
      <button onclick="act('flush')">Flush Cache</button>
      <button onclick="act('page')">Next EPD Page</button>
    </div>
  </div>

  <div class="card">
    <div class="label">Domain Tester &amp; Custom Rules (Saved in Flash)</div>
    <input id="domInput" placeholder="Enter domain (e.g. doubleclick.net or tiktok.com)" />
    <div class="btn-row">
      <button onclick="checkDom()">Check Status</button>
      <button class="primary" onclick="ruleDom('block')">+ Block Domain</button>
      <button onclick="ruleDom('allow')">+ Allow Domain</button>
      <button onclick="act('refresh')">Refresh E-Ink</button>
    </div>
    <div class="sub" id="domResult" style="margin-top:8px;font-weight:600;"></div>
  </div>

  <div class="card">
    <div class="label">Travel Wi-Fi &amp; Upstream DNS Setup</div>
    <p class="sub" style="margin-bottom:8px;">Connect your LilyGO T5 to hotel, cafe, or phone hotspot Wi-Fi:</p>
    <input id="ssid" placeholder="Hotel / Hotspot Wi-Fi SSID (2.4GHz)" />
    <input id="pass" type="password" placeholder="Wi-Fi Password" />
    <input id="up" placeholder="Upstream DNS (default: 9.9.9.9 or 1.1.1.1)" />
    <button class="primary" style="width:100%;" onclick="saveNet()">Save &amp; Connect Wi-Fi</button>
  </div>

  <div class="card">
    <div class="label">Recent On-Device DNS Queries</div>
    <table>
      <thead><tr><th>Domain</th><th>Type</th><th>Result</th><th>Latency</th></tr></thead>
      <tbody id="qBody"></tbody>
    </table>
  </div>

<script>
async function load() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('kTotal').textContent = s.total;
    document.getElementById('kBlocked').textContent = s.blocked;
    document.getElementById('kCached').textContent = s.cached;
    document.getElementById('kCacheEntries').textContent = s.cache_entries + ' stored in PSRAM';
    document.getElementById('kDnsIp').textContent = 'Set DNS to: ' + s.ip;
    document.getElementById('kBatt').textContent = Number(s.battery_v).toFixed(2) + 'V';
    document.getElementById('kPsram').textContent = Math.round(s.free_psram_kb) + ' KB free';
    document.getElementById('hdrBadge').textContent = s.blocking ? 'BLOCKING ON (:53)' : 'BLOCKING OFF';
    document.getElementById('qBody').innerHTML = (s.logs || []).map(l =>
      `<tr><td><b>${l.domain}</b></td><td>${l.qtype}</td><td>${l.path}</td><td>${Number(l.latency_ms).toFixed(1)}ms</td></tr>`
    ).join('');
  } catch (e) {}
}
async function act(cmd) {
  await fetch('/api/cmd?c=' + encodeURIComponent(cmd), {method:'POST'});
  setTimeout(load, 250);
}
async function checkDom() {
  const d = document.getElementById('domInput').value.trim();
  if (!d) return;
  const r = await fetch('/api/check?d=' + encodeURIComponent(d));
  const j = await r.json();
  document.getElementById('domResult').textContent =
    j.blocked ? `BLOCKED (matched rule: ${j.rule})` : `ALLOWED (${j.rule || 'not in blocklist'})`;
}
async function ruleDom(kind) {
  const d = document.getElementById('domInput').value.trim();
  if (!d) return;
  await fetch(`/api/rule?kind=${kind}&d=${encodeURIComponent(d)}`, {method:'POST'});
  await checkDom();
  await load();
}
async function saveNet() {
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  const up = document.getElementById('up').value.trim();
  await fetch(`/api/wifi?ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}&up=${encodeURIComponent(up)}`, {method:'POST'});
  alert('Saved! The LilyGO T5 is connecting to ' + ssid + ' and updating its E-Paper screen.');
}
setInterval(load, 2000);
load();
</script>
</body>
</html>
)rawliteral";
