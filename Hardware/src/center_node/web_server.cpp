#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include "web_server.h"
#include "config.h"
#include "node_manager.h"
#include "db_sync.h"

WebServer server(80);

// ── Session auth ─────────────────────────────────────────────────────────────
static String        s_sessionToken  = "";
static unsigned long s_sessionExpiry = 0;
static const unsigned long SESSION_MS = 4UL * 3600UL * 1000UL; // 4-hour sliding window

static bool isAuthorized()
{
    String tok = server.header("X-Session-Token");
    if (!s_sessionToken.isEmpty() && tok == s_sessionToken) {
        if (millis() < s_sessionExpiry) {
            s_sessionExpiry = millis() + SESSION_MS; // S4: slide window on each use
            return true;
        }
        s_sessionToken  = "";  // S4: expired — force re-login
        s_sessionExpiry = 0;
    }
    String key = server.header("X-API-Key");
    return (!systemConfig.apiKey.isEmpty() && key == systemConfig.apiKey);
}

static void sendUnauthorized()
{
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
}

// I10: reject oversized POST bodies before JSON parsing
static bool checkPayload(size_t limit = 2048)
{
    if (server.arg("plain").length() > limit) {
        server.send(413, "application/json", "{\"error\":\"Payload too large\"}");
        return false;
    }
    return true;
}

// ── Dashboard HTML ────────────────────────────────────────────────────────────
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SIMEM Central Node</title>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=IBM+Plex+Sans:wght@300;400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg:#f1f5f9;--sur:#fff;--raised:#f8fafc;--hov:#f1f5f9;
  --bdr:#e2e8f0;--bdrhi:#cbd5e1;
  --acc:#2563eb;--acc-dim:rgba(37,99,235,.08);--acc-bg:#eff6ff;
  --grn:#16a34a;--grn-dim:rgba(22,163,74,.1);
  --amb:#d97706;--amb-dim:rgba(215,119,6,.1);
  --red:#dc2626;--red-dim:rgba(220,38,38,.1);
  --blu:#0284c7;--blu-dim:rgba(2,132,199,.1);
  --tp:#0f172a;--ts:#64748b;--td:#94a3b8;
  --mono:'IBM Plex Mono',monospace;--sans:'IBM Plex Sans',sans-serif;
  --radius:10px;
}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--sans);background:var(--bg);color:var(--tp);display:flex;min-height:100vh;font-size:14px}
/* ── Sidebar ── */
.sb{width:220px;min-height:100vh;background:var(--sur);border-right:1px solid var(--bdr);display:flex;flex-direction:column;position:fixed;top:0;left:0;bottom:0;z-index:100}
.sb-brand{padding:16px 18px;border-bottom:1px solid var(--bdr);display:flex;align-items:center;gap:10px}
.sb-logo{width:32px;height:32px;background:var(--acc);border-radius:8px;display:flex;align-items:center;justify-content:center;flex-shrink:0;color:#fff;font-size:15px;font-weight:700;font-family:var(--mono)}
.sb-brand .t1{font-size:14px;font-weight:600;line-height:1.2}
.sb-brand .t2{font-size:10px;color:var(--td);font-family:var(--mono);line-height:1}
.sb-nav{padding:10px 8px;flex:1}
.ni{display:flex;align-items:center;gap:9px;padding:9px 12px;border-radius:7px;color:var(--ts);cursor:pointer;font-size:13px;font-weight:500;transition:background .15s,color .15s;user-select:none;margin-bottom:1px}
.ni:hover{background:var(--hov);color:var(--tp)}
.ni.active{background:var(--acc-dim);color:var(--acc);font-weight:600}
.ni svg{width:15px;height:15px;flex-shrink:0;opacity:.7}
.ni.active svg{opacity:1}
.ni-badge{margin-left:auto;background:var(--red);color:#fff;font-size:10px;padding:1px 7px;border-radius:99px;font-family:var(--mono);display:none;font-weight:600}
.sb-sect{font-size:10px;font-weight:600;text-transform:uppercase;letter-spacing:.07em;color:var(--td);padding:12px 12px 4px}
.sb-foot{padding:12px 16px;border-top:1px solid var(--bdr)}
.sync-row{display:flex;align-items:center;gap:6px;font-size:11px;color:var(--ts);font-family:var(--mono)}
.sdot{width:7px;height:7px;border-radius:50%;background:var(--td);flex-shrink:0;transition:background .3s}
.sdot.ok{background:var(--grn)}.sdot.fail{background:var(--red)}
.sb-foot .sub{font-size:10px;color:var(--td);font-family:var(--mono);margin-top:3px;padding-left:13px}
/* ── Main layout ── */
.main{margin-left:220px;flex:1;display:flex;flex-direction:column;min-height:100vh}
.topbar{height:52px;border-bottom:1px solid var(--bdr);display:flex;align-items:center;padding:0 24px;gap:10px;background:var(--sur);position:sticky;top:0;z-index:50}
.tb-title{font-size:15px;font-weight:600;flex:1;color:var(--tp)}
.tb-btn{padding:5px 13px;border:1px solid var(--bdr);border-radius:6px;font-size:12px;font-weight:500;cursor:pointer;background:none;color:var(--ts);transition:all .15s;display:flex;align-items:center;gap:5px}
.tb-btn:hover{border-color:var(--acc);color:var(--acc);background:var(--acc-dim)}
.tb-logout:hover{border-color:var(--red)!important;color:var(--red)!important;background:var(--red-dim)!important}
.content{padding:24px;flex:1}
/* ── Stat cards ── */
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;margin-bottom:24px;position:relative;z-index:2}
.sc{background:var(--sur);border:1px solid var(--bdr);border-radius:var(--radius);padding:18px 20px;display:flex;align-items:center;gap:16px}
.sc-icon{width:42px;height:42px;border-radius:9px;display:flex;align-items:center;justify-content:center;font-size:18px;flex-shrink:0}
.sc-icon.total{background:#f1f5f9}
.sc-icon.online{background:var(--grn-dim)}
.sc-icon.alerts{background:var(--red-dim)}
.sc-info{min-width:0}
.sc-lbl{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;color:var(--ts);margin-bottom:4px}
.sc-val{font-size:26px;font-weight:700;font-family:var(--mono);line-height:1}
.sc-val.g{color:var(--grn)}.sc-val.r{color:var(--red)}.sc-val.n{color:var(--tp)}
/* ── Node grid ── */
.ngrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:16px;position:relative;z-index:2}
.nc{background:var(--sur);border:1px solid var(--bdr);border-radius:var(--radius);overflow:hidden;transition:box-shadow .2s,border-color .2s}
.nc:hover{box-shadow:0 4px 20px rgba(0,0,0,.07)}
.nc-bar{height:3px;background:var(--bdr)}
.nc.alert .nc-bar{background:var(--red)}
.nc.offline .nc-bar{background:var(--td)}
.nc.ok-node .nc-bar{background:var(--grn)}
.nc-body{padding:14px 16px}
.nc-head{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:12px;padding-bottom:10px;border-bottom:1px solid var(--bdr)}
.nc-title .nc-name{font-weight:600;font-size:14px;color:var(--tp)}
.nc-title .nc-ip{font-family:var(--mono);font-size:10px;color:var(--ts);margin-top:2px}
.nc-right{display:flex;flex-direction:column;align-items:flex-end;gap:4px}
.badge{font-family:var(--mono);font-size:10px;padding:2px 8px;border-radius:99px;font-weight:600;display:inline-block;letter-spacing:.02em}
.b-on{background:var(--grn-dim);color:var(--grn)}
.b-off{background:var(--red-dim);color:var(--red)}
.b-hi{background:var(--amb-dim);color:var(--amb)}
.b-lo{background:var(--blu-dim);color:var(--blu)}
.b-gas{background:var(--red-dim);color:var(--red)}
.b-act{background:var(--red-dim);color:var(--red)}
.nc-ts{font-size:10px;color:var(--td);font-family:var(--mono)}
.rrow{display:flex;align-items:center;padding:6px 0;font-size:12.5px;border-bottom:1px solid var(--raised)}
.rrow:last-child{border:none}
.rn{flex:1;color:var(--ts)}
.rv{font-family:var(--mono);font-weight:600;color:var(--tp)}
.ru{font-size:10px;color:var(--td);margin:0 5px 0 2px}
.nc-act{padding:10px 16px 14px;display:flex;gap:8px;border-top:1px solid var(--bdr);background:var(--raised)}
.btn-sm{padding:5px 13px;border-radius:6px;font-size:11px;font-weight:500;cursor:pointer;border:1px solid var(--bdr);background:var(--sur);color:var(--ts);transition:all .15s}
.btn-sm:hover{border-color:var(--acc);color:var(--acc)}
.btn-pri{background:var(--acc);color:#fff;border-color:var(--acc)}
.btn-pri:hover{background:#1d4ed8;border-color:#1d4ed8;color:#fff}
/* ── Empty state ── */
.empty{text-align:center;padding:64px 24px;color:var(--td);grid-column:1/-1}
.empty .ico{font-size:40px;margin-bottom:12px;opacity:.5}
.empty .lbl{font-size:14px;font-weight:600;color:var(--ts);margin-bottom:6px}
.empty .sub{font-size:12px;line-height:1.6}
/* ── Alerts table ── */
.atbl{width:100%;border-collapse:collapse}
.atbl th{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;color:var(--ts);text-align:left;padding:10px 16px;border-bottom:2px solid var(--bdr);background:var(--raised)}
.atbl td{padding:11px 16px;border-bottom:1px solid var(--bdr);font-size:13px;vertical-align:middle}
.atbl tbody tr:hover td{background:var(--raised)}
.atbl tbody tr:last-child td{border:none}
/* ── Configuration card ── */
.cfg-card{background:var(--sur);border:1px solid var(--bdr);border-radius:var(--radius);overflow:hidden}
.cfg-tabbar{display:flex;background:var(--raised);border-bottom:1px solid var(--bdr);padding:0 4px;overflow-x:auto}
.ctab{padding:13px 18px;font-size:13px;font-weight:500;cursor:pointer;color:var(--ts);border-bottom:2px solid transparent;transition:color .15s,border-color .15s;user-select:none;white-space:nowrap;margin-bottom:-1px}
.ctab:hover{color:var(--tp)}
.ctab.active{color:var(--acc);border-bottom-color:var(--acc);font-weight:600}
.cfg-body{padding:24px}
.cpanel{display:none}.cpanel.on{display:block}
.cfg-footer{padding:16px 24px;background:var(--raised);border-top:1px solid var(--bdr);display:flex;gap:10px;align-items:center}
/* Form elements */
.sec-title{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.06em;color:var(--ts);margin:20px 0 12px;padding-bottom:8px;border-bottom:1px solid var(--bdr)}
.sec-title:first-child{margin-top:0}
.fgrid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.ff{display:flex;flex-direction:column;gap:5px}
.ff.s2{grid-column:span 2}
label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.03em;color:var(--ts)}
input[type=text],input[type=password],input[type=number],input:not([type]){padding:8px 11px;border:1px solid var(--bdr);border-radius:7px;font-size:13px;font-family:var(--sans);background:var(--sur);color:var(--tp);width:100%;transition:border .15s,box-shadow .15s}
input:focus{outline:none;border-color:var(--acc);box-shadow:0 0 0 3px rgba(37,99,235,.1)}
.hint{font-size:11px;color:var(--td);line-height:1.4}
.frow{display:flex;align-items:center;gap:9px;padding:2px 0}
.frow input[type=checkbox]{width:16px;height:16px;flex-shrink:0;accent-color:var(--acc);cursor:pointer}
.frow label{text-transform:none;font-size:13px;font-weight:400;cursor:pointer;letter-spacing:0}
/* Buttons */
.btn{padding:9px 22px;border-radius:7px;font-size:13px;font-weight:500;cursor:pointer;border:none;transition:all .15s;display:inline-flex;align-items:center;gap:6px}
.btn-acc{background:var(--acc);color:#fff}.btn-acc:hover{background:#1d4ed8;box-shadow:0 2px 8px rgba(37,99,235,.3)}
.btn-out{background:none;border:1px solid var(--bdr);color:var(--ts)}.btn-out:hover{border-color:var(--bdrhi);color:var(--tp)}
.btn-dng{background:var(--red);color:#fff}.btn-dng:hover{background:#b91c1c}
/* Radar */
.radar-wrap{position:fixed;top:52px;left:220px;right:0;bottom:0;pointer-events:none;z-index:1;display:flex;align-items:center;justify-content:center;opacity:.06}
.radar{width:600px;height:600px;border:1px solid var(--acc);border-radius:50%;position:relative}
.radar::before{content:'';position:absolute;inset:0;border-radius:50%;background:conic-gradient(from 0deg,var(--acc) 0%,transparent 20%);animation:scan 4s linear infinite}
.rring{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);border:1px solid var(--acc);border-radius:50%}
@keyframes scan{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
/* Login overlay */
.overlay{position:fixed;inset:0;background:rgba(15,23,42,.6);backdrop-filter:blur(5px);z-index:500;display:none;align-items:center;justify-content:center}
.lbox{background:var(--sur);border:1px solid var(--bdr);border-radius:14px;padding:32px;width:340px;box-shadow:0 24px 48px rgba(0,0,0,.18)}
.lbox-logo{width:44px;height:44px;background:var(--acc);border-radius:11px;display:flex;align-items:center;justify-content:center;color:#fff;font-size:20px;font-weight:700;font-family:var(--mono);margin-bottom:16px}
.lt{font-size:17px;font-weight:600;margin-bottom:3px}
.ls{font-size:12px;color:var(--ts);margin-bottom:20px}
.lerr{font-size:12px;color:var(--red);margin-top:8px;padding:8px 10px;background:var(--red-dim);border-radius:6px;display:none}
/* Toast */
#toast{position:fixed;bottom:24px;right:24px;z-index:600;display:flex;flex-direction:column;gap:8px}
.ti{padding:10px 16px;border-radius:8px;font-size:13px;font-weight:500;background:#1e293b;color:#f1f5f9;box-shadow:0 4px 16px rgba(0,0,0,.25);animation:fi .2s ease}
.ti.ok{background:var(--grn)}.ti.er{background:var(--red)}
@keyframes fi{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
</style>
</head>
<body>
<!-- Login overlay -->
<div class="overlay" id="lov">
  <div class="lbox">
    <div class="lbox-logo">C</div>
    <div class="lt">Sign In</div>
    <div class="ls">Enter your credentials to access configuration</div>
    <div class="ff"><label>Username</label><input id="lu" autocomplete="username" style="margin-top:2px"></div>
    <div class="ff" style="margin-top:12px"><label>Password</label><input id="lp" type="password" autocomplete="current-password" onkeydown="if(event.key==='Enter')doLogin()" style="margin-top:2px"></div>
    <div class="lerr" id="lerr">Incorrect username or password</div>
    <button class="btn btn-acc" style="width:100%;justify-content:center;margin-top:18px;padding:11px" onclick="doLogin()">Sign In</button>
  </div>
</div>
<div id="toast"></div>
<!-- Sidebar -->
<div class="sb">
  <div class="sb-brand">
    <div class="sb-logo">C</div>
    <div>
      <div class="t1">ISMS Central</div>
      <div class="t2" id="sb-sub">Central Node</div>
    </div>
  </div>
  <div class="sb-nav">
    <div class="sb-sect">Monitor</div>
    <div class="ni active" onclick="nav('dashboard',this)">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/></svg>
      Dashboard
    </div>
    <div class="ni" onclick="nav('alerts',this)">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
      Alerts
      <span class="ni-badge" id="abadge"></span>
    </div>
    <div class="sb-sect">System</div>
    <div class="ni" onclick="nav('config',this)">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.07 4.93a10 10 0 010 14.14M4.93 4.93a10 10 0 000 14.14"/></svg>
      Configuration
    </div>
  </div>
  <div class="sb-foot">
    <div class="sync-row">
      <div class="sdot" id="sdot"></div>
      <span id="stxt">DB sync off</span>
    </div>
    <div class="sub" id="stim"></div>
  </div>
</div>
<!-- Main -->
<div class="main">
  <div class="topbar">
    <div class="tb-title" id="ptitle">Dashboard</div>
    <button class="tb-btn" onclick="addNode()">
      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
      Add Node
    </button>
    <button class="tb-btn tb-logout" id="lbtn" style="display:none" onclick="doLogout()">Logout</button>
  </div>
  <div class="content">

    <!-- ── Dashboard ── -->
    <div id="tab-dashboard">
      <div class="radar-wrap" id="radar">
        <div class="radar">
          <div class="rring" style="width:200px;height:200px"></div>
          <div class="rring" style="width:400px;height:400px"></div>
        </div>
      </div>
      <div class="stats">
        <div class="sc">
          <div class="sc-icon total">&#127760;</div>
          <div class="sc-info"><div class="sc-lbl">Total Nodes</div><div class="sc-val n" id="s-tot">—</div></div>
        </div>
        <div class="sc">
          <div class="sc-icon online">&#9679;</div>
          <div class="sc-info"><div class="sc-lbl">Online</div><div class="sc-val g" id="s-on">—</div></div>
        </div>
        <div class="sc">
          <div class="sc-icon alerts">&#9888;</div>
          <div class="sc-info"><div class="sc-lbl">Active Alerts</div><div class="sc-val" id="s-al">—</div></div>
        </div>
      </div>
      <div class="ngrid" id="ngrid"></div>
    </div>

    <!-- ── Alerts ── -->
    <div id="tab-alerts" style="display:none">
      <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:16px">
        <div style="font-size:15px;font-weight:600">Active Alerts</div>
        <div style="font-size:12px;color:var(--td);font-family:var(--mono)" id="actxt"></div>
      </div>
      <div style="background:var(--sur);border:1px solid var(--bdr);border-radius:var(--radius);overflow:hidden">
        <div class="empty" id="al-empty" style="grid-column:auto">
          <div class="ico">&#10003;</div>
          <div class="lbl">All clear</div>
          <div class="sub">No active alerts across any sensor node</div>
        </div>
        <table class="atbl" id="al-tbl" style="display:none">
          <thead><tr><th>Node</th><th>Sensor</th><th>Type</th><th>State</th><th>Value</th></tr></thead>
          <tbody id="al-body"></tbody>
        </table>
      </div>
    </div>

    <!-- ── Configuration ── -->
    <div id="tab-config" style="display:none">
      <div style="margin-bottom:16px">
        <div style="font-size:15px;font-weight:600">Configuration</div>
        <div style="font-size:12px;color:var(--ts);margin-top:3px">Changes take effect after reboot</div>
      </div>
      <div class="cfg-card">
        <!-- Tab bar -->
        <div class="cfg-tabbar">
          <div class="ctab active" onclick="ctab('net',this)">Network</div>
          <div class="ctab" onclick="ctab('loc',this)">Location</div>
          <div class="ctab" onclick="ctab('db',this)">Database</div>
          <div class="ctab" onclick="ctab('sec',this)">Security</div>
        </div>
        <!-- Tab content -->
        <div class="cfg-body">

          <!-- Network -->
          <div class="cpanel on" id="cp-net">
            <div class="sec-title">Device Identity</div>
            <div class="fgrid">
              <div class="ff"><label>Hostname</label><input id="f-host"><div class="hint">Used in discovery responses and DB records</div></div>
              <div class="ff"><label>NTP Server</label><input id="f-ntp" placeholder="pool.ntp.org"><div class="hint">Time server for accurate DB timestamps</div></div>
            </div>
            <div class="sec-title">WiFi Connection</div>
            <div class="fgrid">
              <div class="ff"><label>WiFi SSID</label><input id="f-ssid"><div class="hint">Leave blank to run in standalone AP mode</div></div>
              <div class="ff"><label>WiFi Password</label><input type="password" id="f-wp" placeholder="(leave blank to keep current)"></div>
              <div class="ff"><label>Hotspot Password</label><input type="password" id="f-hp" placeholder="(leave blank to keep current)"><div class="hint">Used when in standalone AP mode</div></div>
              <div class="ff" style="justify-content:flex-end;padding-bottom:2px">
                <div class="frow" style="margin-top:auto">
                  <input type="checkbox" id="f-sa">
                  <label for="f-sa">Standalone / AP mode</label>
                </div>
                <div class="hint" style="padding-left:25px">Sensor nodes connect to this device as access point</div>
              </div>
            </div>
          </div>

          <!-- Location -->
          <div class="cpanel" id="cp-loc">
            <div class="sec-title">Site Information</div>
            <div class="fgrid">
              <div class="ff"><label>Factory / Site Name</label><input id="f-fac" placeholder="e.g. Main Factory"></div>
              <div class="ff"><label>Building</label><input id="f-bld" placeholder="e.g. Building A"></div>
              <div class="ff s2"><label>Room / Section</label><input id="f-room" placeholder="e.g. Production Floor"></div>
            </div>
            <div style="margin-top:14px;padding:12px 14px;background:var(--acc-dim);border:1px solid rgba(37,99,235,.15);border-radius:7px;font-size:12px;color:var(--ts)">
              Location metadata is embedded in every record synced to the database, enabling multi-site filtering and reporting.
            </div>
          </div>

          <!-- Database -->
          <div class="cpanel" id="cp-db">
            <div class="sec-title">ISMS Web Portal Sync</div>
            <div class="ff s2" style="margin-bottom:14px">
              <label>Sync URL</label>
              <input id="f-murl" placeholder="https://your-domain.com/api/sync">
              <div class="hint">Web portal sync endpoint — register this node in the portal (Devices page) to get its URL and API key</div>
            </div>
            <div class="fgrid">
              <div class="ff"><label>API Key</label><input type="password" id="f-mkey" placeholder="(leave blank to keep current)"></div>
              <div class="ff"><label>Database Name</label><input id="f-mdb" placeholder="isms_db"></div>
              <div class="ff"><label>Collection Name</label><input id="f-mcol" placeholder="sensor_readings"></div>
              <div class="ff"><label>Sync Interval (seconds)</label><input type="number" id="f-sint" min="5" max="3600" placeholder="30"></div>
            </div>
            <div class="sec-title">Sync Control</div>
            <div class="frow">
              <input type="checkbox" id="f-son">
              <label for="f-son">Enable automatic synchronisation</label>
            </div>
            <div class="hint" style="padding-left:25px;margin-top:4px">When enabled, all node readings are pushed to the web portal at the configured interval</div>
          </div>

          <!-- Security -->
          <div class="cpanel" id="cp-sec">
            <div class="sec-title">Web Login</div>
            <div class="fgrid">
              <div class="ff"><label>Username</label><input id="f-wu"></div>
              <div class="ff"><label>New Password</label><input type="password" id="f-wpass" placeholder="(leave blank to keep current)"><div class="hint">Minimum 4 characters</div></div>
            </div>
            <div class="sec-title">API Key</div>
            <div class="ff"><label>X-API-Key Header Value</label><input id="f-api"><div class="hint">Sent in the X-API-Key header to authenticate external API calls to protected endpoints</div></div>
          </div>

        </div><!-- /cfg-body -->
        <!-- Footer with save actions -->
        <div class="cfg-footer">
          <button class="btn btn-acc" onclick="saveCfg()">
            <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="20 6 9 17 4 12"/></svg>
            Save &amp; Reboot
          </button>
          <button class="btn btn-out" onclick="doReboot()">Reboot Now</button>
          <div style="flex:1"></div>
          <div style="font-size:11px;color:var(--td)">All changes require a reboot to take effect</div>
        </div>
      </div>
    </div>

  </div>
</div>
<script>
let tok = sessionStorage.getItem('cn_tok') || '';
let curTab = 'dashboard';
let cfgDone = false;

function toast(msg, type) {
  const el = document.createElement('div');
  el.className = 'ti' + (type==='ok'?' ok':type==='er'?' er':'');
  el.textContent = msg;
  document.getElementById('toast').appendChild(el);
  setTimeout(()=>el.remove(), 3200);
}

function aClass(s) {
  return s==='high'?'b-hi':s==='low'?'b-lo':s==='gas'?'b-gas':s==='active'?'b-act':'b-on';
}

function nav(tab, el) {
  if (tab==='config' && !tok) { showLogin(); return; }
  document.querySelectorAll('.ni').forEach(n=>n.classList.remove('active'));
  if (el) el.classList.add('active');
  document.querySelectorAll('[id^="tab-"]').forEach(t=>t.style.display='none');
  document.getElementById('tab-'+tab).style.display='block';
  document.getElementById('radar').style.display=(tab==='dashboard')?'flex':'none';
  document.getElementById('ptitle').textContent=tab.charAt(0).toUpperCase()+tab.slice(1);
  document.getElementById('lbtn').style.display=tok?'block':'none';
  curTab = tab;
  if (tab==='config' && !cfgDone) fetchCfg();
}

function ctab(name, el) {
  document.querySelectorAll('.ctab').forEach(t=>t.classList.remove('active'));
  document.querySelectorAll('.cpanel').forEach(p=>p.classList.remove('on'));
  el.classList.add('active');
  document.getElementById('cp-'+name).classList.add('on');
}

function showLogin() {
  document.getElementById('lov').style.display='flex';
  document.getElementById('lerr').style.display='none';
  document.getElementById('lu').value='';
  document.getElementById('lp').value='';
  setTimeout(()=>document.getElementById('lu').focus(),50);
}

async function doLogin() {
  const res = await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:document.getElementById('lu').value,password:document.getElementById('lp').value})});
  if (res.ok) {
    const d = await res.json();
    tok = d.token;
    sessionStorage.setItem('cn_tok', tok);
    document.getElementById('lov').style.display='none';
    document.getElementById('lbtn').style.display='block';
    document.querySelectorAll('.ni').forEach(n=>n.classList.remove('active'));
    document.querySelectorAll('[id^="tab-"]').forEach(t=>t.style.display='none');
    document.querySelectorAll('.ni')[2].classList.add('active');
    document.getElementById('tab-config').style.display='block';
    document.getElementById('radar').style.display='none';
    document.getElementById('ptitle').textContent='Configuration';
    curTab='config';
    if (!cfgDone) fetchCfg();
  } else {
    document.getElementById('lerr').style.display='block';
  }
}

async function doLogout() {
  await fetch('/api/logout',{method:'POST',headers:{'X-Session-Token':tok}});
  tok=''; sessionStorage.removeItem('cn_tok');
  document.getElementById('lbtn').style.display='none';
  document.querySelectorAll('.ni').forEach(n=>n.classList.remove('active'));
  document.querySelectorAll('.ni')[0].classList.add('active');
  document.querySelectorAll('[id^="tab-"]').forEach(t=>t.style.display='none');
  document.getElementById('tab-dashboard').style.display='block';
  document.getElementById('radar').style.display='flex';
  document.getElementById('ptitle').textContent='Dashboard';
  curTab='dashboard';
}

function renderNodes(data) {
  let totalAlerts = 0;
  const online = data.filter(n=>n.online).length;
  data.forEach(n=>{totalAlerts+=n.alerts||0;});
  document.getElementById('s-tot').textContent=data.length;
  document.getElementById('s-on').textContent=online;
  const ae = document.getElementById('s-al');
  ae.textContent=totalAlerts;
  ae.className='sc-val '+(totalAlerts>0?'r':'g');
  const b=document.getElementById('abadge');
  b.style.display=totalAlerts>0?'inline':'none';
  b.textContent=totalAlerts;
  document.getElementById('sb-sub').textContent=data.length+' node'+(data.length!==1?'s':'')+' tracked';

  const grid=document.getElementById('ngrid');
  if (data.length===0) {
    grid.innerHTML='<div class="empty"><div class="ico">&#9711;</div><div class="lbl">No sensor nodes discovered</div><div class="sub">Waiting for nodes to respond to the discovery broadcast.<br>Use <strong>+ Add Node</strong> to register a node manually by IP address.</div></div>';
    return;
  }
  grid.innerHTML=data.map(n=>{
    const hasAlert=(n.alerts||0)>0;
    const readings=(n.readings||[]).filter(r=>r.type!=='relay');
    const s=n.secsAgo||0;
    const agoStr=s<5?'just now':s<60?s+'s ago':s<3600?Math.floor(s/60)+'m ago':Math.floor(s/3600)+'h ago';
    const barClass=!n.online?'offline':hasAlert?'alert':'ok-node';
    return `<div class="nc ${barClass}">
<div class="nc-bar"></div>
<div class="nc-body">
<div class="nc-head">
  <div class="nc-title"><div class="nc-name">${n.hostname}</div><div class="nc-ip">${n.ip}</div></div>
  <div class="nc-right">
    <span class="badge ${n.online?'b-on':'b-off'}">${n.online?'ONLINE':'OFFLINE'}</span>
    <span class="nc-ts">${agoStr}</span>
  </div>
</div>
<div>${readings.length===0?'<div style="font-size:12px;color:var(--td);padding:8px 0;text-align:center">No sensor readings yet</div>':
  readings.map(r=>`<div class="rrow">
  <span class="rn">${r.name||r.sensorId}</span>
  <span class="rv">${typeof r.value==='number'?r.value.toFixed(2):r.value}<span class="ru">${r.unit||''}</span></span>
  ${r.alertState&&r.alertState!=='ok'?`<span class="badge ${aClass(r.alertState)}">${r.alertState.toUpperCase()}</span>`:''}
</div>`).join('')}
</div>
</div>
<div class="nc-act">
  <button class="btn-sm btn-pri" onclick="window.open('http://${n.ip}')">Open Node</button>
  <button class="btn-sm" onclick="refreshNode('${n.ip}')">Refresh</button>
</div>
</div>`;
  }).join('');
}

function renderAlerts(alerts) {
  document.getElementById('actxt').textContent=alerts.length+' alert'+(alerts.length!==1?'s':'');
  const empty=document.getElementById('al-empty');
  const tbl=document.getElementById('al-tbl');
  const body=document.getElementById('al-body');
  if (alerts.length===0){empty.style.display='block';tbl.style.display='none';return;}
  empty.style.display='none';tbl.style.display='table';
  body.innerHTML=alerts.map(a=>`<tr>
  <td><strong>${a.nodeHostname}</strong></td>
  <td>${a.sensorName}</td>
  <td style="font-family:var(--mono);font-size:11px;color:var(--ts)">${a.sensorType}</td>
  <td><span class="badge ${aClass(a.alertState)}">${a.alertState.toUpperCase()}</span></td>
  <td style="font-family:var(--mono)">${typeof a.value==='number'?a.value.toFixed(2):a.value}<span style="font-size:10px;color:var(--td);margin-left:4px">${a.unit||''}</span></td>
</tr>`).join('');
}

async function refresh() {
  try {
    const r=await fetch('/api/nodes');
    const data=await r.json();
    renderNodes(data);
    const alerts=[];
    data.forEach(n=>{
      (n.readings||[]).forEach(r=>{
        if(r.alertState&&r.alertState!=='ok')
          alerts.push({nodeHostname:n.hostname,sensorName:r.name||r.sensorId,sensorType:r.type,alertState:r.alertState,value:r.value,unit:r.unit});
      });
    });
    renderAlerts(alerts);
  } catch(e){}
  try {
    const sr=await fetch('/api/sync_status');
    const sd=await sr.json();
    const dot=document.getElementById('sdot');
    const txt=document.getElementById('stxt');
    const tim=document.getElementById('stim');
    if(!sd.enabled){dot.className='sdot';txt.textContent='DB sync off';tim.textContent='';}
    else if(sd.ok){dot.className='sdot ok';txt.textContent='DB sync OK';tim.textContent=sd.lastSync?'Last: '+new Date(sd.lastSync*1000).toLocaleTimeString():'';}
    else{dot.className='sdot fail';txt.textContent='DB sync failed';tim.textContent=sd.lastSync?new Date(sd.lastSync*1000).toLocaleTimeString():'';}
  } catch(e){}
}

async function refreshNode(ip) {
  try { await fetch('/api/poll_node',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ip})}); } catch(e){}
  setTimeout(refresh,600);
}

async function addNode() {
  const ip=prompt('Enter sensor node IP address:');
  if(!ip||!ip.trim()) return;
  const res=await fetch('/api/add_node',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ip:ip.trim()})});
  if(res.ok){toast('Node added','ok');setTimeout(refresh,800);}
  else{toast('Could not reach '+ip.trim(),'er');}
}

async function fetchCfg() {
  const res=await fetch('/api/config',{headers:{'X-Session-Token':tok}});
  if(res.status===401){showLogin();return;}
  const c=await res.json();
  document.getElementById('f-host').value=c.hostname||'';
  document.getElementById('f-ssid').value=c.ssid||'';
  document.getElementById('f-sa').checked=c.standalone||false;
  document.getElementById('f-ntp').value=c.ntpServer||'pool.ntp.org';
  document.getElementById('f-fac').value=c.factory||'';
  document.getElementById('f-bld').value=c.building||'';
  document.getElementById('f-room').value=c.room||'';
  document.getElementById('f-murl').value=c.mongoUrl||'';
  document.getElementById('f-mdb').value=c.mongoDatabase||'';
  document.getElementById('f-mcol').value=c.mongoCollection||'';
  document.getElementById('f-sint').value=c.syncInterval||30;
  document.getElementById('f-son').checked=c.syncEnabled||false;
  document.getElementById('f-api').value=c.apiKey||'';
  document.getElementById('f-wu').value=c.webUsername||'';
  cfgDone=true;
}

async function saveCfg() {
  const body={
    hostname:document.getElementById('f-host').value,
    ssid:document.getElementById('f-ssid').value,
    pass:document.getElementById('f-wp').value,
    hotspotPassword:document.getElementById('f-hp').value,
    standalone:document.getElementById('f-sa').checked,
    ntpServer:document.getElementById('f-ntp').value,
    factory:document.getElementById('f-fac').value,
    building:document.getElementById('f-bld').value,
    room:document.getElementById('f-room').value,
    mongoUrl:document.getElementById('f-murl').value,
    mongoApiKey:document.getElementById('f-mkey').value,
    mongoDatabase:document.getElementById('f-mdb').value,
    mongoCollection:document.getElementById('f-mcol').value,
    syncInterval:parseInt(document.getElementById('f-sint').value)||30,
    syncEnabled:document.getElementById('f-son').checked,
    apiKey:document.getElementById('f-api').value,
    webUsername:document.getElementById('f-wu').value,
    webPassword:document.getElementById('f-wpass').value,
  };
  const res=await fetch('/api/save_config',{method:'POST',headers:{'Content-Type':'application/json','X-Session-Token':tok},body:JSON.stringify(body)});
  if(res.status===401){toast('Session expired','er');showLogin();return;}
  toast('Saved — rebooting...','ok');
  setTimeout(()=>fetch('/api/reboot',{method:'POST',headers:{'X-Session-Token':tok}}),800);
}

async function doReboot() {
  if(!confirm('Reboot the central node now?')) return;
  await fetch('/api/reboot',{method:'POST',headers:{'X-Session-Token':tok}});
  toast('Rebooting...','');
}

// Verify stored token on load
if(tok){
  fetch('/api/config',{headers:{'X-Session-Token':tok}}).then(r=>{
    if(r.status===401){tok='';sessionStorage.removeItem('cn_tok');}
    else document.getElementById('lbtn').style.display='block';
  });
}

setInterval(refresh, 3000);
refresh();
</script>
</body>
</html>
)rawliteral";

// ── API handlers ──────────────────────────────────────────────────────────────

static void handleRoot()
{
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleAPINodes()
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    unsigned long now = millis();

    for (const auto& node : discoveredNodes) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"]       = node.id;
        obj["hostname"] = node.hostname;
        obj["ip"]       = node.ip;
        obj["online"]   = node.online;
        obj["secsAgo"]  = (uint32_t)((now - node.lastSeen) / 1000UL);

        int alertCount = 0;
        JsonArray readings = obj["readings"].to<JsonArray>();
        for (const auto& r : node.lastReadings) {
            JsonObject rObj = readings.add<JsonObject>();
            rObj["sensorId"]   = r.sensorId;
            rObj["name"]       = r.name;
            rObj["type"]       = r.type;
            rObj["value"]      = r.value;
            rObj["unit"]       = r.unit;
            rObj["alertState"] = r.alertState;
            if (r.alertState != "ok" && r.type != "relay") alertCount++;
        }
        obj["alerts"] = alertCount;
    }

    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleAPISyncStatus()
{
    JsonDocument doc;
    doc["enabled"]  = systemConfig.syncEnabled;
    doc["ok"]       = g_syncLastOk;
    doc["lastSync"] = g_syncLastTime;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleAPIConfig()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }

    JsonDocument doc;
    doc["hostname"]        = systemConfig.hostname;
    doc["ssid"]            = systemConfig.wifiSSID;
    doc["standalone"]      = systemConfig.standalone;
    doc["ntpServer"]       = systemConfig.ntpServer;
    doc["factory"]         = systemConfig.factory;
    doc["building"]        = systemConfig.building;
    doc["room"]            = systemConfig.room;
    doc["apiKey"]          = systemConfig.apiKey;
    doc["webUsername"]     = systemConfig.webUsername;
    doc["mongoUrl"]        = systemConfig.mongoUrl;
    doc["mongoDatabase"]   = systemConfig.mongoDatabase;
    doc["mongoCollection"] = systemConfig.mongoCollection;
    doc["syncInterval"]    = systemConfig.syncInterval;
    doc["syncEnabled"]     = systemConfig.syncEnabled;
    doc["hotspotPassword"] = systemConfig.hotspotPassword;

    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleSaveConfig()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"No body\"}"); return; }
    if (!checkPayload(2048)) return;

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Bad JSON\"}"); return;
    }

    if (doc["hostname"].is<String>())     systemConfig.hostname    = doc["hostname"].as<String>();
    if (doc["ssid"].is<String>())         systemConfig.wifiSSID    = doc["ssid"].as<String>();
    if (doc["pass"].is<String>() && !doc["pass"].as<String>().isEmpty())
        systemConfig.wifiPassword = doc["pass"].as<String>();
    if (doc["hotspotPassword"].is<String>()) systemConfig.hotspotPassword = doc["hotspotPassword"].as<String>();
    if (doc["standalone"].is<bool>())     systemConfig.standalone  = doc["standalone"].as<bool>();
    if (doc["ntpServer"].is<String>())    systemConfig.ntpServer   = doc["ntpServer"].as<String>();
    if (doc["factory"].is<String>())      systemConfig.factory     = doc["factory"].as<String>();
    if (doc["building"].is<String>())     systemConfig.building    = doc["building"].as<String>();
    if (doc["room"].is<String>())         systemConfig.room        = doc["room"].as<String>();
    if (doc["apiKey"].is<String>())       systemConfig.apiKey      = doc["apiKey"].as<String>();
    if (doc["webUsername"].is<String>())  systemConfig.webUsername = doc["webUsername"].as<String>();
    if (doc["webPassword"].is<String>() && !doc["webPassword"].as<String>().isEmpty())
        systemConfig.webPassword = doc["webPassword"].as<String>();

    systemConfig.mongoUrl        = doc["mongoUrl"]        | "";
    systemConfig.mongoApiKey     = doc["mongoApiKey"].is<String>() && !doc["mongoApiKey"].as<String>().isEmpty()
                                    ? doc["mongoApiKey"].as<String>() : systemConfig.mongoApiKey;
    systemConfig.mongoDatabase   = doc["mongoDatabase"]   | "isms_db";
    systemConfig.mongoCollection = doc["mongoCollection"] | "sensor_readings";
    systemConfig.syncInterval    = doc["syncInterval"]    | 30;
    systemConfig.syncEnabled     = doc["syncEnabled"]     | false;

    saveConfig();
    // Invalidate session so UI re-login is required after reboot
    s_sessionToken = "";
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handleLogin()
{
    if (!server.hasArg("plain")) { server.send(400); return; }
    if (!checkPayload(256)) return;
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    String user = doc["username"] | "";
    String pass = doc["password"] | "";

    if (user == systemConfig.webUsername && pass == systemConfig.webPassword) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%08" PRIx32 "%08" PRIx32,
                (uint32_t)(ESP.getEfuseMac() >> 32),
                (uint32_t)millis());
        s_sessionToken  = String(buf);
        s_sessionExpiry = millis() + SESSION_MS;  // S4: start expiry window
        String resp = "{\"token\":\"" + s_sessionToken + "\"}";
        server.send(200, "application/json", resp);
    } else {
        server.send(401, "application/json", "{\"error\":\"Invalid credentials\"}");
    }
}

static void handleLogout()
{
    s_sessionToken = "";
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handleReboot()
{
    if (!isAuthorized()) { sendUnauthorized(); return; }
    server.send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(300);
    ESP.restart();
}

// Trigger a UI refresh — responds immediately; normal poll cycle updates data.
static void handlePollNode()
{
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handleAddNode()
{
    if (!server.hasArg("plain")) { server.send(400); return; }
    if (!checkPayload(256)) return;
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    String ip = doc["ip"] | "";
    if (ip.isEmpty()) { server.send(400, "application/json", "{\"error\":\"Missing ip\"}"); return; }

    if (addNodeByIP(ip)) {
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
        server.send(404, "application/json",
                    "{\"error\":\"Node not reachable — check IP and that both devices are on the same network\"}");
    }
}

static void handleFavicon() { server.send(204); }

// ── Server start ──────────────────────────────────────────────────────────────

void startWebServer()
{
    static const char* hdrs[] = { "X-Session-Token", "X-API-Key" };
    server.collectHeaders(hdrs, 2);

    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/api/nodes",       HTTP_GET,  handleAPINodes);
    server.on("/api/sync_status", HTTP_GET,  handleAPISyncStatus);
    server.on("/api/config",      HTTP_GET,  handleAPIConfig);
    server.on("/api/save_config", HTTP_POST, handleSaveConfig);
    server.on("/api/login",       HTTP_POST, handleLogin);
    server.on("/api/logout",      HTTP_POST, handleLogout);
    server.on("/api/reboot",      HTTP_POST, handleReboot);
    server.on("/api/poll_node",   HTTP_POST, handlePollNode);
    server.on("/api/add_node",    HTTP_POST, handleAddNode);
    server.on("/favicon.ico",               handleFavicon);

    server.begin();
    Serial.println("[WEB] Central node server started on :80");
}
