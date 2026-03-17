#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "web_server.h"
#include "config.h"
#include "sensors.h"
#include <cmath>

extern WebServer server;

// ── ESP32 valid GPIO pins for sensor use ────────────────────────────────────
static bool isValidGPIO(int pin)
{
    // Valid for digital/output: 13, 14, 25, 26, 27, 32, 33
    // Valid for input-only: 34, 35, 36, 39
    return (pin == 13) || (pin == 14) || (pin == 15) ||
           (pin >= 25 && pin <= 27) ||
           (pin >= 32 && pin <= 36) ||
           (pin == 39);
}

static bool isADC1Pin(int pin)
{
    // ADC1 Pins: 32, 33, 34, 35, 36, 37, 38, 39
    return (pin >= 32 && pin <= 39);
}

// ── Monotonic sensor ID counter ─────────────────────────────────────────────
static uint32_t s_sensorCounter = 0;

static String nextSensorId()
{
    // Re-sync with persisted sensors to avoid collisions after reboot
    for (const auto& s : activeSensors) {
        uint32_t n = 0;
        sscanf(s.id.c_str(), "s_%05" PRIu32, &n);
        if (n > s_sensorCounter) s_sensorCounter = n;
    }
    s_sensorCounter++;
    char buf[16];
    snprintf(buf, sizeof(buf), "s_%05" PRIu32, s_sensorCounter);
    return String(buf);
}

// ── Session management ────────────────────────────────────────────────────────
static String        s_sessionToken  = "";
static unsigned long s_sessionExpiry = 0;
static const unsigned long SESSION_MS = 4UL * 3600UL * 1000UL; // 4 h sliding window

static bool isAuthorized()
{
    String token = server.header("X-Session-Token");
    if (!s_sessionToken.isEmpty() && token == s_sessionToken) {
        if (millis() < s_sessionExpiry) {
            s_sessionExpiry = millis() + SESSION_MS; // slide on each valid use
            return true;
        }
        s_sessionToken  = "";  // S4: expired — clear so login is required
        s_sessionExpiry = 0;
    }
    if (!systemConfig.apiKey.isEmpty() &&
        server.header("X-API-Key") == systemConfig.apiKey) return true;
    return false;
}

// I10: reject oversized POST bodies before parsing JSON
static bool checkPayload(size_t limit = 2048)
{
    if (server.arg("plain").length() > limit) {
        server.send(413, "application/json", "{\"error\":\"Payload too large\"}");
        return false;
    }
    return true;
}

static void sendUnauthorized()
{
    server.send(401, "application/json",
                "{\"error\":\"Unauthorized\"}");
}

// ── HTML sanitiser ───────────────────────────────────────────────────────────
static String htmlEscape(const String& s)
{
    String out;
    out.reserve(s.length() + 16);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#x27;"; break;
            default:   out += c;
        }
    }
    return out;
}

// ============================================================
// DASHBOARD HTML (PROGMEM)
// ============================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Sensor Node</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=IBM+Plex+Sans:wght@300;400;500;600&display=swap" rel="stylesheet">
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
:root {
  --bg-base:#f1f5f9; --bg-surface:#ffffff; --bg-raised:#f8fafc; --bg-hover:#f1f5f9;
  --border:#e2e8f0; --border-hi:#cbd5e1;
  --accent:#2563eb; --accent-dim:rgba(37,99,235,.08); --accent-glow:rgba(37,99,235,.2);
  --green:#16a34a; --green-dim:rgba(22,163,74,.1);
  --amber:#d97706; --amber-dim:rgba(215,119,6,.1);
  --red:#dc2626; --red-dim:rgba(220,38,38,.1);
  --text-pri:#0f172a; --text-sec:#64748b; --text-dim:#94a3b8;
  --mono:'IBM Plex Mono',monospace; --sans:'IBM Plex Sans',sans-serif;
}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--sans);background:var(--bg-base);color:var(--text-pri);display:flex;min-height:100vh;font-size:14px}
::-webkit-scrollbar{width:5px;height:5px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--border-hi);border-radius:99px}
.sidebar{width:220px;min-height:100vh;background:var(--bg-surface);border-right:1px solid var(--border);display:flex;flex-direction:column;position:fixed;top:0;left:0;bottom:0;z-index:100}
.sidebar-brand{padding:20px 18px 16px;border-bottom:1px solid var(--border)}
.sidebar-brand .label{font-family:var(--mono);font-size:10px;letter-spacing:.12em;text-transform:uppercase;color:var(--text-sec);margin-bottom:4px}
.sidebar-brand .name{font-size:15px;font-weight:600;color:var(--text-pri)}
.status-dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--green);margin-right:6px;box-shadow:0 0 6px var(--green);animation:pulse 2.4s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
.nav{padding:10px 8px;flex:1}
.nav-item{display:flex;align-items:center;gap:10px;padding:9px 12px;border-radius:7px;color:var(--text-sec);cursor:pointer;font-size:13.5px;font-weight:500;transition:background .15s,color .15s;user-select:none;margin-bottom:2px}
.nav-item svg{flex-shrink:0}
.nav-item:hover{background:var(--bg-hover);color:var(--text-pri)}
.nav-item.active{background:var(--accent-dim);color:var(--accent)}
.nav-item.active svg{stroke:var(--accent)}
.sidebar-footer{padding:14px 18px;border-top:1px solid var(--border);font-family:var(--mono);font-size:10px;color:var(--text-dim);line-height:1.6}
.main{margin-left:220px;flex:1;display:flex;flex-direction:column;min-height:100vh}
.topbar{height:52px;border-bottom:1px solid var(--border);display:flex;align-items:center;padding:0 24px;gap:12px;background:var(--bg-surface);position:sticky;top:0;z-index:50}
.topbar-title{font-size:14px;font-weight:600;color:var(--text-pri)}
.topbar-sub{font-family:var(--mono);font-size:11px;color:var(--text-sec);margin-left:auto}
.refresh-indicator{width:28px;height:28px;border:1.5px solid var(--border-hi);border-radius:7px;display:flex;align-items:center;justify-content:center;cursor:pointer;color:var(--text-sec);transition:border-color .15s,color .15s}
.refresh-indicator:hover{border-color:var(--accent);color:var(--accent)}
.refresh-indicator.spinning svg{animation:spin .6s linear}
@keyframes spin{to{transform:rotate(360deg)}}
#alert-banner{display:none;align-items:center;gap:10px;padding:10px 24px;background:#fef2f2;border-bottom:1px solid #fecaca;font-size:13px;font-weight:500;color:var(--red);position:sticky;top:52px;z-index:49;flex-wrap:wrap}
#alert-banner.show{display:flex;animation:fadeUp .3s ease}
.alert-chip{font-family:var(--mono);font-size:10px;padding:2px 8px;border-radius:99px;background:white;border:1px solid #fca5a5;color:var(--red);margin-left:4px}
.content{padding:24px;flex:1}
.section-header{display:flex;align-items:center;gap:10px;margin-bottom:20px}
.section-header h2{font-size:16px;font-weight:600}
.badge{font-family:var(--mono);font-size:10px;padding:2px 8px;border-radius:99px;background:var(--accent-dim);color:var(--accent);border:1px solid rgba(37,99,235,.2)}
.tabs{display:flex;gap:4px;margin-bottom:20px;border-bottom:1px solid var(--border)}
.tab-btn{padding:8px 14px;background:transparent;border:none;border-bottom:2px solid transparent;color:var(--text-sec);font-family:var(--sans);font-size:13px;font-weight:500;cursor:pointer;transition:color .15s,border-color .15s;margin-bottom:-1px;display:flex;align-items:center;gap:6px;width:auto}
.tab-btn .count{font-family:var(--mono);font-size:10px;background:var(--bg-raised);border:1px solid var(--border);border-radius:99px;padding:0 6px;line-height:16px}
.tab-btn:hover{color:var(--text-pri);background:transparent;border-bottom-color:var(--border-hi)}
.tab-btn.active{color:var(--accent);border-bottom-color:var(--accent);background:transparent}
.tab-content{display:none}
.tab-content.active{display:block;animation:fadeUp .2s ease}
@keyframes fadeUp{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:translateY(0)}}
.sensor-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:14px}
.sensor-card{background:var(--bg-surface);border:1px solid var(--border);border-radius:10px;padding:16px;transition:border-color .2s,box-shadow .2s}
.sensor-card:hover{border-color:var(--border-hi);box-shadow:0 2px 12px rgba(0,0,0,.05)}
.sensor-card.alert-card{border-color:#fca5a5!important;background:#fffbfb}
.sensor-card.warn-card{border-color:#fcd34d!important;background:#fffdf5}
.card-top{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:10px}
.card-name{font-size:11px;font-weight:600;color:var(--text-sec);text-transform:uppercase;letter-spacing:.07em}
.type-pill{font-family:var(--mono);font-size:9px;padding:2px 7px;border-radius:4px;border:1px solid var(--border);color:var(--text-dim)}
.card-value-row{display:flex;align-items:baseline;gap:5px;margin-bottom:8px}
.card-value{font-family:var(--mono);font-size:30px;font-weight:600;line-height:1;color:var(--text-pri);transition:color .3s}
.card-unit{font-family:var(--mono);font-size:12px;color:var(--text-sec)}
.threshold-bar-wrap{margin-bottom:8px}
.threshold-bar-label{font-family:var(--mono);font-size:9px;color:var(--text-dim);margin-bottom:3px;display:flex;justify-content:space-between}
.threshold-bar{height:4px;border-radius:99px;background:var(--border);position:relative}
.bar-fill{height:100%;border-radius:99px;background:var(--green);transition:width .4s,background .3s}
.bar-fill.warn{background:var(--amber)}
.bar-fill.alert{background:var(--red)}
.air-status-badge{display:inline-flex;align-items:center;gap:5px;font-family:var(--mono);font-size:10px;font-weight:500;padding:3px 9px;border-radius:99px;margin-bottom:10px;border:1px solid transparent}
.air-status-badge .dot{width:6px;height:6px;border-radius:50%;flex-shrink:0}
.air-status-badge.clean{background:var(--green-dim);color:var(--green);border-color:rgba(22,163,74,.2)}
.air-status-badge.clean .dot{background:var(--green);box-shadow:0 0 4px var(--green);animation:pulse 2s infinite}
.air-status-badge.warn{background:var(--amber-dim);color:var(--amber);border-color:rgba(215,119,6,.25)}
.air-status-badge.warn .dot{background:var(--amber)}
.air-status-badge.danger{background:var(--red-dim);color:var(--red);border-color:rgba(220,38,38,.25)}
.air-status-badge.danger .dot{background:var(--red);box-shadow:0 0 5px var(--red);animation:pulse 1s infinite}
/* Vibration card — SVG-based indicator, no emoji */
.vib-indicator{display:flex;flex-direction:column;align-items:center;justify-content:center;height:140px;gap:14px}
.vib-ring{width:76px;height:76px;border-radius:50%;border:3px solid var(--border);display:flex;align-items:center;justify-content:center;transition:border-color .2s,background .2s,box-shadow .2s;background:var(--bg-raised)}
.vib-ring svg{stroke:var(--text-dim);transition:stroke .2s}
.vib-ring.active{border-color:var(--red);background:var(--red-dim);box-shadow:0 0 24px rgba(220,38,38,.25);animation:vibShake .12s infinite}
.vib-ring.active svg{stroke:var(--red)}
@keyframes vibShake{0%,100%{transform:translate(0,0) rotate(0)}25%{transform:translate(-1px,1px) rotate(-.5deg)}75%{transform:translate(1px,-1px) rotate(.5deg)}}
.vib-label{font-family:var(--mono);font-size:11px;font-weight:600;color:var(--text-sec);letter-spacing:.1em}
.vib-label.active{color:var(--red)}
.chart-container{position:relative;height:130px;width:100%}
.empty-state{display:flex;flex-direction:column;align-items:center;justify-content:center;padding:60px 20px;gap:12px;color:var(--text-sec);border:1px dashed var(--border);border-radius:10px;text-align:center}
.empty-state svg{opacity:.3}
.empty-state p{font-size:13px}
.card{background:var(--bg-surface);border:1px solid var(--border);border-radius:10px;padding:20px;margin-bottom:16px}
.form-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:16px}
.form-field{display:flex;flex-direction:column;gap:6px}
.form-field label{font-size:11px;font-weight:500;text-transform:uppercase;letter-spacing:.08em;color:var(--text-sec)}
input,select{background:var(--bg-base);border:1px solid var(--border);border-radius:7px;color:var(--text-pri);font-family:var(--sans);font-size:13px;padding:9px 12px;width:100%;margin:0;transition:border-color .15s,box-shadow .15s;outline:none}
input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-glow)}
select option{background:var(--bg-surface)}
input[type="checkbox"]{width:15px;height:15px;margin:0;accent-color:var(--accent);cursor:pointer}
input[type="number"]{font-family:var(--mono)}
input[type="password"]{font-family:var(--mono);letter-spacing:.1em}
.toggle-row{display:flex;align-items:center;gap:10px;padding:12px 14px;background:var(--bg-raised);border:1px solid var(--border);border-radius:7px;cursor:pointer}
.toggle-row label{font-size:13px;color:var(--text-pri);cursor:pointer;flex:1}
.toggle-hint{font-size:12px;color:var(--text-sec);margin-top:8px;line-height:1.5}
button{background:var(--accent);color:white;border:none;border-radius:7px;font-family:var(--sans);font-size:13px;font-weight:500;padding:9px 18px;cursor:pointer;transition:background .15s,transform .1s,box-shadow .15s;width:auto;margin:0}
button:hover{background:#1d4ed8;box-shadow:0 0 12px var(--accent-glow)}
button:active{transform:scale(.97)}
button.secondary{background:var(--bg-raised);color:var(--text-pri);border:1px solid var(--border)}
button.secondary:hover{background:var(--bg-hover);box-shadow:none}
button.danger{background:transparent;color:var(--red);border:1px solid rgba(220,38,38,.3)}
button.danger:hover{background:var(--red-dim);box-shadow:none}
button.danger-solid{background:var(--red);color:white;border:none}
button.danger-solid:hover{background:#b91c1c}
button.loading{opacity:.7;cursor:not-allowed;pointer-events:none}
button.loading::after{content:'';display:inline-block;width:11px;height:11px;border:2px solid rgba(255,255,255,.4);border-top-color:white;border-radius:50%;animation:spin .6s linear infinite;margin-left:8px;vertical-align:middle}
.table-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse}
thead tr{border-bottom:1px solid var(--border)}
th{font-family:var(--mono);font-size:10px;text-transform:uppercase;letter-spacing:.1em;color:var(--text-sec);padding:8px 14px;text-align:left;font-weight:500;white-space:nowrap}
td{padding:11px 14px;border-bottom:1px solid var(--border);font-size:13px;vertical-align:middle}
tbody tr:last-child td{border-bottom:none}
tbody tr:hover td{background:var(--bg-raised)}
.type-tag{font-family:var(--mono);font-size:10px;padding:3px 8px;border-radius:4px;background:var(--bg-raised);border:1px solid var(--border);color:var(--text-sec)}
.threshold-form{display:grid;grid-template-columns:1fr 1fr 1fr auto;gap:12px;align-items:end}
.threshold-form .form-field label{font-size:10px}
.threshold-state-badge{display:inline-flex;align-items:center;gap:5px;font-family:var(--mono);font-size:10px;padding:2px 8px;border-radius:99px;border:1px solid transparent}
.threshold-state-badge.ok{background:var(--green-dim);color:var(--green);border-color:rgba(22,163,74,.2)}
.threshold-state-badge.high{background:var(--red-dim);color:var(--red);border-color:rgba(220,38,38,.2)}
.threshold-state-badge.low{background:#eff6ff;color:#1e40af;border-color:#bfdbfe}
.threshold-state-badge.active{background:var(--red-dim);color:var(--red);border-color:rgba(220,38,38,.2)}
.threshold-state-badge.gas{background:var(--amber-dim);color:var(--amber);border-color:rgba(215,119,6,.2)}
.threshold-state-badge.off{background:var(--bg-raised);color:var(--text-dim);border-color:var(--border)}
.api-key-box{display:flex;align-items:center;gap:8px;background:var(--bg-base);border:1px solid var(--border);border-radius:7px;padding:9px 12px;font-family:var(--mono);font-size:13px}
.api-key-box span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.divider{border:none;border-top:1px solid var(--border);margin:24px 0}
.subsection-title{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.1em;color:var(--text-sec);margin-bottom:14px}
#toast{position:fixed;bottom:24px;right:24px;background:var(--bg-surface);border:1px solid var(--border);border-radius:8px;padding:12px 18px;font-size:13px;display:flex;align-items:center;gap:10px;transform:translateY(80px);opacity:0;transition:all .3s cubic-bezier(.34,1.56,.64,1);z-index:999;min-width:220px;box-shadow:0 4px 20px rgba(0,0,0,.1)}
#toast.show{transform:translateY(0);opacity:1}
#toast.success{border-left:3px solid var(--green)}
#toast.error{border-left:3px solid var(--red)}
/* Hint text shown beneath input fields */
.field-hint{font-family:var(--mono);font-size:10px;color:var(--text-dim);margin-top:3px}
/* Relay card */
.relay-badge{display:inline-flex;align-items:center;gap:6px;font-family:var(--mono);font-size:13px;font-weight:600;padding:5px 16px;border-radius:99px;border:1px solid transparent}
.relay-badge.on{background:var(--green-dim);color:var(--green);border-color:rgba(22,163,74,.25)}
.relay-badge.off{background:var(--bg-raised);color:var(--text-dim);border-color:var(--border)}
/* Pin map */
.pin-map-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(58px,1fr));gap:5px;margin-top:10px}
.pin-cell{font-family:var(--mono);font-size:9px;padding:5px 3px;border-radius:5px;text-align:center;border:1px solid transparent;line-height:1.4}
.pin-cell.sys-res{background:#fee2e2;color:#b91c1c;border-color:#fca5a5}
.pin-cell.vib-only{background:#f3e8ff;color:#7c3aed;border-color:#d8b4fe}
.pin-cell.in-use{background:var(--green-dim);color:var(--green);border-color:rgba(22,163,74,.2)}
.pin-cell.available{background:#eff6ff;color:#1d4ed8;border-color:#bfdbfe}
.pin-cell.unavail{opacity:0.2;background:var(--bg-raised);border-color:var(--border)}
</style>
</head>
<body>

<nav class="sidebar">
  <div class="sidebar-brand">
    <div class="label">Device Node</div>
    <div class="name"><span class="status-dot"></span><span id="brand-hostname">Loading...</span></div>
  </div>
  <div class="nav">
    <div class="nav-item active" onclick="nav('dashboard')" id="nav-dashboard">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="#64748b" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="9"/><rect x="14" y="3" width="7" height="5"/><rect x="14" y="12" width="7" height="9"/><rect x="3" y="16" width="7" height="5"/></svg>
      Dashboard
    </div>
    <div class="nav-item" onclick="nav('sensors')" id="nav-sensors">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="#64748b" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M12 2v2M12 20v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M2 12h2M20 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
      Sensors
    </div>
    <div class="nav-item" onclick="nav('alerts')" id="nav-alerts">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="#64748b" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/></svg>
      Alert Thresholds
    </div>
    <div class="nav-item" onclick="nav('management')" id="nav-management">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="#64748b" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.07 4.93A10 10 0 0 0 3 12c0 5.52 4.48 10 10 10a10 10 0 0 0 6.93-2.8"/><path d="M22 12c0-1.65-.4-3.2-1.1-4.57"/></svg>
      Management
    </div>
    <div class="divider" style="margin:8px 12px"></div>
    <div class="nav-item" onclick="switchToAPMode()" style="color:var(--amber)">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
      Switch to AP Mode
    </div>
  </div>
  <div class="sidebar-footer" id="footer-loc">-</div>
</nav>

<div class="main">
  <header class="topbar">
    <span class="topbar-title" id="topbar-title">Live Dashboard</span>
    <span class="topbar-sub">AUTO-REFRESH 2s</span>
    
    <div style="margin-left:auto; display:flex; gap:8px; align-items:center">
      <button id="auth-btn-in" class="secondary" style="padding:5px 12px; font-size:11px" onclick="showLoginOverlay()">Sign In</button>
      <button id="auth-btn-out" class="secondary" style="padding:5px 12px; font-size:11px; display:none" onclick="doLogout()">Sign Out</button>
      
      <div class="refresh-indicator" id="refreshBtn" onclick="manualRefresh()">
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
      </div>
    </div>
  </header>

  <div id="alert-banner">
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
    <strong>Active Alerts</strong>
    <span id="alert-items"></span>
    <button class="secondary" style="margin-left:auto;padding:4px 10px;font-size:11px" onclick="nav('alerts')">Configure</button>
  </div>

  <div class="content">

    <!-- DASHBOARD -->
    <div id="view-dashboard">
      <div class="section-header">
        <h2>Sensor Telemetry</h2>
        <span class="badge" id="sensor-count-badge">0 SENSORS</span>
      </div>
      <div class="tabs">
        <button class="tab-btn active" onclick="switchTab('dash','tab-air',this)">Air Quality <span class="count" id="cnt-air">0</span></button>
        <button class="tab-btn" onclick="switchTab('dash','tab-temp',this)">Temperature <span class="count" id="cnt-temp">0</span></button>
        <button class="tab-btn" onclick="switchTab('dash','tab-vib',this)">Vibration <span class="count" id="cnt-vib">0</span></button>
        <button class="tab-btn" onclick="switchTab('dash','tab-cur',this)">Current <span class="count" id="cnt-cur">0</span></button>
        <button class="tab-btn" onclick="switchTab('dash','tab-dust',this)">Dust <span class="count" id="cnt-dust">0</span></button>
        <button class="tab-btn" onclick="switchTab('dash','tab-relay',this)">Relay <span class="count" id="cnt-relay">0</span></button>
      </div>
      <div id="tab-air"   class="tab-content dash active"><div class="sensor-grid" id="grid-air"></div></div>
      <div id="tab-temp"  class="tab-content dash"><div class="sensor-grid" id="grid-temp"></div></div>
      <div id="tab-vib"   class="tab-content dash"><div class="sensor-grid" id="grid-vib"></div></div>
      <div id="tab-cur"   class="tab-content dash"><div class="sensor-grid" id="grid-cur"></div></div>
      <div id="tab-dust"  class="tab-content dash"><div class="sensor-grid" id="grid-dust"></div></div>
      <div id="tab-relay" class="tab-content dash"><div class="sensor-grid" id="grid-relay"></div></div>
    </div>

    <!-- SENSORS -->
    <div id="view-sensors" style="display:none">
      <div class="section-header"><h2>Sensor Configuration</h2></div>
      <div class="card" style="max-width:620px;margin-bottom:24px">
        <p class="subsection-title">Add New Sensor</p>
        <div class="form-grid">
          <div class="form-field">
            <label>Sensor Name</label>
            <input type="text" id="s_name" placeholder="e.g. Boiler Room Temp" maxlength="48">
          </div>
          <div class="form-field">
            <label>Sensor Type</label>
            <select id="s_type" onchange="onTypeChange()">
              <option value="temp">Temperature (DS18B20)</option>
              <option value="mq2">Combustible Gas (MQ-2)</option>
              <option value="mq135">Air Quality (MQ-135)</option>
              <option value="particle">Dust / Particle (GP2Y1010)</option>
              <option value="vib">Vibration (801S)</option>
              <option value="current">Current (ACS712 20A)</option>
              <option value="relay">Relay Output (CW-020)</option>
            </select>
          </div>
          <div class="form-field">
            <label>Data Pin (GPIO)</label>
            <input type="number" id="s_pin1" placeholder="e.g. 34" min="13" max="39">
            <span class="field-hint" id="pin1-hint">Analog: GPIO 32-35 &nbsp;|&nbsp; Digital: GPIO 13-14, 25-27</span>
          </div>
          <!-- pin2 row: shown only for Dust/Particle (GP2Y1010 LED pin) -->
          <div class="form-field" id="pin2-field" style="display:none">
            <label>LED Pin (GPIO)</label>
            <input type="number" id="s_pin2" placeholder="e.g. 14" min="0" max="39">
            <span class="field-hint">GP2Y1010 LED drive pin — needs 150 Ohm + 220uF</span>
          </div>
          
          <div class="form-field">
            <label>Read Interval (ms)</label>
            <input type="number" id="s_interval" value="2000" min="100" max="5000">
            <span class="field-hint">Min 100ms, Max 5000ms</span>
          </div>
        </div>
        <div style="margin-top:18px"><button id="addSensorBtn" onclick="addSensor()">Add Sensor</button></div>
      </div>
      <div class="card" style="margin-bottom:16px">
        <p class="subsection-title">Pin Utilization Map</p>
        <div style="font-size:11px;color:var(--text-sec);margin-bottom:10px;line-height:1.7">
          Available sensor pins: <strong>GPIO 13-14, 25-27, 32-35, 39</strong>.
          GPIO39 (SN) is strictly reserved for the 801S vibration sensor.<br>
          System-reserved (LCD + buttons):
          <span style="font-family:var(--mono)">G4, G5, G16-G19, G21-G23</span>
        </div>
        <div style="display:flex;flex-wrap:wrap;gap:10px;margin-bottom:10px;font-size:11px">
          <span><span class="pin-cell sys-res" style="display:inline-block;padding:2px 6px">RES</span> System reserved</span>
          <span><span class="pin-cell vib-only" style="display:inline-block;padding:2px 6px">VIB</span> Vibration only (GPIO39)</span>
          <span><span class="pin-cell in-use" style="display:inline-block;padding:2px 6px">USE</span> In use by sensor</span>
          <span><span class="pin-cell available" style="display:inline-block;padding:2px 6px">AVL</span> Available</span>
        </div>
        <div class="pin-map-grid" id="pin-map-grid"></div>
      </div>

      <div class="card">
        <p class="subsection-title">Configured Sensors</p>
        <div class="table-wrap">
          <table>
            <thead><tr><th>Name</th><th>Type</th><th>Pin(s)</th><th>Interval</th><th>Last Value</th><th>Air Status</th><th>Action</th></tr></thead>
            <tbody id="sensorTbody"></tbody>
          </table>
        </div>
        <div id="sensor-empty" class="empty-state" style="display:none;margin-top:12px">
          <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
          <p>No sensors configured yet.<br>Add your first sensor above.</p>
        </div>
      </div>
    </div>

    <!-- ALERT THRESHOLDS -->
    <div id="view-alerts" style="display:none">
      <div class="section-header">
        <h2>Alert Thresholds</h2>
        <span class="badge" id="alerts-active-badge">0 ACTIVE</span>
      </div>
      <div class="card" style="margin-bottom:16px;background:#fffbeb;border-color:#fde68a">
        <p style="font-size:13px;color:#92400e;line-height:1.7">
          <strong>How thresholds work:</strong> Set a <strong>Low</strong> and/or <strong>High</strong> limit,
          then enable alerts for that sensor. The firmware evaluates the threshold after every read and writes an
          alert state (<code>ok</code> / <code>high</code> / <code>low</code>) which the dashboard reads from
          <code>/api/status</code>. MQ gas sensors additionally fire a <code>gas</code> alert automatically
          from the air-quality classifier. Vibration sensors always alert when active — no threshold needed.
          <strong>Relay (CW-020)</strong> sensors can be configured to auto-off when a linked
          ACS712 20A exceeds its HIGH threshold.
          All thresholds and relay config persist to flash storage immediately on save.
        </p>
      </div>
      <div id="threshold-list"></div>
      <div id="threshold-empty" class="empty-state" style="display:none">
        <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/></svg>
        <p>No sensors configured yet.<br>Add sensors first, then set thresholds here.</p>
      </div>
    </div>

    <!-- MANAGEMENT -->
    <div id="view-management" style="display:none">
      <div class="section-header"><h2>Device Management</h2></div>
      <div class="tabs">
        <button class="tab-btn active" onclick="switchTab('mgmt','tab-meta',this)">Device Info</button>
        <button class="tab-btn" onclick="switchTab('mgmt','tab-network',this)">Network</button>
        <button class="tab-btn" onclick="switchTab('mgmt','tab-charts',this)">Display</button>
        <button class="tab-btn" onclick="switchTab('mgmt','tab-system',this)">System</button>
      </div>
      <div id="tab-meta" class="tab-content mgmt active">
        <div class="card" style="max-width:560px">
          <p class="subsection-title">Device Metadata</p>
          <div class="form-grid">
            <div class="form-field"><label>Hostname</label><input type="text" id="m_host"></div>
            <div class="form-field"><label>Factory</label><input type="text" id="m_factory"></div>
            <div class="form-field"><label>Building</label><input type="text" id="m_building"></div>
            <div class="form-field"><label>Room</label><input type="text" id="m_room"></div>
          </div>
          <div style="margin-top:18px"><button onclick="saveSettings()">Save Metadata</button></div>
        </div>
      </div>
      <div id="tab-network" class="tab-content mgmt">
        <div class="card" style="max-width:560px">
          <p class="subsection-title">Network Mode</p>
          <div class="toggle-row"><input type="checkbox" id="m_standalone" onchange="toggleNetwork()"><label for="m_standalone">Enable Standalone Mode (Access Point)</label></div>
          <p class="toggle-hint">When enabled, the device broadcasts its own Wi-Fi hotspot. Disable to connect to an existing network.</p>
          <div id="cluster_div" style="display:none;margin-top:18px">
            <div class="form-grid">
              <div class="form-field"><label>Network SSID</label><input type="text" id="m_ssid"></div>
              <div class="form-field"><label>Password</label><input type="password" id="m_pass" placeholder="Leave blank to keep current"></div>
            </div>
          </div>
          <div style="margin-top:18px"><button onclick="saveSettings()">Save Network Config</button></div>
        </div>
      </div>
      <div id="tab-charts" class="tab-content mgmt">
        <div class="card" style="max-width:360px">
          <p class="subsection-title">Chart Display</p>
          <div class="form-field"><label>Max Data Points</label><input type="number" id="m_maxpoints" min="10" max="200"></div>
          <div style="margin-top:18px"><button onclick="saveSettings()">Save</button></div>
        </div>
      </div>
      <div id="tab-system" class="tab-content mgmt">
        <div class="card" style="max-width:480px">
          <p class="subsection-title">Web Login</p>
          <p style="font-size:12px;color:var(--text-sec);margin-bottom:12px;line-height:1.6">
            Change the username and/or password required to access Sensors, Alerts, and Management pages.
            Default is <code>admin</code> / <code>admin</code>.
          </p>
          <div class="form-grid">
            <div class="form-field"><label>New Username</label><input type="text" id="m_webuser" placeholder="Leave blank to keep current" autocomplete="off"></div>
            <div class="form-field"><label>New Password</label><input type="password" id="m_webpass" placeholder="Leave blank to keep current" autocomplete="new-password"></div>
          </div>
          <div style="margin-top:18px"><button onclick="saveWebLogin()">Update Web Login</button></div>
          <hr class="divider">
          <p class="subsection-title">API Security</p>
          <p style="font-size:12px;color:var(--text-sec);margin-bottom:12px;line-height:1.6">
            All state-changing endpoints require this key in the
            <code>X-API-Key</code> request header. It is unique to this device
            and stored in flash. Rotate it by saving a new value below.
          </p>
          <div class="form-field" style="margin-bottom:12px">
            <label>Current API Key</label>
            <div class="api-key-box">
              <span id="current-api-key">-</span>
              <button class="secondary" style="padding:4px 10px;font-size:11px" onclick="copyApiKey()">Copy</button>
            </div>
          </div>
          <div class="form-field">
            <label>New API Key <span style="font-weight:400;text-transform:none;letter-spacing:0;color:var(--text-dim)">(min 8 characters)</span></label>
            <input type="text" id="m_apikey" placeholder="Leave blank to keep current">
          </div>
          <hr class="divider">
          <p class="subsection-title">Hotspot Security</p>
          <div class="form-field">
            <label>New Hotspot Password <span style="font-weight:400;text-transform:none;letter-spacing:0;color:var(--text-dim)">(min 8 characters)</span></label>
            <input type="text" id="m_hotspotpass" placeholder="Leave blank to keep current">
          </div>
          <div style="margin-top:18px"><button onclick="saveSecuritySettings()">Update Security Settings</button></div>
          <hr class="divider">
          <p class="subsection-title">Factory Reset</p>
          <p style="font-size:12px;color:var(--text-sec);margin-bottom:16px;line-height:1.6">
            Erases <strong>all sensors and configuration</strong> from flash storage and reboots
            with chip defaults. This cannot be undone.
          </p>
          <button class="danger-solid" onclick="factoryReset()">Factory Reset</button>
          <hr class="divider">
          <p class="subsection-title">System Operations</p>
          <p style="font-size:12px;color:var(--text-sec);margin-bottom:16px;line-height:1.6">Rebooting will briefly disconnect all clients. The device will be available again in about 10 seconds.</p>
          <button class="danger-solid" onclick="rebootDevice()">Reboot Device</button>
        </div>
      </div>
    </div>

  </div>
</div>
<!-- LOGIN OVERLAY -->
<div id="login-overlay" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:9000;align-items:center;justify-content:center">
  <div style="background:var(--bg-surface);border:1px solid var(--border);border-radius:12px;padding:32px;width:320px;box-shadow:0 8px 40px rgba(0,0,0,.18)">
    <div style="font-size:16px;font-weight:600;margin-bottom:6px">Sign In</div>
    <div style="font-size:12px;color:var(--text-sec);margin-bottom:20px">Required to access this page.</div>
    <div class="form-field" style="margin-bottom:14px"><label>Username</label><input type="text" id="li_user" autocomplete="username"></div>
    <div class="form-field" style="margin-bottom:8px"><label>Password</label><input type="password" id="li_pass" autocomplete="current-password" onkeydown="if(event.key==='Enter')doLogin()"></div>
    <div id="li_err" style="color:var(--red);font-size:12px;margin-bottom:14px;display:none">Invalid username or password.</div>
    <button style="width:100%;margin-top:6px" onclick="doLogin()">Sign In</button>
    <button class="secondary" style="width:100%;margin-top:8px" onclick="hideLoginOverlay()">Cancel</button>
  </div>
</div>
<div id="toast"></div>

<script>
// ── XSS protection ──────────────────────────────────────────────────────────
function escH(s) {
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;').replace(/'/g,'&#x27;');
}

const SENSOR_META = {
  temp:    {unit:'C',    color:'#f97316',label:'DS18B20',  chartLabel:'Temp (C)',       decimals:1, hasThreshold:true, rangeMin:-10,rangeMax:100},
  mq2:     {unit:'ratio',color:'#8b5cf6',label:'MQ-2',     chartLabel:'Rs/R0 Ratio',    decimals:3, hasThreshold:true, rangeMin:0,  rangeMax:10},
  mq135:   {unit:'ratio',color:'#0ea5e9',label:'MQ-135',   chartLabel:'Rs/R0 Ratio',    decimals:3, hasThreshold:true, rangeMin:0,  rangeMax:10},
  particle:{unit:'ug/m3',color:'#64748b',label:'GP2Y1010', chartLabel:'Dust (ug/m3)',   decimals:1, hasThreshold:true, rangeMin:0,  rangeMax:500},
  vib:     {unit:'',     color:'#ec4899',label:'801S',      chartLabel:'Vibration',      decimals:0, hasThreshold:false,rangeMin:0,  rangeMax:1},
  current: {unit:'A',    color:'#2563eb',label:'ACS712',   chartLabel:'Current (A RMS)',decimals:3, hasThreshold:true, rangeMin:0,  rangeMax:30},
  relay:   {unit:'',     color:'#16a34a',label:'CW-020',   chartLabel:'Relay State',    decimals:0, hasThreshold:false,rangeMin:0,  rangeMax:1},
};
const AIR_CLASS = {
  'Clean Air':'clean','Combustible Gas Detected':'danger','Smoke / VOC Presence':'danger',
  'Air Pollution Detected':'warn','Calibrating':'warn','Unknown':'warn'
};
const ALERT_LABELS = {ok:'OK',high:'HIGH',low:'LOW',active:'VIBRATING',gas:'GAS ALERT'};
const TITLES = {dashboard:'Live Dashboard',sensors:'Sensor Config',alerts:'Alert Thresholds',management:'Device Management'};

let charts={}, globalConfig={}, sensorMeta={}, sensorList=[];
let relayStates={};   // id → bool  — client-side relay state (source of truth)
let relayPending={};  // id → bool  — true while a toggle POST is in-flight
let apiKey = '';
let sessionToken = sessionStorage.getItem('sn_token') || '';

function authHeaders() {
  const h = {'Content-Type':'application/json'};
  if(sessionToken) h['X-Session-Token'] = sessionToken;
  else if(apiKey)  h['X-API-Key'] = apiKey;
  return h;
}

function updateAuthUI() {
  const isIn = !!sessionToken;
  document.getElementById('auth-btn-in').style.display = isIn ? 'none' : 'block';
  document.getElementById('auth-btn-out').style.display = isIn ? 'block' : 'none';
}

function showLoginOverlay(pendingPage) {
  _pendingNav = pendingPage || null;
  const ol = document.getElementById('login-overlay');
  ol.style.display = 'flex';
  document.getElementById('li_user').value = '';
  document.getElementById('li_pass').value = '';
  document.getElementById('li_err').style.display = 'none';
  setTimeout(()=>document.getElementById('li_user').focus(), 50);
}

function hideLoginOverlay() {
  document.getElementById('login-overlay').style.display = 'none';
}

async function doLogin() {
  const user = document.getElementById('li_user').value;
  const pass = document.getElementById('li_pass').value;
  try {
    const res = await fetch('/api/login', {method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({username:user, password:pass})});
    if(res.ok) {
      const data = await res.json();
      sessionToken = data.token;
      sessionStorage.setItem('sn_token', sessionToken);
      updateAuthUI();
      hideLoginOverlay();
      if(_pendingNav) { const p=_pendingNav; _pendingNav=null; nav(p); }
    } else {
      document.getElementById('li_err').style.display = 'block';
    }
  } catch { document.getElementById('li_err').style.display = 'block'; }
}

async function doLogout() {
  await fetch('/api/logout', {method:'POST', headers:authHeaders()});
  sessionToken = '';
  sessionStorage.removeItem('sn_token');
  updateAuthUI();
  nav('dashboard');
  showToast('Signed out');
}

// ── Navigation ───────────────────────────────────────────────────────────────
const PROTECTED_PAGES = ['sensors','alerts','management'];

function nav(page) {
  if(PROTECTED_PAGES.includes(page) && !sessionToken) {
    showLoginOverlay(page);
    return;
  }
  document.querySelectorAll('.main > .content > div').forEach(d=>d.style.display='none');
  document.querySelectorAll('.nav-item').forEach(a=>a.classList.remove('active'));
  document.getElementById('view-'+page).style.display='block';
  document.getElementById('nav-'+page).classList.add('active');
  document.getElementById('topbar-title').textContent=TITLES[page]||page;
  if(page==='alerts') renderThresholdPage();
}

function switchTab(g,tabId,btn) {
  document.querySelectorAll('.'+g).forEach(t=>t.classList.remove('active'));
  btn.parentElement.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
  document.getElementById(tabId).classList.add('active');
  btn.classList.add('active');
}

function toggleNetwork() {
  document.getElementById('cluster_div').style.display=
    document.getElementById('m_standalone').checked?'none':'block';
}

function onTypeChange() {
  const t      = document.getElementById('s_type').value;
  const pin1El = document.getElementById('s_pin1');
  const hintEl = document.getElementById('pin1-hint');
  document.getElementById('pin2-field').style.display = (t==='particle') ? 'block' : 'none';
  
  const analogTypes=['mq2','mq135','current','particle'];
  if(t==='vib'){
    pin1El.value='39'; pin1El.readOnly=true;
    if(hintEl) hintEl.textContent='Fixed: GPIO39 / SN — 801S vibration sensor ONLY';
  } else if(t==='relay'){
    pin1El.value='13'; pin1El.readOnly=true;
    if(hintEl) hintEl.textContent='Fixed: GPIO13 — CW-020 IN pin';
  } else if(analogTypes.includes(t)){
    pin1El.readOnly=false;
    if(hintEl) hintEl.innerHTML='<span style="color:var(--red)">ADC1 required</span>: GPIO 32-35 only — ADC2 (13,14,25-27) blocked by Wi-Fi';
  } else {
    pin1El.readOnly=false;
    if(hintEl) hintEl.textContent='Digital: GPIO 13-14, 25-27, 32-35';
  }
}

let toastTimer;
function showToast(msg,type='success') {
  const t=document.getElementById('toast');
  t.textContent=msg; t.className='show '+type;
  clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>{t.className='';},3200);
}

function manualRefresh() {
  const btn=document.getElementById('refreshBtn');
  btn.classList.add('spinning');
  updateData().finally(()=>setTimeout(()=>btn.classList.remove('spinning'),600));
}

// ── Load config ──────────────────────────────────────────────────────────────
async function loadConfig() {
  const res = await fetch('/api/config');
  globalConfig = await res.json();
  apiKey = globalConfig.apiKey || '';
  document.getElementById('m_host').value        = globalConfig.hostname||'';
  document.getElementById('m_factory').value     = globalConfig.factory||'';
  document.getElementById('m_building').value    = globalConfig.building||'';
  document.getElementById('m_room').value        = globalConfig.room||'';
  document.getElementById('m_maxpoints').value   = globalConfig.maxDataPoints||30;
  document.getElementById('m_hotspotpass').value = '';
  document.getElementById('m_apikey').value      = '';
  document.getElementById('m_ssid').value        = globalConfig.ssid||'';
  document.getElementById('m_standalone').checked= !!globalConfig.standalone;
  const keyEl = document.getElementById('current-api-key');
  if(keyEl) keyEl.textContent = apiKey || '(none)';
  toggleNetwork();
  const hn = globalConfig.hostname||'Unnamed Node';
  document.getElementById('brand-hostname').textContent = hn;
  document.title = hn+' - Sensor Node';
  const parts=[globalConfig.factory,globalConfig.building,globalConfig.room].filter(Boolean);
  document.getElementById('footer-loc').textContent = parts.join(' / ')||'No location set';
}

function copyApiKey() {
  navigator.clipboard.writeText(apiKey).then(()=>showToast('API key copied'));
}

// ── Load sensors ─────────────────────────────────────────────────────────────
async function loadSensors() {
  const res = await fetch('/api/sensors');
  sensorList = await res.json();
  ['grid-air','grid-temp','grid-vib','grid-cur','grid-dust','grid-relay'].forEach(id=>document.getElementById(id).innerHTML='');
  charts={}; sensorMeta={}; relayStates={}; relayPending={};
  document.getElementById('sensorTbody').innerHTML='';
  const counts={air:0,temp:0,vib:0,cur:0,dust:0,relay:0};
  const GRID_MAP={mq2:'grid-air',mq135:'grid-air',temp:'grid-temp',vib:'grid-vib',current:'grid-cur',particle:'grid-dust',relay:'grid-relay'};
  const CNT_MAP ={mq2:'air',mq135:'air',temp:'temp',vib:'vib',current:'cur',particle:'dust',relay:'relay'};
  document.getElementById('sensor-empty').style.display=sensorList.length?'none':'flex';
  const tbody=document.getElementById('sensorTbody');

  sensorList.forEach(s=>{
    const m=SENSOR_META[s.type]||{unit:'',color:'#64748b',label:s.type.toUpperCase(),chartLabel:s.type,decimals:1,hasThreshold:true,rangeMin:0,rangeMax:100};
    sensorMeta[s.id]={type:s.type,pin1:s.pin1,name:s.name,unit:m.unit,decimals:m.decimals,rangeMin:m.rangeMin,rangeMax:m.rangeMax};
    if(s.type==='relay'){ s.relayAutoOff=!!s.relayAutoOff; s.relayLinkedId=s.relayLinkedId||''; }
    const isGas=(s.type==='mq2'||s.type==='mq135');

    const tr=document.createElement('tr');
    const nameTd=document.createElement('td');
    const ns=document.createElement('span'); ns.style.fontWeight='500'; ns.textContent=s.name; nameTd.appendChild(ns);
    const typeTd=document.createElement('td');
    typeTd.innerHTML=`<span class="type-tag">${escH(m.label)}</span>`;
    const pinTd=document.createElement('td');
    pinTd.style.fontFamily='var(--mono)'; pinTd.style.fontSize='12px'; pinTd.style.color='var(--text-sec)';
    pinTd.textContent = s.pin2 && s.pin2>=0 && s.type==='particle' ? 'D:'+s.pin1+' L:'+s.pin2 : 'GPIO '+s.pin1;
    const intTd=document.createElement('td');
    intTd.style.fontFamily='var(--mono)'; intTd.style.fontSize='12px'; intTd.style.color='var(--text-sec)';
    intTd.textContent=(s.interval)+'ms';
    const valTd=document.createElement('td');
    valTd.id='td-val-'+s.id; valTd.style.fontFamily='var(--mono)'; valTd.style.fontSize='12px'; valTd.textContent='-';
    const airTd=document.createElement('td');
    airTd.id='td-air-'+s.id;
    if(isGas) airTd.innerHTML='<span class="type-tag">-</span>';
    else { airTd.style.color='var(--text-dim)'; airTd.style.fontSize='12px'; airTd.textContent='N/A'; }
    const actTd=document.createElement('td');
    const delBtn=document.createElement('button');
    delBtn.className='danger'; delBtn.textContent='Remove'; delBtn.onclick=()=>deleteSensor(s.id);
    actTd.appendChild(delBtn);
    [nameTd,typeTd,pinTd,intTd,valTd,airTd,actTd].forEach(td=>tr.appendChild(td));
    tbody.appendChild(tr);

    if(CNT_MAP[s.type]) counts[CNT_MAP[s.type]]++;
    const targetGrid=GRID_MAP[s.type]||'grid-air';
    const div=document.createElement('div');
    div.className='sensor-card'; div.id='card-'+s.id;

    if(s.type==='vib'){
      div.innerHTML=`<div class="card-top"><span class="card-name">${escH(s.name)}</span><span class="type-pill">${escH(m.label)}</span></div>
        <div class="card-value-row" style="align-items:center;gap:10px">
          <span id="vib-state-${escH(s.id)}" class="threshold-state-badge ok" style="font-size:12px;padding:4px 12px">QUIET</span>
        </div>
        <div class="chart-container"><canvas></canvas></div>`;
    } else if(s.type==='relay'){
      div.innerHTML=`<div class="card-top"><span class="card-name">${escH(s.name)}</span><span class="type-pill">${escH(m.label)}</span></div>
        <div class="card-value-row" style="align-items:center;gap:12px;margin-bottom:12px">
          <span id="relay-badge-${escH(s.id)}" class="relay-badge off">OFF</span>
          <button class="secondary" style="padding:6px 14px;font-size:12px;font-family:var(--mono)"
            onclick="toggleRelay('${escH(s.id)}')">Turn ON</button>
        </div>
        <div id="relay-autoinfo-${escH(s.id)}" style="font-size:11px;color:var(--text-sec);margin-bottom:8px"></div>
        <div class="chart-container"><canvas></canvas></div>`;
    } else {
      const airBadge=isGas?`<div id="airbadge-${escH(s.id)}" class="air-status-badge warn"><span class="dot"></span><span id="airstatus-${escH(s.id)}">Calibrating</span></div>`:'';
      div.innerHTML=`<div class="card-top"><span class="card-name">${escH(s.name)}</span><span class="type-pill">${escH(m.label)}</span></div>
        <div class="card-value-row"><span class="card-value" id="val-${escH(s.id)}">-</span><span class="card-unit">${escH(m.unit)}</span></div>
        ${airBadge}
        <div id="thbar-wrap-${escH(s.id)}" style="display:none" class="threshold-bar-wrap">
          <div class="threshold-bar-label"><span id="thbar-low-${escH(s.id)}"></span><span id="thbar-high-${escH(s.id)}"></span></div>
          <div class="threshold-bar"><div class="bar-fill" id="thbar-fill-${escH(s.id)}" style="width:0%"></div></div>
        </div>
        <div class="chart-container"><canvas></canvas></div>`;
    }

    document.getElementById(targetGrid).appendChild(div);

    if (typeof Chart === 'undefined') return;

    const isVibChart = (s.type==='vib');
    charts[s.id]=new Chart(div.querySelector('canvas'),{
      type:'line',
      data:{labels:[],datasets:[{
        label:m.chartLabel,
        data:[],
        borderColor:m.color,
        backgroundColor:m.color+'20',
        borderWidth:isVibChart?2:1.5,
        pointRadius:0,
        fill:true,
        tension:isVibChart?0:0.4,
        stepped:isVibChart?'before':false
      }]},
      options:{
        responsive:true,maintainAspectRatio:false,animation:false,
        plugins:{
          legend:{display:false},
          tooltip:{
            backgroundColor:'rgba(15,23,42,0.88)',
            callbacks:{
              label:item=>item.dataset.label+': '+item.parsed.y.toFixed(1)
            }
          }
        },
        scales:{
          x:{display:true,grid:{display:false},ticks:{maxRotation:0,maxTicksLimit:5,autoSkip:true}},
          y:{
            grid:{color:'rgba(0,0,0,0.04)'},
            ticks:{
              callback:v=>v.toFixed(1),
              maxTicksLimit: 4
            }
          }
        }
      }
    });
  });

  document.getElementById('sensor-count-badge').textContent=sensorList.length+' SENSOR'+(sensorList.length!==1?'S':'');
  ['air','temp','vib','cur','dust','relay'].forEach(k=>document.getElementById('cnt-'+k).textContent=counts[k]);
  renderPinMap();
  startSensorPolls();
}

function renderThresholdPage() {
  const container=document.getElementById('threshold-list');
  const empty=document.getElementById('threshold-empty');
  container.innerHTML='';
  if(!sensorList.length){empty.style.display='flex';return;}
  empty.style.display='none';

  sensorList.forEach(s=>{
    const m=SENSOR_META[s.type]||{unit:'',label:s.type.toUpperCase(),hasThreshold:true,decimals:1};
    const isRelay=(s.type==='relay');
    const isVib=(s.type==='vib');
    const card=document.createElement('div');
    card.className='card'; card.style.marginBottom='12px';

    const highVal=(s.thresholdHigh!==null&&s.thresholdHigh!==undefined)?s.thresholdHigh:'';
    const lowVal =(s.thresholdLow !==null&&s.thresholdLow !==undefined)?s.thresholdLow :'';
    const enabled=!!s.alertEnabled;

    let formHtml='';
    if(isVib){
      formHtml=`<p style="font-size:12px;color:var(--text-sec)">The 801S vibration sensor alerts automatically whenever a vibration event is latched.</p>`;
    } else if(isRelay){
      const acsList=sensorList.filter(x=>x.type==='current');
      const opts=acsList.map(x=>`<option value="${escH(x.id)}"${s.relayLinkedId===x.id?' selected':''}>${escH(x.name)}</option>`).join('');
      formHtml=`
        <p style="font-size:12px;color:var(--text-sec);margin-bottom:14px">Relay auto-off protection config:</p>
        <div style="display:grid;grid-template-columns:auto 1fr auto;gap:12px;align-items:end" id="thform-${escH(s.id)}">
          <div class="form-field">
            <label>Auto-Off on Overcurrent</label>
            <div class="toggle-row" style="padding:8px 12px"><input type="checkbox" id="th-rao-${escH(s.id)}" ${s.relayAutoOff?'checked':''}> Enabled</div>
          </div>
          <div class="form-field">
            <label>Linked ACS712 Sensor</label>
            <select id="th-rlink-${escH(s.id)}"><option value="">-- select sensor --</option>${opts}</select>
          </div>
          <div class="form-field"><button onclick="saveRelayAutoOff('${escH(s.id)}')">Save</button></div>
        </div>`;
    } else {
      formHtml=`
        <div class="threshold-form" id="thform-${escH(s.id)}">
          <div class="form-field">
            <label>Low Threshold (${escH(m.unit)})</label>
            <input type="number" step="any" id="th-low-${escH(s.id)}" value="${escH(String(lowVal))}">
          </div>
          <div class="form-field">
            <label>High Threshold (${escH(m.unit)})</label>
            <input type="number" step="any" id="th-high-${escH(s.id)}" value="${escH(String(highVal))}">
          </div>
          <div class="form-field">
            <label>Alerts Enabled</label>
            <div class="toggle-row" style="padding:8px 12px"><input type="checkbox" id="th-en-${escH(s.id)}" ${enabled?'checked':''}> Enabled</div>
          </div>
          <div class="form-field"><button onclick="saveThreshold('${escH(s.id)}')">Save</button></div>
        </div>`;
    }

    card.innerHTML=`<div style="display:flex;align-items:center;gap:12px;margin-bottom:16px"><span style="font-weight:600">${escH(s.name)}</span><span id="th-state-badge-${escH(s.id)}" class="threshold-state-badge off" style="margin-left:auto">-</span><span id="th-live-${escH(s.id)}" style="font-family:var(--mono);font-size:13px">-</span></div>${formHtml}`;
    container.appendChild(card);
  });
}

async function saveThreshold(id) {
  const highRaw = document.getElementById('th-high-'+id).value.trim();
  const lowRaw  = document.getElementById('th-low-'+id).value.trim();
  const payload={id,
    alertEnabled:  document.getElementById('th-en-'+id).checked,
    thresholdHigh: highRaw !== '' ? parseFloat(highRaw) : null,
    thresholdLow:  lowRaw  !== '' ? parseFloat(lowRaw)  : null};
  const res=await fetch('/api/set_threshold',{method:'POST',headers:authHeaders(),body:JSON.stringify(payload)});
  if(res.status===401){sessionToken='';sessionStorage.removeItem('sn_token');showLoginOverlay('alerts');return;}
  if(res.ok){
    showToast('Threshold saved');
    // Sync sensorList so inputs stay correct if user navigates away and back
    const s=sensorList.find(x=>x.id===id);
    if(s){ s.alertEnabled=payload.alertEnabled; s.thresholdHigh=payload.thresholdHigh; s.thresholdLow=payload.thresholdLow; }
  } else showToast('Save failed','error');
}

async function saveRelayAutoOff(id) {
  const autoOff  = document.getElementById('th-rao-'+id).checked;
  const linkedId = document.getElementById('th-rlink-'+id).value;
  const res=await fetch('/api/relay_autooff',{method:'POST',headers:authHeaders(),body:JSON.stringify({id,autoOff,linkedId})});
  if(res.status===401){sessionToken='';sessionStorage.removeItem('sn_token');showLoginOverlay('alerts');return;}
  if(res.ok){
    showToast('Relay auto-off saved');
    const s=sensorList.find(x=>x.id===id);
    if(s){ s.relayAutoOff=autoOff; s.relayLinkedId=linkedId; }
  } else showToast('Save failed','error');
}

async function factoryReset() {
  if(!confirm('DANGER: This will erase ALL sensors and settings. Are you sure?')) return;
  if(!confirm('LAST WARNING: This cannot be undone. Proceed with format?')) return;
  
  showToast('Formatting storage...', 'error');
  const res = await fetch('/api/factory_reset', {method:'POST', headers:authHeaders()});
  if(res.ok) {
    alert('Factory reset complete. The device is rebooting to defaults.');
    window.location.reload();
  }
}

let sensorTimers = {};

async function updateSensorData(id) {
  try {
    const statusRes = await fetch(`/api/status`); 
    const statusData = await statusRes.json();
    const s = statusData[id];
    if (!s) return;

    const sm = sensorMeta[id] || {};
    const maxPoints = globalConfig.maxDataPoints || 30;
    const now = new Date();
    const tl = now.getHours().toString().padStart(2,'0')+':'+now.getMinutes().toString().padStart(2,'0')+':'+now.getSeconds().toString().padStart(2,'0');

    const valEl = document.getElementById('val-' + id);
    if (valEl) valEl.textContent = parseFloat(s.value).toFixed(1);
    const tdVal = document.getElementById('td-val-' + id);
    if (tdVal) tdVal.textContent = parseFloat(s.value).toFixed(1) + (sm.unit ? ' ' + sm.unit : '');

    if (sm.type === 'relay') {
      // Skip poll-driven update while a toggle POST is in-flight to avoid
      // overwriting the UI with stale server state mid-request.
      if (!relayPending[id]) {
        const badge = document.getElementById('relay-badge-' + id);
        const btn = document.querySelector(`#card-${id} button`);
        const isOn = (s.value >= 1);
        relayStates[id] = isOn;
        if (badge) {
          badge.className = 'relay-badge ' + (isOn ? 'on' : 'off');
          badge.textContent = isOn ? 'POWER CUT' : 'SAFE/ON';
        }
        if (btn) btn.textContent = isOn ? 'Restore Power' : 'Cut Power';
      }
    }

    if (sm.type === 'vib') {
      const vibBadge = document.getElementById('vib-state-' + id);
      if (vibBadge) {
        const isActive = s.value >= 1;
        vibBadge.className = 'threshold-state-badge ' + (isActive ? 'high' : 'ok');
        vibBadge.textContent = isActive ? 'VIBRATING' : 'QUIET';
      }
    }

    if (s.airStatus) {
      const badgeEl = document.getElementById('airbadge-' + id);
      const statusEl = document.getElementById('airstatus-' + id);
      if (badgeEl && statusEl) {
        statusEl.textContent = s.airStatus;
        const cls = s.airStatus === 'Clean Air' ? 'clean' : s.airStatus === 'Calibrating' ? 'warn' : 'danger';
        badgeEl.className = 'air-status-badge ' + cls;
      }
      const tdAir = document.getElementById('td-air-' + id);
      if (tdAir) tdAir.innerHTML = '<span class="type-tag">' + escH(s.airStatus) + '</span>';
    }

    if (charts[id]) {
      charts[id].data.labels.push(tl);
      charts[id].data.datasets[0].data.push(s.value);
      while (charts[id].data.labels.length > maxPoints) {
        charts[id].data.labels.shift();
        charts[id].data.datasets[0].data.shift();
      }
      charts[id].update('none');
    }
  } catch (e) {}
}

async function updateData() {
  try {
    const res = await fetch('/api/status');
    const data = await res.json();
    const alerts = [];
    for (const id in data) {
      const st = data[id];
      if (st.alertState && st.alertState !== 'ok') {
        const sn = sensorList.find(x => x.id === id);
        alerts.push({name: sn ? sn.name : id, state: st.alertState});
      }
    }

    // Alert banner (dashboard)
    const banner = document.getElementById('alert-banner');
    const items  = document.getElementById('alert-items');
    if (banner && items) {
      if (alerts.length) {
        banner.classList.add('show');
        items.innerHTML = alerts.map(a => `<span class="alert-chip">${escH(a.name)}: ${escH(a.state.toUpperCase())}</span>`).join('');
      } else {
        banner.classList.remove('show');
        items.innerHTML = '';
      }
    }

    // Active-alert count badge in nav
    const ab = document.getElementById('alerts-active-badge');
    if (ab) ab.textContent = alerts.length + ' ACTIVE';

    // Live readings + state badges on the Alerts page
    const onAlerts = document.getElementById('view-alerts') &&
                     document.getElementById('view-alerts').style.display !== 'none';
    if (onAlerts) {
      for (const id in data) {
        const st  = data[id];
        const sm  = sensorMeta[id] || {};
        const thB = document.getElementById('th-state-badge-' + id);
        const thL = document.getElementById('th-live-' + id);
        if (thB) {
          const aState = st.alertState || 'ok';
          thB.className   = 'threshold-state-badge ' + aState;
          thB.textContent = aState.toUpperCase();
        }
        if (thL) {
          const val = parseFloat(st.value || 0);
          thL.textContent = val.toFixed(sm.decimals != null ? sm.decimals : 1) +
                            (sm.unit ? ' ' + sm.unit : '');
        }
      }
    }
  } catch(e) {}
}

function startSensorPolls() {
  for (const id in sensorTimers) clearInterval(sensorTimers[id]);
  sensorTimers = {};
  sensorList.forEach(s => {
    const interval = Math.max(s.interval || 2000, 500);
    sensorTimers[s.id] = setInterval(() => updateSensorData(s.id), interval);
    updateSensorData(s.id);
  });
}

async function addSensor() {
  const body = {
    name: document.getElementById('s_name').value.trim(),
    type: document.getElementById('s_type').value,
    pin1: parseInt(document.getElementById('s_pin1').value),
    pin2: parseInt(document.getElementById('s_pin2').value)||-1,
    interval: parseInt(document.getElementById('s_interval').value)
  };
  const res=await fetch('/api/add_sensor',{method:'POST',headers:authHeaders(),body:JSON.stringify(body)});
  if(res.ok){ showToast('Sensor added'); loadSensors(); }
  else { const t=await res.text(); showToast(t,'error'); }
}

async function deleteSensor(id) {
  if(!confirm('Remove sensor?')) return;
  await fetch('/api/delete_sensor',{method:'POST',headers:authHeaders(),body:JSON.stringify({id})});
  showToast('Removed'); loadSensors();
}

async function toggleRelay(id) {
  if (relayPending[id]) return;                    // ignore double-click in flight
  const currentState = relayStates[id] === true;   // read software state, not badge CSS
  const newState = !currentState;
  relayPending[id] = true;

  const res = await fetch('/api/relay_control',{method:'POST',headers:authHeaders(),body:JSON.stringify({id,state:newState})});
  relayPending[id] = false;

  if (res.status === 401) { sessionToken=''; sessionStorage.removeItem('sn_token'); showLoginOverlay('dashboard'); return; }
  if (!res.ok) { showToast('Relay control failed','error'); return; }

  // Update client state and UI immediately — don't wait for the next poll.
  // Without this, the badge stays stale and the next click sends the wrong state.
  relayStates[id] = newState;
  const badge = document.getElementById('relay-badge-' + id);
  const btn   = document.querySelector(`#card-${id} button`);
  if (badge) { badge.className = 'relay-badge ' + (newState ? 'on' : 'off'); badge.textContent = newState ? 'POWER CUT' : 'SAFE/ON'; }
  if (btn)   btn.textContent = newState ? 'Restore Power' : 'Cut Power';
}

function renderPinMap() {
  const grid=document.getElementById('pin-map-grid');
  if(!grid) return; grid.innerHTML='';
  const SENSOR_PINS=[13,14,25,26,27,32,33,34,35,39];
  for(let p=0;p<=39;p++){
    const cell=document.createElement('div'); cell.className='pin-cell';
    if(SENSOR_PINS.includes(p)) cell.classList.add('available');
    else cell.classList.add('unavail');
    cell.innerHTML=`<div style="font-weight:600">G${p}</div>`;
    grid.appendChild(cell);
  }
}

async function saveSettings() {
  const payload={
    hostname:document.getElementById('m_host').value,
    factory: document.getElementById('m_factory').value,
    building:document.getElementById('m_building').value,
    room:    document.getElementById('m_room').value,
    standalone:document.getElementById('m_standalone').checked,
    ssid:    document.getElementById('m_ssid').value,
    pass:    document.getElementById('m_pass').value,
    maxDataPoints:parseInt(document.getElementById('m_maxpoints').value)
  };
  const res=await fetch('/api/save_config',{method:'POST',headers:authHeaders(),body:JSON.stringify(payload)});
  if(res.status===401){sessionToken='';sessionStorage.removeItem('sn_token');showLoginOverlay('management');return;}
  if(!res.ok){showToast('Save failed','error');return;}
  showToast('Saved'); loadConfig();
}

async function saveWebLogin() {
  const user=document.getElementById('m_webuser').value;
  const pass=document.getElementById('m_webpass').value;
  if(!user&&!pass){showToast('Nothing to update');return;}
  const payload={};
  if(user) payload.username=user;
  if(pass) payload.password=pass;
  const res=await fetch('/api/save_web_login',{method:'POST',headers:authHeaders(),body:JSON.stringify(payload)});
  if(res.status===401){sessionToken='';sessionStorage.removeItem('sn_token');showLoginOverlay('management');return;}
  if(!res.ok){showToast('Save failed','error');return;}
  showToast('Login updated');
  document.getElementById('m_webuser').value='';
  document.getElementById('m_webpass').value='';
}

async function saveSecuritySettings() {
  const newKey=document.getElementById('m_apikey').value;
  const newHp=document.getElementById('m_hotspotpass').value;
  if(!newKey&&!newHp){showToast('Nothing to update');return;}
  const payload={};
  if(newKey) payload.apiKey=newKey;
  if(newHp) payload.hotspotPassword=newHp;
  const res=await fetch('/api/save_config',{method:'POST',headers:authHeaders(),body:JSON.stringify(payload)});
  if(res.status===401){sessionToken='';sessionStorage.removeItem('sn_token');showLoginOverlay('management');return;}
  if(!res.ok){showToast('Save failed','error');return;}
  showToast('Security updated');
  document.getElementById('m_apikey').value='';
  document.getElementById('m_hotspotpass').value='';
  loadConfig();
}

async function switchToAPMode() {
  if(!confirm('Switch device to Standalone AP mode and reboot?')) return;
  const res = await fetch('/api/config');
  const cfg = await res.json();
  cfg.standalone = true;
  await fetch('/api/save_config',{method:'POST',headers:authHeaders(),body:JSON.stringify(cfg)});
  showToast('Switching to AP mode...');
  setTimeout(()=>rebootDevice(), 1500);
}

async function rebootDevice() {
  await fetch('/api/reboot',{method:'POST',headers:authHeaders()});
  showToast('Rebooting...');
}

updateAuthUI();
loadConfig().then(async ()=>{
  await loadSensors();
  setInterval(updateData,1000);
});
</script>
</body>
</html>
)rawliteral";

// ============================================================
// C++ REQUEST HANDLERS
// ============================================================

void handleRoot() { server.send_P(200, "text/html", DASHBOARD_HTML); }

void handleAPIData()
{
    String targetId = server.arg("id");
    JsonDocument doc;
    for (const auto& s : activeSensors) {
        if (targetId.isEmpty() || s.id == targetId) {
            doc[s.id] = s.lastValue;
        }
    }
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleAPIStatus()
{
    String targetId = server.arg("id");
    JsonDocument doc;
    for (const auto& s : activeSensors) {
        if (targetId.isEmpty() || s.id == targetId) {
            JsonObject obj = doc[s.id].to<JsonObject>();
            obj["value"]      = s.lastValue;
            obj["alertState"] = s.alertState;
            if (s.type == "mq2" || s.type == "mq135")
                obj["airStatus"] = getAirStatus(s.pin1);
        }
    }
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleAPISensors()
{
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (const auto& s : activeSensors) {
        JsonObject obj = array.add<JsonObject>();
        obj["id"]           = s.id;
        obj["name"]         = s.name;
        obj["type"]         = s.type;
        obj["pin1"]         = s.pin1;
        obj["pin2"]         = s.pin2;
        obj["interval"]     = s.readInterval;
        obj["alertEnabled"]  = s.alertEnabled;
        obj["relayAutoOff"]  = s.relayAutoOff;
        obj["relayLinkedId"] = s.relayLinkedId;
        // Send NaN thresholds as JSON null so the JS side can distinguish
        // "not set" from 0. Without these fields sensorList has no threshold
        // values and the Alerts page inputs always render blank.
        if (!isnan(s.thresholdHigh)) obj["thresholdHigh"] = s.thresholdHigh;
        else                          obj["thresholdHigh"] = nullptr;
        if (!isnan(s.thresholdLow))  obj["thresholdLow"]  = s.thresholdLow;
        else                          obj["thresholdLow"]  = nullptr;
    }
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleAPIConfig()
{
    JsonDocument doc;
    doc["hostname"]        = systemConfig.hostname;
    doc["factory"]         = systemConfig.factory;
    doc["building"]        = systemConfig.building;
    doc["room"]            = systemConfig.room;
    doc["standalone"]      = systemConfig.standalone;
    doc["ssid"]            = systemConfig.wifiSSID;
    doc["maxDataPoints"]   = systemConfig.maxDataPoints;
    doc["apiKey"]          = systemConfig.apiKey;
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleAddSensor()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!checkPayload(1024)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    
    DynamicSensor s;
    s.id            = nextSensorId();
    s.name          = req["name"] | "New Sensor";
    s.type          = req["type"] | "temp";
    s.pin1          = req["pin1"] | -1;
    s.pin2          = req["pin2"] | -1;
    s.readInterval  = req["interval"] | 2000;
    s.lastValue     = 0.0f;
    s.lastReadTime  = 0;
    s.alertState    = "ok";
    s.alertEnabled  = false;
    s.thresholdHigh = NAN;
    s.thresholdLow  = NAN;
    s.relayOn       = false;
    s.relayAutoOff  = false;
    s.relayLinkedId = "";

    // Validation
    if (s.readInterval < 100) s.readInterval = 100;
    if (s.readInterval > 5000) s.readInterval = 5000;

    bool needsADC = (s.type == "mq2" || s.type == "mq135" || s.type == "current" || s.type == "particle");
    if (needsADC && !isADC1Pin(s.pin1)) {
        server.send(400, "text/plain", "Error: Analog sensors MUST use ADC1 (32-35).");
        return;
    }
    if (s.type == "particle" && (s.pin2 < 0 || !isValidGPIO(s.pin2))) {
        server.send(400, "text/plain", "Error: Particle sensor requires a valid digital LED pin (GPIO 13-14, 25-27, 32-35).");
        return;
    }

    addSensorConfig(s);
    initSensors(); // Re-init all hardware
    server.send(200, "text/plain", "OK");
}

void handleSetThreshold()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!checkPayload(512)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    String targetId = req["id"];
    for (auto& s : activeSensors) {
        if (s.id == targetId) {
            s.alertEnabled  = req["alertEnabled"] | false;
            s.thresholdHigh = req["thresholdHigh"].isNull() ? NAN : req["thresholdHigh"].as<float>();
            s.thresholdLow  = req["thresholdLow"].isNull()  ? NAN : req["thresholdLow"].as<float>();
            saveSensors();
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(404, "text/plain", "Not Found");
}

void handleDeleteSensor()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!checkPayload(256)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    String targetId = req["id"];
    for (auto it = activeSensors.begin(); it != activeSensors.end(); ++it) {
        if (it->id == targetId) {
            activeSensors.erase(it);
            saveSensors();
            initSensors(); // Re-init to clean hardware state
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(404, "text/plain", "Not Found");
}

void handleSaveConfig()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!checkPayload(1024)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    systemConfig.hostname      = req["hostname"] | systemConfig.hostname;
    systemConfig.factory       = req["factory"] | systemConfig.factory;
    systemConfig.building      = req["building"] | systemConfig.building;
    systemConfig.room          = req["room"] | systemConfig.room;
    systemConfig.standalone    = req["standalone"] | systemConfig.standalone;
    systemConfig.maxDataPoints = req["maxDataPoints"] | systemConfig.maxDataPoints;
    systemConfig.wifiSSID      = req["ssid"] | systemConfig.wifiSSID;
    if (req["pass"].is<String>() && !req["pass"].as<String>().isEmpty())
        systemConfig.wifiPassword = req["pass"].as<String>();
    if (req["apiKey"].is<String>() && !req["apiKey"].as<String>().isEmpty())
        systemConfig.apiKey = req["apiKey"].as<String>();
    if (req["hotspotPassword"].is<String>() && !req["hotspotPassword"].as<String>().isEmpty())
        systemConfig.hotspotPassword = req["hotspotPassword"].as<String>();
    saveConfig();
    server.send(200, "text/plain", "OK");
}

void handleLogin()
{
    if (!checkPayload(256)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    if (req["username"] == systemConfig.webUsername && req["password"] == systemConfig.webPassword) {
        s_sessionToken  = chipDerivedSecret("t") + String(millis());
        s_sessionExpiry = millis() + SESSION_MS;  // S4: start expiry window
        server.send(200, "application/json", "{\"token\":\"" + s_sessionToken + "\"}");
    } else {
        server.send(401, "text/plain", "Invalid");
    }
}

void handleLogout() { s_sessionToken = ""; server.send(200, "text/plain", "OK"); }

void handleSaveWebLogin()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!checkPayload(256)) return;
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    if (req["username"].is<String>()) systemConfig.webUsername = req["username"].as<String>();
    if (req["password"].is<String>()) systemConfig.webPassword = req["password"].as<String>();
    saveConfig();
    server.send(200, "text/plain", "OK");
}

void handleFactoryReset()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    server.send(200, "text/plain", "OK");
    delay(500);
    Serial.println("[SYS] Factory reset triggered from Web UI...");
    LittleFS.format();
    ESP.restart();
}

void handleReboot()

{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    server.send(200, "text/plain", "OK");
    delay(500); ESP.restart();
}

void handleRelayControl()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    String id = req["id"];
    bool state = req["state"];
    for (auto& s : activeSensors) {
        if (s.id == id) {
            setRelayState(s, state);
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(404, "text/plain", "Sensor not found");
}

void handleRelayAutoOff()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    JsonDocument req;
    deserializeJson(req, server.arg("plain"));
    String id = req["id"];
    for (auto& s : activeSensors) {
        if (s.id == id) {
            s.relayAutoOff = req["autoOff"] | false;
            s.relayLinkedId = req["linkedId"] | "";
            saveSensors();
            server.send(200, "text/plain", "OK");
            return;
        }
    }
}

void startWebServer()
{
    const char* h[] = { "X-API-Key", "X-Session-Token" };
    server.collectHeaders(h, 2);
    server.on("/", handleRoot);
    server.on("/api/data", handleAPIData);
    server.on("/api/status", handleAPIStatus);
    server.on("/api/sensors", handleAPISensors);
    server.on("/api/config", handleAPIConfig);
    server.on("/api/add_sensor", HTTP_POST, handleAddSensor);
    server.on("/api/delete_sensor", HTTP_POST, handleDeleteSensor);
    server.on("/api/save_config", HTTP_POST, handleSaveConfig);
    server.on("/api/set_threshold", HTTP_POST, handleSetThreshold);
    server.on("/api/login", HTTP_POST, handleLogin);
    server.on("/api/logout", HTTP_POST, handleLogout);
    server.on("/api/save_web_login", HTTP_POST, handleSaveWebLogin);
    server.on("/api/factory_reset", HTTP_POST, handleFactoryReset);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/api/relay_control", HTTP_POST, handleRelayControl);
    server.on("/api/relay_autooff", HTTP_POST, handleRelayAutoOff);
    server.begin();
}
