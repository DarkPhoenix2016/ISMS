// ─────────────────────────────────────────────────────────────────────────────
// Socket.io — real-time events
// ─────────────────────────────────────────────────────────────────────────────
let _socket = null;

function initSocket() {
  _socket = io();
  const dot = document.getElementById('ws-dot');

  _socket.on('connect', () => {
    if (dot) { dot.className = 'ws-dot connected'; dot.title = 'Real-time: connected'; }
    console.log('[WS] Connected:', _socket.id);
  });

  _socket.on('disconnect', () => {
    if (dot) { dot.className = 'ws-dot error'; dot.title = 'Real-time: disconnected'; }
  });

  // New alert from a sensor node
  _socket.on('alert:new', alert => {
    toast(`⚠ ${alert.sensorName} (${alert.nodeHostname}): ${(alert.alertState||'').toUpperCase()}  ${typeof alert.value === 'number' ? alert.value.toFixed(2) : ''}`, 'err');
    // Pulse the nav badge
    const badge = document.getElementById('nav-alert-count');
    if (badge) {
      badge.style.transition = 'none';
      badge.style.transform = 'scale(1.4)';
      setTimeout(() => { badge.style.transition = 'transform .2s'; badge.style.transform = 'scale(1)'; }, 150);
    }
    if (activePage === 'dashboard') loadDashboard();
  });

  // Alert resolved / cleared
  _socket.on('alert:resolved', () => {
    if (activePage === 'dashboard') loadDashboard();
  });

  // New data synced from a center node
  _socket.on('sync:new', ({ centerId }) => {
    if (activePage === 'dashboard') loadDashboard();
    if (activePage === 'topology') loadTopology();
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Dark mode
// ─────────────────────────────────────────────────────────────────────────────
function initTheme() {
  const saved = localStorage.getItem('isms_theme') || 'light';
  applyTheme(saved);
}

function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  const btn = document.getElementById('theme-btn');
  if (btn) {
    btn.innerHTML = `<i data-lucide="${theme === 'dark' ? 'sun' : 'moon'}"></i>`;
    lucide.createIcons();
  }
  localStorage.setItem('isms_theme', theme);
}

function toggleTheme() {
  const current = document.documentElement.getAttribute('data-theme') || 'light';
  applyTheme(current === 'dark' ? 'light' : 'dark');
}

// ─────────────────────────────────────────────────────────────────────────────
// App state
// ─────────────────────────────────────────────────────────────────────────────
let currentUser = null;
let db = null;          // MongoDB API helper object
let activeAlert = null; // currently being acknowledged
let activeIncId = null; // currently editing incident
let charts = {};
let activePage = 'dashboard';
let pageState = {
  alerts: { page: 0, limit: 20 },
  readings: { page: 0, limit: 25 },
  nodes: { page: 0, limit: 20 },
  incidents: { page: 0, limit: 20 }
};

// ─────────────────────────────────────────────────────────────────────────────
// Toast
// ─────────────────────────────────────────────────────────────────────────────
function toast(msg, type = 'info') {
  const el = document.createElement('div');
  el.className = 'toast toast-' + type;
  const icon = type === 'ok' ? 'check-circle' : type === 'err' ? 'x-circle' : type === 'warn' ? 'alert-triangle' : 'info';
  el.innerHTML = `<i data-lucide="${icon}" style="width:16px;height:16px"></i> ` + msg;
  document.getElementById('toast-container').appendChild(el);
  lucide.createIcons();
  setTimeout(() => el.remove(), 4000);
}

// ─────────────────────────────────────────────────────────────────────────────
// MongoDB — local server proxy (/api/db/:action)
// ─────────────────────────────────────────────────────────────────────────────
function getMongoConfig() {
  return {
    db: localStorage.getItem('isms_mongo_db') || 'isms_db',
    cols: {
      readings:  localStorage.getItem('isms_col_readings')  || 'sensor_readings',
      acks:      localStorage.getItem('isms_col_acks')      || 'alert_acks',
      nodes:     localStorage.getItem('isms_col_nodes')     || 'node_registry',
      incidents: localStorage.getItem('isms_col_incidents') || 'incidents'
    }
  };
}

async function mongoRequest(action, collection, params = {}) {
  const cfg = getMongoConfig();
  const res = await fetch(`/api/db/${action}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ database: cfg.db, collection, ...params })
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(`DB ${action} failed (${res.status}): ${err.substring(0, 120)}`);
  }
  return res.json();
}

const mongo = {
  find:      (col, filter = {}, opts = {}) => mongoRequest('find',      col, { filter, ...opts }),
  findOne:   (col, filter = {})            => mongoRequest('findOne',   col, { filter }),
  insertOne: (col, document)               => mongoRequest('insertOne', col, { document }),
  updateOne: (col, filter, update)         => mongoRequest('updateOne', col, { filter, update }),
  deleteOne: (col, filter)                 => mongoRequest('deleteOne', col, { filter }),
  aggregate: (col, pipeline)               => mongoRequest('aggregate', col, { pipeline })
};

function col(name) { return getMongoConfig().cols[name] || name; }

// ─────────────────────────────────────────────────────────────────────────────
// Firebase Auth
// ─────────────────────────────────────────────────────────────────────────────
async function initFirebase() {
  let fbConfig = null;
  try {
    const r = await fetch('/api/firebase-config');
    if (r.ok) fbConfig = await r.json();
  } catch(e) {}
  if (!fbConfig || !fbConfig.apiKey) {
    // No Firebase config — dev/offline mode, skip auth
    showApp({ email: 'dev@localhost', displayName: 'Developer' });
    return;
  }
  firebase.initializeApp(fbConfig);
  firebase.auth().onAuthStateChanged(user => {
    if (user) { currentUser = user; showApp(user); }
    else      { showLogin(); }
  });
}

function showLogin() {
  document.getElementById('login-page').style.display = 'flex';
  document.getElementById('app').style.display = 'none';
}

function showApp(user) {
  currentUser = user;
  document.getElementById('login-page').style.display = 'none';
  document.getElementById('app').style.display = 'block';
  const email = user.email || 'unknown';
  const name  = user.displayName || email.split('@')[0];
  document.getElementById('sb-uname').textContent = email;
  document.getElementById('sb-avatar').textContent = name[0].toUpperCase();
  document.getElementById('sb-org').textContent = 'Industrial Safety';
  if (document.getElementById('cfg-email')) document.getElementById('cfg-email').value = email;
  loadDashboard();
  setInterval(updateClock, 1000);
  updateClock();
  lucide.createIcons();
}

async function doLogin() {
  const email = document.getElementById('login-email').value.trim();
  const pass  = document.getElementById('login-pass').value;
  const errEl = document.getElementById('login-err');
  errEl.style.display = 'none';
  if (!firebase.apps?.length) {
    showApp({ email, displayName: email.split('@')[0] });
    return;
  }
  try {
    await firebase.auth().signInWithEmailAndPassword(email, pass);
  } catch (e) {
    errEl.textContent = e.code === 'auth/invalid-credential' ? 'Incorrect email or password.' : e.message;
    errEl.style.display = 'block';
  }
}

async function doResetPassword() {
  const email = document.getElementById('login-email').value.trim();
  if (!email) { toast('Enter your email address first', 'warn'); return; }
  try {
    await firebase.auth().sendPasswordResetEmail(email);
    toast('Password reset email sent', 'ok');
  } catch(e) { toast(e.message, 'err'); }
}

async function doLogout() {
  if (window.firebase?.auth) await firebase.auth().signOut();
  else showLogin();
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────
const PAGE_TITLES = {
  dashboard: 'Dashboard',
  nodes:     'Sensor Nodes',
  alerts:    'Alerts',
  incidents: 'Incidents',
  readings:  'Readings',
  reports:   'Reports',
  devices:   'Device Registry',
  settings:  'Settings',
  topology:  'Topology'
};

function navTo(page, el) {
  activePage = page;
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.ni').forEach(n => n.classList.remove('active'));
  document.getElementById('page-' + page).classList.add('active');
  if (el) el.classList.add('active');
  else {
    const ni = document.querySelector(`[data-page="${page}"]`);
    if (ni) ni.classList.add('active');
  }
  document.getElementById('tb-title').textContent = PAGE_TITLES[page] || page;
  // Lazy-load page data
  const loaders = {
    alerts:    loadAlerts,
    nodes:     loadNodes,
    readings:  loadReadings,
    incidents: loadIncidents,
    devices:   loadDevices,
    settings:  loadSettings,
    topology:  loadTopology
  };
  if (loaders[page]) loaders[page]();
}

function updateClock() {
  document.getElementById('tb-time').textContent = new Date().toLocaleTimeString();
}

function syncNow() {
  const active = document.querySelector('.page.active');
  if (active) navTo(active.id.replace('page-',''));
  else loadDashboard();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
function fmtTime(epoch) {
  if (!epoch) return '—';
  return new Date(epoch * 1000).toLocaleString();
}

function fmtAgo(epoch) {
  if (!epoch) return '—';
  const s = Math.floor(Date.now()/1000 - epoch);
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s/60) + 'm ago';
  if (s < 86400) return Math.floor(s/3600) + 'h ago';
  return Math.floor(s/86400) + 'd ago';
}

function alertBadge(state) {
  const map = { high:'b-high', low:'b-low', gas:'b-gas', active:'b-active', ok:'b-ok' };
  return `<span class="badge ${map[state]||'b-info'}">${(state||'ok').toUpperCase()}</span>`;
}

function statusBadge(s) {
  const map = { pending:'b-pending', acknowledged:'b-acked', resolved:'b-resolved', open:'b-pending', investigating:'b-acked', closed:'b-resolved', active:'b-ok', inactive:'b-offline' };
  return `<span class="badge ${map[s]||'b-info'}">${(s||'—').toUpperCase()}</span>`;
}

function sevBadge(s) {
  const map = { critical:'b-active', high:'b-high', medium:'b-acked', low:'b-ok' };
  return `<span class="badge ${map[s]||'b-info'}">${(s||'—').toUpperCase()}</span>`;
}

function openModal(id) { document.getElementById(id).classList.add('open'); }
function closeModal(id) { document.getElementById(id).classList.remove('open'); }

function confirmDelete(msg, callback) {
  document.getElementById('confirm-msg').textContent = msg;
  document.getElementById('confirm-btn').onclick = () => { closeModal('modal-confirm'); callback(); };
  openModal('modal-confirm');
}

function mkPager(containerId, state, total, loader) {
  const pages = Math.ceil(total / state.limit);
  const el = document.getElementById(containerId);
  if (!el) return;
  el.innerHTML = `<span>${total} record${total!==1?'s':''}</span>
    <button onclick="(function(){if(${state.page}>0){state.page--;loader();}}).call(window)" ${state.page===0?'disabled':''}>Prev</button>
    <span>Page ${state.page+1} of ${pages||1}</span>
    <button onclick="(function(){if(${state.page}<${pages-1}){state.page++;loader();}}).call(window)" ${state.page>=pages-1?'disabled':''}>Next</button>`;
  // Rebind buttons properly
  const btns = el.querySelectorAll('button');
  btns[0].onclick = () => { if (state.page > 0) { state.page--; loader(); } };
  btns[1].onclick = () => { if (state.page < pages-1) { state.page++; loader(); } };
}

// ─────────────────────────────────────────────────────────────────────────────
// Dashboard
// ─────────────────────────────────────────────────────────────────────────────
async function loadDashboard() {
  const cfg  = getMongoConfig();
  const params = new URLSearchParams({
    database:      cfg.db,
    readingsCol:   cfg.cols.readings  || 'sensor_readings',
    alertsCol:     cfg.cols.acks      || 'alert_acks',
    incidentsCol:  cfg.cols.incidents || 'incidents'
  });

  let data;
  try {
    const res = await fetch(`/api/analytics/dashboard?${params}`);
    if (!res.ok) throw new Error(await res.text());
    data = await res.json();
  } catch(e) {
    toast('Dashboard load failed: ' + e.message, 'err');
    return;
  }

  const s = data.stats || {};

  // KPI cards
  _setKpi('s-records',   s.records24h   ?? '—', 'kpi-bar-records',   s.records24h, 500);
  _setKpi('s-nodes',     s.activeNodes  ?? '—', 'kpi-bar-nodes',     s.activeNodes, 20);
  _setKpi('s-centers',   s.activeCenters?? '—', 'kpi-bar-centers',   s.activeCenters, 10);
  _setKpi('s-alerts',    s.activeAlerts ?? '—', 'kpi-bar-alerts',    s.activeAlerts, 20);
  _setKpi('s-incidents', s.openIncidents?? '—', 'kpi-bar-incidents', s.openIncidents, 10);
  document.getElementById('s-syncrate').textContent = s.syncRatePerHour ?? '—';
  document.getElementById('s-syncrate-sub').textContent = 'last hour';

  // Color alerts KPI red if non-zero
  const aEl = document.getElementById('s-alerts');
  aEl.style.color = s.activeAlerts > 0 ? 'var(--red)' : 'var(--grn)';

  // Nav alert badge
  const badge = document.getElementById('nav-alert-count');
  badge.style.display = s.activeAlerts > 0 ? 'inline' : 'none';
  badge.textContent   = s.activeAlerts;

  // Alert banner
  if (s.activeAlerts > 0) {
    document.getElementById('alert-banner').classList.add('show');
    document.getElementById('alert-banner-text').textContent =
      `${s.activeAlerts} unresolved alert${s.activeAlerts > 1 ? 's' : ''} require attention.`;
  } else {
    document.getElementById('alert-banner').classList.remove('show');
  }

  // Live sensor tiles
  _renderSensorTiles(data.recentReadings || []);

  // Active alerts list
  _renderLiveAlerts(data.liveAlerts || []);

  // Recent syncs table
  _renderRecentSyncs(data.recentReadings || []);

  // Charts
  _renderAlertTrend(data.alertsByDay || []);
  _renderAlertTypes(data.alertByType || []);
}

function _setKpi(valId, val, barId, raw, max) {
  document.getElementById(valId).textContent = val;
  const bar = document.getElementById(barId);
  if (bar && typeof raw === 'number' && max > 0)
    bar.style.width = Math.min(100, (raw / max) * 100) + '%';
}

function _renderSensorTiles(docs) {
  const el = document.getElementById('dash-sensor-tiles');
  // Build a flat map: latest reading per sensor across all nodes
  const byId = {};
  [...docs].sort((a,b) => a.timestamp - b.timestamp).forEach(doc => {
    (doc.nodes || []).forEach(node => {
      (node.readings || []).forEach(r => {
        if (r.type === 'relay') return;
        byId[node.nodeId + '|' + r.sensorId] = { ...r, nodeHostname: node.hostname, centerId: doc.centerId, docTs: doc.timestamp };
      });
    });
  });

  const tiles = Object.values(byId);
  if (!tiles.length) { el.innerHTML = `<div style="color:var(--td);font-size:12px;padding:8px 0;grid-column:1/-1">No sensor data available</div>`; return; }

  // Latest doc timestamp
  const latest = Math.max(...docs.map(d => d.timestamp || 0));
  document.getElementById('dash-live-ts').textContent = latest ? 'Updated ' + fmtAgo(latest) : 'Latest reading from all connected nodes';

  const alertColor = { high:'var(--amb)', gas:'var(--red)', active:'var(--red)', low:'var(--blu)', ok:'var(--grn)' };
  const tileClass  = { high:'st-warn', gas:'st-alert', active:'st-alert', low:'', ok:'' };

  el.innerHTML = tiles.map(r => {
    const val = typeof r.value === 'number' ? r.value.toFixed(2) : (r.value ?? '—');
    const cls = tileClass[r.alertState] || '';
    const dot = alertColor[r.alertState] || 'var(--td)';
    return `<div class="sensor-tile ${cls}">
      <div class="sensor-tile-top">
        <div>
          <div class="sensor-tile-name">${escH(r.name || r.sensorId)}</div>
          <div class="sensor-tile-node">${escH(r.nodeHostname || r.centerId || '')}</div>
        </div>
        <span class="badge ${r.alertState === 'ok' ? 'b-ok' : (r.alertState === 'high' ? 'b-high' : r.alertState === 'low' ? 'b-low' : 'b-gas')}" style="font-size:9px">${(r.alertState||'ok').toUpperCase()}</span>
      </div>
      <div class="sensor-tile-val">${val}</div>
      <div class="sensor-tile-unit">${escH(r.unit || r.type || '')}</div>
      <div class="sensor-tile-meta">
        <span>${fmtAgo(r.timestamp || r.docTs)}</span>
      </div>
    </div>`;
  }).join('');
}

function _renderLiveAlerts(alerts) {
  const el = document.getElementById('dash-live-alerts');
  if (!alerts.length) { el.innerHTML = `<div style="color:var(--td);font-size:12px;padding:16px 0 8px">No active alerts</div>`; return; }
  const dotCls = { high:'amb', gas:'red', active:'red', low:'blu' };
  el.innerHTML = `<div class="la-list">` + alerts.map(a => `
    <div class="la-item">
      <div class="la-dot ${dotCls[a.alertState] || 'red'}"></div>
      <div>
        <div class="la-name">${escH(a.sensorName || a.sensorId || '—')}</div>
        <div style="font-size:10px;color:var(--td)">${escH(a.nodeHostname||'')} · ${escH(a.centerId||'')}</div>
      </div>
      <div style="margin-left:auto;text-align:right">
        <div class="la-val">${typeof a.value === 'number' ? a.value.toFixed(2) : (a.value||'—')} ${escH(a.unit||'')}</div>
        <div class="la-time">${fmtAgo(a.occurredAt)}</div>
      </div>
    </div>`).join('') + `</div>`;
}

function _renderRecentSyncs(docs) {
  const tbody = document.getElementById('tbody-recent');
  if (!docs.length) { tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;padding:16px;color:var(--td)">No data</td></tr>`; return; }
  // Flatten to one row per node per doc (latest 12)
  const rows = [];
  [...docs].sort((a,b) => b.timestamp - a.timestamp).forEach(doc => {
    (doc.nodes || []).forEach(node => {
      const alerts = (node.readings || []).filter(r => r.alertState && r.alertState !== 'ok');
      const worstState = alerts.length ? alerts[0].alertState : 'ok';
      rows.push({ doc, node, state: worstState, count: node.readings?.length || 0 });
    });
  });
  tbody.innerHTML = rows.slice(0, 12).map(({ doc, node, state, count }) => `<tr>
    <td class="mono td-dim" style="font-size:11px">${fmtTime(doc.timestamp)}</td>
    <td style="font-size:12px">${escH(node.hostname || node.nodeId || '—')}</td>
    <td class="mono" style="font-size:12px">${count}</td>
    <td>${alertBadge(state)}</td>
  </tr>`).join('');
}

function _renderAlertTrend(byDay) {
  const days = [];
  for (let i = 6; i >= 0; i--) {
    const d = new Date(); d.setDate(d.getDate() - i);
    days.push(d.toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric' }));
  }
  const ctx = document.getElementById('chart-alerts').getContext('2d');
  if (charts.alerts) charts.alerts.destroy();
  charts.alerts = new Chart(ctx, {
    type: 'bar',
    data: {
      labels: days,
      datasets: [{ label: 'Alerts', data: [...byDay].reverse(), backgroundColor: 'rgba(220,38,38,.65)', borderRadius: 5 }]
    },
    options: { responsive: true, maintainAspectRatio: false, plugins: { legend: { display: false } }, scales: { y: { beginAtZero: true, ticks: { stepSize: 1 }, grid: { color: 'rgba(0,0,0,.04)' } }, x: { grid: { display: false } } } }
  });
}

function _renderAlertTypes(byType) {
  if (!byType.length) return;
  const colorMap = { high: '#d97706', gas: '#dc2626', active: '#dc2626', low: '#0284c7', ok: '#16a34a' };
  const ctx = document.getElementById('chart-types').getContext('2d');
  if (charts.types) charts.types.destroy();
  charts.types = new Chart(ctx, {
    type: 'doughnut',
    data: {
      labels: byType.map(d => (d._id || 'unknown').toUpperCase()),
      datasets: [{ data: byType.map(d => d.count), backgroundColor: byType.map(d => colorMap[d._id] || '#7c3aed'), borderWidth: 2, borderColor: '#fff' }]
    },
    options: { responsive: true, maintainAspectRatio: false, plugins: { legend: { position: 'bottom', labels: { font: { size: 11 }, padding: 12 } } }, cutout: '60%' }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nodes page
// ─────────────────────────────────────────────────────────────────────────────
let allNodes = [];

async function loadNodes() {
  const tbody = document.getElementById('tbody-nodes');
  tbody.innerHTML = `<tr class="loading-row"><td colspan="7"><span class="spinner"></span> Loading...</td></tr>`;
  try {
    // Pull known nodes from registry
    const regRes = await mongo.find(col('nodes'), {}, { sort: { lastSeen: -1 }, limit: 200 });
    allNodes = regRes.documents || [];

    // Also discover nodes from recent readings that may not be in registry
    const recRes = await mongo.aggregate(col('readings'), [
      { $sort: { timestamp: -1 } },
      { $limit: 100 },
      { $unwind: '$nodes' },
      { $group: { _id: '$nodes.nodeId', hostname: { $first: '$nodes.hostname' }, centerId: { $first: '$centerId' }, lastSeen: { $max: '$timestamp' }, location: { $first: '$location' } } }
    ]);
    (recRes.documents || []).forEach(d => {
      if (!allNodes.find(n => n.nodeId === d._id)) {
        allNodes.push({ nodeId: d._id, hostname: d.hostname, centerId: d.centerId, lastSeen: d.lastSeen, location: d.location||{}, status: 'active', _discovered: true });
      }
    });

    renderNodes(allNodes);
  } catch(e) {
    tbody.innerHTML = `<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function filterNodes() {
  const q = (document.getElementById('node-search').value||'').toLowerCase();
  const s = document.getElementById('node-filter-status').value;
  const filtered = allNodes.filter(n =>
    (!q || (n.hostname||'').toLowerCase().includes(q) || (n.nodeId||'').includes(q)) &&
    (!s || n.status === s)
  );
  renderNodes(filtered);
}

function renderNodes(nodes) {
  const tbody = document.getElementById('tbody-nodes');
  document.getElementById('node-count').textContent = nodes.length + ' nodes';
  if (!nodes.length) {
    tbody.innerHTML = '<tr><td colspan="7"><div class="empty"><div class="ico"><i data-lucide="server-off"></i></div><div class="lbl">No nodes found</div></div></td></tr>';
    lucide.createIcons();
    return;
  }
  tbody.innerHTML = nodes.map(n => `<tr>
    <td class="mono td-dim" style="font-size:11px">${n.nodeId||'—'}</td>
    <td><strong>${n.hostname||n.nodeId||'Unknown'}</strong></td>
    <td>${n.centerId||'—'}</td>
    <td style="font-size:12px">${[n.location?.factory, n.location?.building, n.location?.room].filter(Boolean).join(' / ')||'—'}</td>
    <td class="mono td-dim" style="font-size:11px">${fmtAgo(n.lastSeen)}</td>
    <td>${statusBadge(n._discovered?'active':n.status||'active')}</td>
    <td>
      ${!n._discovered ? `<button class="btn-ghost btn-sm" onclick="editNode('${n._id?.$oid||n.nodeId}')">Edit</button>
      <button class="btn-ghost btn-sm" style="color:var(--red)" onclick="deleteNode('${n._id?.$oid}','${n.hostname||n.nodeId}')">Delete</button>` : '<span style="font-size:11px;color:var(--ts)">discovered</span>'}
    </td>
  </tr>`).join('');
}

function openNodeModal(id) {
  document.getElementById('modal-node-title').textContent = id ? 'Edit Node' : 'Add Node to Registry';
  ['nr-id','nr-host','nr-center','nr-owner','nr-desc','nr-factory','nr-building','nr-room'].forEach(f => document.getElementById(f).value = '');
  activeIncId = id || null;
  openModal('modal-node');
}

async function submitNodeRegistry() {
  const doc = {
    nodeId:   document.getElementById('nr-id').value.trim(),
    hostname: document.getElementById('nr-host').value.trim(),
    centerId: document.getElementById('nr-center').value.trim(),
    owner:    document.getElementById('nr-owner').value.trim(),
    description: document.getElementById('nr-desc').value.trim(),
    location: {
      factory:  document.getElementById('nr-factory').value.trim(),
      building: document.getElementById('nr-building').value.trim(),
      room:     document.getElementById('nr-room').value.trim()
    },
    status:  'active',
    addedAt: Math.floor(Date.now()/1000),
    addedBy: currentUser?.email || 'unknown'
  };
  if (!doc.nodeId) { toast('Node ID is required', 'warn'); return; }
  try {
    await mongo.insertOne(col('nodes'), doc);
    toast('Node added to registry', 'ok');
    closeModal('modal-node');
    loadNodes();
  } catch(e) { toast(e.message, 'err'); }
}

async function deleteNode(id, name) {
  if (!id) return;
  confirmDelete(`Delete node "${name}" from the registry? This does not remove its sensor data.`, async () => {
    try {
      await mongo.deleteOne(col('nodes'), { _id: { $oid: id } });
      toast('Node removed from registry', 'ok');
      loadNodes();
    } catch(e) { toast(e.message, 'err'); }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Alerts page
// ─────────────────────────────────────────────────────────────────────────────
async function loadAlerts() {
  pageState.alerts.page = 0;
  await fetchAlerts();
}

async function fetchAlerts() {
  const tbody = document.getElementById('tbody-alerts');
  tbody.innerHTML = `<tr class="loading-row"><td colspan="8"><span class="spinner"></span> Loading...</td></tr>`;
  const filter = {};
  const status = document.getElementById('al-filter-status')?.value;
  const type   = document.getElementById('al-filter-type')?.value;
  const from   = document.getElementById('al-filter-from')?.value;
  const to     = document.getElementById('al-filter-to')?.value;
  if (status) filter.status = status;
  if (type)   filter.alertState = type;
  if (from || to) {
    filter.occurredAt = {};
    if (from) filter.occurredAt.$gte = Math.floor(new Date(from).getTime()/1000);
    if (to)   filter.occurredAt.$lte = Math.floor(new Date(to).getTime()/1000) + 86400;
  }
  try {
    const r = await mongo.find(col('acks'), filter, {
      sort: { occurredAt: -1 },
      limit: pageState.alerts.limit,
      skip:  pageState.alerts.page * pageState.alerts.limit
    });
    const cnt = await mongo.aggregate(col('acks'), [{ $match: filter }, { $count: 'n' }]);
    const total = cnt.documents?.[0]?.n || 0;
    document.getElementById('alert-count-txt').textContent = total + ' records';
    const docs = r.documents || [];
    if (!docs.length) {
      tbody.innerHTML = '<tr><td colspan="8"><div class="empty"><div class="ico"><i data-lucide="shield-check"></i></div><div class="lbl">No alerts found</div><div class="sub">Use "Scan Records" to detect alerts from sensor readings</div></div></td></tr>';
      lucide.createIcons();
    } else {
      tbody.innerHTML = docs.map(a => `<tr>
        <td class="mono td-dim" style="font-size:11px">${fmtTime(a.occurredAt)}</td>
        <td><strong>${a.nodeHostname||a.nodeId||'—'}</strong><div style="font-size:11px;color:var(--ts)">${a.centerId||''}</div></td>
        <td>${a.sensorName||a.sensorId||'—'}</td>
        <td>${alertBadge(a.alertState)}</td>
        <td class="mono">${typeof a.value==='number'?a.value.toFixed(2):a.value||'—'} <span style="font-size:10px;color:var(--td)">${a.unit||''}</span></td>
        <td>${statusBadge(a.status)}</td>
        <td style="font-size:12px;color:var(--ts)">${a.acknowledgedBy||'—'}<div style="font-size:10px">${a.acknowledgedAt?fmtAgo(a.acknowledgedAt):''}</div></td>
        <td>
          ${a.status!=='resolved'?`<button class="btn btn-sm btn-acc" onclick="openAck('${a._id?.$oid}')">Acknowledge</button> `:''}
          <button class="btn-ghost btn-sm" style="color:var(--red)" onclick="deleteAlert('${a._id?.$oid}')">Del</button>
        </td>
      </tr>`).join('');
    }
    mkPager('pager-alerts', pageState.alerts, total, fetchAlerts);
  } catch(e) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function clearAlertFilters() {
  ['al-filter-status','al-filter-type','al-filter-from','al-filter-to'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.value = '';
  });
  loadAlerts();
}

function openAck(id) {
  activeAlert = id;
  document.getElementById('ack-note').value = '';
  document.getElementById('ack-action').value = 'acknowledged';
  openModal('modal-ack');
}

async function submitAck() {
  if (!activeAlert) return;
  const action = document.getElementById('ack-action').value;
  const note   = document.getElementById('ack-note').value.trim();
  try {
    await mongo.updateOne(col('acks'),
      { _id: { $oid: activeAlert } },
      { $set: {
          status:          action,
          acknowledgedBy:  currentUser?.email || 'unknown',
          acknowledgedAt:  Math.floor(Date.now()/1000),
          note
        }
      }
    );
    toast('Alert ' + action, 'ok');
    closeModal('modal-ack');
    activeAlert = null;
    fetchAlerts();
    loadDashboard();
  } catch(e) { toast(e.message, 'err'); }
}

async function deleteAlert(id) {
  if (!id) return;
  confirmDelete('Delete this alert record permanently?', async () => {
    try {
      await mongo.deleteOne(col('acks'), { _id: { $oid: id } });
      toast('Alert deleted', 'ok');
      fetchAlerts();
    } catch(e) { toast(e.message, 'err'); }
  });
}

// Scan sensor_readings collection and create ack records for unacknowledged alerts
async function scanForAlerts() {
  toast('Scanning for alerts...', 'info');
  try {
    const r = await mongo.find(col('readings'),
      { 'nodes.readings.alertState': { $nin: ['ok', null, ''] } },
      { sort: { timestamp: -1 }, limit: 200 }
    );
    let created = 0;
    for (const doc of r.documents || []) {
      for (const node of doc.nodes || []) {
        for (const reading of node.readings || []) {
          if (!reading.alertState || reading.alertState === 'ok') continue;
          // Check if already logged
          const existing = await mongo.findOne(col('acks'), {
            centerId: doc.centerId,
            nodeId:   node.nodeId,
            sensorId: reading.sensorId,
            occurredAt: doc.timestamp
          });
          if (existing.document) continue;
          await mongo.insertOne(col('acks'), {
            centerId:      doc.centerId,
            nodeId:        node.nodeId,
            nodeHostname:  node.hostname || node.nodeId,
            sensorId:      reading.sensorId,
            sensorName:    reading.name || reading.sensorId,
            sensorType:    reading.type,
            alertState:    reading.alertState,
            value:         reading.value,
            unit:          reading.unit || '',
            occurredAt:    doc.timestamp,
            status:        'pending',
            acknowledgedBy: null,
            acknowledgedAt: null,
            note:           '',
            location:       doc.location || {}
          });
          created++;
        }
      }
    }
    toast(created > 0 ? `${created} new alert records created` : 'No new alerts found', created > 0 ? 'warn' : 'ok');
    if (created > 0) { fetchAlerts(); loadDashboard(); }
  } catch(e) { toast(e.message, 'err'); }
}

function exportAlertsCSV() {
  const tbody = document.getElementById('tbody-alerts');
  const rows = [['Time','Center','Node','Sensor','Alert Type','Value','Unit','Status','Acknowledged By']];
  tbody.querySelectorAll('tr').forEach(tr => {
    const cells = tr.querySelectorAll('td');
    if (cells.length < 6) return;
    rows.push([
      cells[0].textContent.trim(),
      cells[1].textContent.trim(),
      cells[2].textContent.trim(),
      cells[3].textContent.trim(),
      cells[4].textContent.trim(),
      cells[5].textContent.trim(),
      cells[6].textContent.trim()
    ]);
  });
  downloadCSV(rows, 'isms_alerts_' + new Date().toISOString().slice(0,10) + '.csv');
}

// ─────────────────────────────────────────────────────────────────────────────
// Incidents
// ─────────────────────────────────────────────────────────────────────────────
async function loadIncidents() {
  await fetchIncidents();
}

async function fetchIncidents() {
  const tbody = document.getElementById('tbody-incidents');
  tbody.innerHTML = `<tr class="loading-row"><td colspan="8"><span class="spinner"></span> Loading...</td></tr>`;
  const filter = {};
  const status   = document.getElementById('inc-filter-status')?.value;
  const severity = document.getElementById('inc-filter-severity')?.value;
  if (status)   filter.status   = status;
  if (severity) filter.severity = severity;
  try {
    const r = await mongo.find(col('incidents'), filter, {
      sort: { raisedAt: -1 },
      limit: pageState.incidents.limit,
      skip:  pageState.incidents.page * pageState.incidents.limit
    });
    const cnt = await mongo.aggregate(col('incidents'), [{ $match: filter }, { $count: 'n' }]);
    const total = cnt.documents?.[0]?.n || 0;
    document.getElementById('inc-count').textContent = total + ' incidents';
    const docs = r.documents || [];
    if (!docs.length) {
      tbody.innerHTML = '<tr><td colspan="8"><div class="empty"><div class="ico"><i data-lucide="clipboard-list"></i></div><div class="lbl">No incidents found</div></div></td></tr>';
      lucide.createIcons();
    } else {
      tbody.innerHTML = docs.map((inc,i) => {
        const id = inc._id?.$oid || i;
        const shortId = id.toString().slice(-6).toUpperCase();
        return `<tr>
          <td class="mono" style="font-size:11px">INC-${shortId}</td>
          <td><strong>${inc.title||'Untitled'}</strong></td>
          <td>${sevBadge(inc.severity)}</td>
          <td style="font-size:12px">${inc.node||''}<div style="font-size:11px;color:var(--ts)">${inc.location||''}</div></td>
          <td class="mono td-dim" style="font-size:11px">${fmtTime(inc.raisedAt)}</td>
          <td>${statusBadge(inc.status)}</td>
          <td style="font-size:12px;color:var(--ts)">${inc.assignee||'—'}</td>
          <td>
            <button class="btn-ghost btn-sm" onclick="editIncident('${id}')">Edit</button>
            <button class="btn-ghost btn-sm" style="color:var(--red)" onclick="deleteIncident('${id}','${(inc.title||'').replace(/'/g,'')}')">Del</button>
          </td>
        </tr>`;
      }).join('');
    }
    mkPager('pager-incidents', pageState.incidents, total, fetchIncidents);
  } catch(e) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function openIncidentModal() {
  activeIncId = null;
  document.getElementById('modal-inc-title').textContent = 'New Incident';
  ['inc-title','inc-node','inc-location','inc-assignee','inc-desc'].forEach(f => document.getElementById(f).value = '');
  document.getElementById('inc-severity').value = 'medium';
  document.getElementById('inc-status').value   = 'open';
  document.getElementById('inc-due').value = '';
  openModal('modal-incident');
}

async function editIncident(id) {
  try {
    const r = await mongo.findOne(col('incidents'), { _id: { $oid: id } });
    const inc = r.document;
    if (!inc) return;
    activeIncId = id;
    document.getElementById('modal-inc-title').textContent = 'Edit Incident';
    document.getElementById('inc-title').value    = inc.title || '';
    document.getElementById('inc-severity').value = inc.severity || 'medium';
    document.getElementById('inc-status').value   = inc.status || 'open';
    document.getElementById('inc-node').value     = inc.node || '';
    document.getElementById('inc-location').value = inc.location || '';
    document.getElementById('inc-assignee').value = inc.assignee || '';
    document.getElementById('inc-due').value      = inc.dueDate || '';
    document.getElementById('inc-desc').value     = inc.description || '';
    openModal('modal-incident');
  } catch(e) { toast(e.message, 'err'); }
}

async function submitIncident() {
  const doc = {
    title:       document.getElementById('inc-title').value.trim(),
    severity:    document.getElementById('inc-severity').value,
    status:      document.getElementById('inc-status').value,
    node:        document.getElementById('inc-node').value.trim(),
    location:    document.getElementById('inc-location').value.trim(),
    assignee:    document.getElementById('inc-assignee').value.trim(),
    dueDate:     document.getElementById('inc-due').value,
    description: document.getElementById('inc-desc').value.trim(),
    updatedAt:   Math.floor(Date.now()/1000),
    updatedBy:   currentUser?.email || 'unknown'
  };
  if (!doc.title) { toast('Title is required', 'warn'); return; }
  try {
    if (activeIncId) {
      await mongo.updateOne(col('incidents'), { _id: { $oid: activeIncId } }, { $set: doc });
      toast('Incident updated', 'ok');
    } else {
      doc.raisedAt  = Math.floor(Date.now()/1000);
      doc.raisedBy  = currentUser?.email || 'unknown';
      await mongo.insertOne(col('incidents'), doc);
      toast('Incident created', 'ok');
    }
    closeModal('modal-incident');
    fetchIncidents();
    loadDashboard();
  } catch(e) { toast(e.message, 'err'); }
}

async function deleteIncident(id, title) {
  confirmDelete(`Delete incident "${title}"? This cannot be undone.`, async () => {
    try {
      await mongo.deleteOne(col('incidents'), { _id: { $oid: id } });
      toast('Incident deleted', 'ok');
      fetchIncidents();
    } catch(e) { toast(e.message, 'err'); }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Readings page — drill-down: Central Nodes → Sensor Nodes → Readings
// ─────────────────────────────────────────────────────────────────────────────
const rdState = {
  level:    1,
  centerId: null,
  nodeId:   null,
  nodeHostname: null,
  _centerData: null,       // cached center detail response for pager re-renders
  centers:  { page: 0, limit: 10 },
  nodes:    { page: 0, limit: 15 },
  readings: { page: 0, limit: 25 },
  timeFrom: null,     // epoch seconds, null = use default (24h ago)
  timeTo:   null,     // epoch seconds, null = use now
  timeWindow: 86400,  // active quick-select window in seconds
  _autoRefreshTimer: null,
  _activeSensors: {},  // sensorId → true/false for multi-chart toggle
};

function _rdAnalyticsUrl(path) {
  const cfg = getMongoConfig();
  return `/api/analytics/${path}?database=${encodeURIComponent(cfg.db)}&collection=${encodeURIComponent(cfg.cols.readings || 'sensor_readings')}`;
}

// Entry point called by the nav / lazy loader
async function loadReadings() {
  rdState.level    = 1;
  rdState.centerId = null;
  rdState.nodeId   = null;
  rdState.centers.page = 0;
  _rdShowLevel(1);
  await _rdFetchCenters();
}

function _rdShowLevel(n) {
  document.getElementById('rd-level-centers').style.display = n === 1 ? '' : 'none';
  document.getElementById('rd-level-center').style.display  = n === 2 ? '' : 'none';
  document.getElementById('rd-level-node').style.display    = n === 3 ? '' : 'none';
  document.getElementById('rd-export-btn').style.display    = n === 3 ? '' : 'none';
  _rdUpdateBreadcrumb();
}

function _rdUpdateBreadcrumb() {
  const bc = document.getElementById('rd-breadcrumb');
  if (!bc) return;
  const sep = `<span style="color:var(--td);margin:0 2px">/</span>`;
  let html = `<span style="cursor:pointer;color:var(--acc)" onclick="rdNavTo(1)">Central Nodes</span>`;
  if (rdState.centerId) {
    html += sep;
    if (rdState.level > 2)
      html += `<span style="cursor:pointer;color:var(--acc)" onclick="rdNavTo(2)">${escH(rdState.centerId)}</span>`;
    else
      html += `<span style="color:var(--ts)">${escH(rdState.centerId)}</span>`;
  }
  if (rdState.nodeId) {
    html += sep;
    html += `<span style="color:var(--ts)">${escH(rdState.nodeHostname || rdState.nodeId)}</span>`;
  }
  bc.innerHTML = html;
}

async function rdNavTo(level) {
  if (level === 1) {
    rdState.level = 1; rdState.centerId = null; rdState.nodeId = null;
    _rdShowLevel(1);
  } else if (level === 2 && rdState.centerId) {
    rdState.level = 2; rdState.nodeId = null;
    rdState.nodes.page = 0;
    _rdShowLevel(2);
    if (rdState._centerData) _rdRenderNodes(rdState._centerData);
    else await _rdFetchCenterDetail(rdState.centerId);
  }
}

// ── Level 1 ──────────────────────────────────────────────────────────────────

async function _rdFetchCenters() {
  const tbody = document.getElementById('tbody-rd-centers');
  tbody.innerHTML = `<tr class="loading-row"><td colspan="6"><span class="spinner"></span> Loading...</td></tr>`;
  try {
    const res = await fetch(_rdAnalyticsUrl('centers'));
    if (!res.ok) throw new Error(await res.text());
    const { centers } = await res.json();
    rdState._allCenters = centers;
    _rdRenderCenters(centers);
  } catch(e) {
    tbody.innerHTML = `<tr><td colspan="6" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function _rdRenderCenters(centers) {
  const tbody = document.getElementById('tbody-rd-centers');
  const { page, limit } = rdState.centers;
  const slice = centers.slice(page * limit, page * limit + limit);
  document.getElementById('rd-centers-count').textContent = `${centers.length} central node${centers.length !== 1 ? 's' : ''}`;
  if (!slice.length) {
    tbody.innerHTML = '<tr><td colspan="6"><div class="empty"><div class="ico"><i data-lucide="radio"></i></div><div class="lbl">No central nodes found</div><div class="sub">Data appears once a center node syncs to this server</div></div></td></tr>';
    lucide.createIcons();
  } else {
    tbody.innerHTML = slice.map(c => {
      const loc = c.location ? [c.location.factory, c.location.building, c.location.room].filter(Boolean).join(' / ') : '—';
      return `<tr>
        <td style="font-weight:600;font-family:var(--mono)">${escH(c.centerId||'—')}</td>
        <td class="td-dim">${escH(loc)}</td>
        <td class="mono td-dim" style="font-size:11px">${fmtTime(c.lastSync)}</td>
        <td class="mono">${c.nodeCount ?? '—'}</td>
        <td class="mono">${c.recordCount ?? '—'}</td>
        <td><button class="btn btn-acc btn-sm" onclick='rdDrillCenter("${escH(c.centerId)}")'>View →</button></td>
      </tr>`;
    }).join('');
  }
  mkPager('pager-rd-centers', rdState.centers, centers.length, () => _rdRenderCenters(rdState._allCenters || []));
}

// ── Level 2 ──────────────────────────────────────────────────────────────────

async function rdDrillCenter(centerId) {
  rdState.centerId = centerId;
  rdState.nodeId   = null;
  rdState.level    = 2;
  rdState.nodes.page = 0;
  rdState._centerData = null;
  _rdShowLevel(2);
  await _rdFetchCenterDetail(centerId);
}

async function _rdFetchCenterDetail(centerId) {
  const detailCard = document.getElementById('rd-center-detail-card');
  const tbody = document.getElementById('tbody-rd-nodes');
  detailCard.innerHTML = `<div style="padding:8px"><span class="spinner"></span> Loading…</div>`;
  tbody.innerHTML = `<tr class="loading-row"><td colspan="6"><span class="spinner"></span> Loading...</td></tr>`;
  try {
    const res = await fetch(_rdAnalyticsUrl(`center/${encodeURIComponent(centerId)}`));
    if (!res.ok) throw new Error(await res.text());
    const data = await res.json();
    rdState._centerData = data;
    _rdRenderCenterCard(data);
    _rdRenderNodes(data);
  } catch(e) {
    detailCard.innerHTML = `<div style="color:var(--red);padding:12px">${e.message}</div>`;
    tbody.innerHTML = `<tr><td colspan="6" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function _rdRenderCenterCard(data) {
  const card = document.getElementById('rd-center-detail-card');
  const loc = data.location ? [data.location.factory, data.location.building, data.location.room].filter(Boolean).join(' / ') : '—';
  const locParts = [
    data.location?.factory  ? `<div><div class="rd-stat-lbl">Factory</div><div class="rd-stat-val">${escH(data.location.factory)}</div></div>`  : '',
    data.location?.building ? `<div><div class="rd-stat-lbl">Building</div><div class="rd-stat-val">${escH(data.location.building)}</div></div>` : '',
    data.location?.room     ? `<div><div class="rd-stat-lbl">Room</div><div class="rd-stat-val">${escH(data.location.room)}</div></div>`         : ''
  ].join('');
  card.innerHTML = `
    <div class="card-hd">
      <div><div class="card-title">${escH(data.centerId)}</div><div class="card-sub">${escH(loc)}</div></div>
    </div>
    <div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:16px;margin-top:4px">
      <div><div class="rd-stat-lbl">Last Sync</div><div class="mono" style="font-size:12px;margin-top:4px">${fmtTime(data.lastSync)}</div></div>
      <div><div class="rd-stat-lbl">Total Records</div><div class="mono rd-stat-num">${data.recordCount}</div></div>
      <div><div class="rd-stat-lbl">Sensor Nodes</div><div class="mono rd-stat-num">${data.nodes.length}</div></div>
      ${locParts}
    </div>`;
}

function _rdRenderNodes(data) {
  const tbody = document.getElementById('tbody-rd-nodes');
  const { page, limit } = rdState.nodes;
  const slice = data.nodes.slice(page * limit, page * limit + limit);
  document.getElementById('rd-nodes-count').textContent = `${data.nodes.length} node${data.nodes.length !== 1 ? 's' : ''}`;
  if (!slice.length) {
    tbody.innerHTML = '<tr><td colspan="6"><div class="empty"><div class="ico"><i data-lucide="server-off"></i></div><div class="lbl">No nodes found</div></div></td></tr>';
    lucide.createIcons();
  } else {
    tbody.innerHTML = slice.map(n => `<tr>
      <td class="mono td-dim" style="font-size:11px">${escH(n.nodeId||'—')}</td>
      <td style="font-weight:500">${escH(n.hostname||'—')}</td>
      <td class="mono td-dim" style="font-size:11px">${fmtTime(n.lastSeen)}</td>
      <td class="mono">${n.sensorCount ?? '—'}</td>
      <td>${n.online ? '<span class="badge b-online">ONLINE</span>' : '<span class="badge b-offline">OFFLINE</span>'}</td>
      <td><button class="btn btn-acc btn-sm" onclick='rdDrillNode("${escH(n.nodeId)}","${escH(n.hostname||n.nodeId)}")'>View →</button></td>
    </tr>`).join('');
  }
  mkPager('pager-rd-nodes', rdState.nodes, data.nodes.length, () => _rdRenderNodes(rdState._centerData));
}

// ── Level 3 ──────────────────────────────────────────────────────────────────

async function rdDrillNode(nodeId, hostname) {
  rdState.nodeId       = nodeId;
  rdState.nodeHostname = hostname;
  rdState.level        = 3;
  rdState.readings.page = 0;
  rdState._activeSensors = {};
  rdState._lastSensorSummary = null;
  _rdShowLevel(3);
  _rdRenderNodeCard();
  await Promise.all([
    fetchSensorSummary(),
    fetchNodeReadings()
  ]);
  _renderMultiSensorChart(rdState._lastSensorSummary || []);
}

function _rdRenderNodeCard() {
  const card = document.getElementById('rd-node-detail-card');
  const node = (rdState._centerData?.nodes || []).find(n => n.nodeId === rdState.nodeId);
  if (!node) { card.innerHTML = ''; return; }
  const loc = rdState._centerData?.location
    ? [rdState._centerData.location.factory, rdState._centerData.location.building, rdState._centerData.location.room].filter(Boolean).join(' / ')
    : '—';
  card.innerHTML = `
    <div class="card-hd">
      <div>
        <div class="card-title">${escH(node.hostname || rdState.nodeId)}</div>
        <div class="card-sub">ID: ${escH(rdState.nodeId)} &nbsp;|&nbsp; Center: ${escH(rdState.centerId)}</div>
      </div>
      <div>${node.online ? '<span class="badge b-online">ONLINE</span>' : '<span class="badge b-offline">OFFLINE</span>'}</div>
    </div>
    <div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:16px;margin-top:4px">
      <div><div class="rd-stat-lbl">Last Seen</div><div class="mono" style="font-size:12px;margin-top:4px">${fmtTime(node.lastSeen)}</div></div>
      <div><div class="rd-stat-lbl">Sensors</div><div class="mono rd-stat-num">${node.sensorCount}</div></div>
      <div><div class="rd-stat-lbl">Location</div><div style="font-size:13px;margin-top:4px">${escH(loc)}</div></div>
    </div>`;
}

async function fetchNodeReadings() {
  const tbody = document.getElementById('tbody-readings');
  tbody.innerHTML = `<tr class="loading-row"><td colspan="5"><span class="spinner"></span> Loading...</td></tr>`;
  const q    = document.getElementById('rd-search')?.value?.toLowerCase() || '';
  const type = document.getElementById('rd-filter-type')?.value || '';
  const { from: fromTs, to: toTs } = _rdGetTimeRange();
  const from = fromTs;
  const to   = toTs;
  try {
    const cfg = getMongoConfig();
    const params = new URLSearchParams({
      database:   cfg.db,
      collection: cfg.cols.readings || 'sensor_readings',
      page:       rdState.readings.page,
      limit:      rdState.readings.limit
    });
    if (type) params.set('type', type);
    params.set('from', from);
    params.set('to',   to);

    const res = await fetch(`/api/analytics/node/${encodeURIComponent(rdState.centerId)}/${encodeURIComponent(rdState.nodeId)}?${params}`);
    if (!res.ok) throw new Error(await res.text());
    const { readings, total } = await res.json();

    const shown = q ? readings.filter(r => (r.name||'').toLowerCase().includes(q) || (r.sensorId||'').includes(q)) : readings;
    document.getElementById('rd-count').textContent = `${total} reading${total !== 1 ? 's' : ''}`;

    if (!shown.length) {
      tbody.innerHTML = `<tr><td colspan="5"><div class="empty"><div class="ico"><i data-lucide="database-zap"></i></div><div class="lbl">No readings found</div></div></td></tr>`;
      lucide.createIcons();
    } else {
      tbody.innerHTML = shown.map(r => `<tr>
        <td class="mono td-dim" style="font-size:11px">${fmtTime(r.timestamp)}</td>
        <td>${escH(r.name || r.sensorId || '—')}</td>
        <td class="mono td-dim" style="font-size:11px">${r.type || '—'}</td>
        <td class="mono">${typeof r.value === 'number' ? r.value.toFixed(2) : (r.value || '—')} <span style="font-size:10px;color:var(--td)">${r.unit||''}</span></td>
        <td>${alertBadge(r.alertState)}</td>
      </tr>`).join('');
    }
    mkPager('pager-readings', rdState.readings, total, fetchNodeReadings);
  } catch(e) {
    tbody.innerHTML = `<tr><td colspan="5" style="text-align:center;padding:24px;color:var(--red)">${e.message}</td></tr>`;
  }
}

function rdSearchChange() {
  rdState.readings.page = 0;
  fetchNodeReadings();
}

function clearReadingFilters() {
  ['rd-search', 'rd-filter-type'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.value = '';
  });
  rdState.timeWindow = 86400;
  rdState.timeFrom = null;
  rdState.timeTo = null;
  document.querySelectorAll('.tr-btn').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.tr-btn').forEach(b => { if (b.textContent === '24H') b.classList.add('active'); });
  rdState.readings.page = 0;
  if (rdState.level === 3) _rdRefreshLevel3();
  else fetchNodeReadings();
}

function exportReadingsCSV() {
  const tbody = document.getElementById('tbody-readings');
  const rows = [['Time', 'Sensor', 'Type', 'Value', 'Unit', 'Alert State']];
  tbody.querySelectorAll('tr').forEach(tr => {
    const cells = tr.querySelectorAll('td');
    if (cells.length < 4) return;
    rows.push(Array.from(cells).map(c => c.textContent.trim()));
  });
  const name = `isms_readings_${rdState.centerId||'all'}_${rdState.nodeId||'all'}_${new Date().toISOString().slice(0,10)}.csv`;
  downloadCSV(rows, name);
}

// ── Readings L3 — time range ─────────────────────────────────────────────────

function rdSetRange(seconds) {
  rdState.timeWindow = seconds;
  rdState.timeTo   = null;
  rdState.timeFrom = null;
  // Update button styles
  document.querySelectorAll('.tr-btn').forEach(b => b.classList.remove('active'));
  const labels = { 3600:'1H', 21600:'6H', 86400:'24H', 604800:'7D', 2592000:'30D' };
  document.querySelectorAll('.tr-btn').forEach(b => {
    if (b.textContent === (labels[seconds] || '')) b.classList.add('active');
  });
  _rdRefreshLevel3();
}

function rdApplyCustomRange() {
  const fromEl = document.getElementById('rd-custom-from');
  const toEl   = document.getElementById('rd-custom-to');
  if (!fromEl.value || !toEl.value) { toast('Select both from and to', 'warn'); return; }
  rdState.timeFrom   = Math.floor(new Date(fromEl.value).getTime() / 1000);
  rdState.timeTo     = Math.floor(new Date(toEl.value).getTime()   / 1000);
  rdState.timeWindow = null;
  document.querySelectorAll('.tr-btn').forEach(b => b.classList.remove('active'));
  _rdRefreshLevel3();
}

function rdToggleAutoRefresh() {
  const on = document.getElementById('rd-autorefresh')?.checked;
  if (rdState._autoRefreshTimer) { clearInterval(rdState._autoRefreshTimer); rdState._autoRefreshTimer = null; }
  if (on) rdState._autoRefreshTimer = setInterval(_rdRefreshLevel3, 30000);
}

function _rdGetTimeRange() {
  const now = Math.floor(Date.now() / 1000);
  return {
    to:   rdState.timeTo   || now,
    from: rdState.timeFrom || (now - (rdState.timeWindow || 86400))
  };
}

async function _rdRefreshLevel3() {
  if (rdState.level !== 3 || !rdState.centerId || !rdState.nodeId) return;
  rdState.readings.page = 0;
  await Promise.all([
    fetchSensorSummary(),
    fetchNodeReadings(),
    renderDrillChart()
  ]);
}

// ── Readings L3 — sensor summary cards ───────────────────────────────────────

async function fetchSensorSummary() {
  const scEl = document.getElementById('sc-cards');
  if (!scEl) return;
  scEl.innerHTML = `<div style="color:var(--td);font-size:12px;padding:8px 0;grid-column:1/-1"><span class="spinner"></span> Loading…</div>`;

  const { from, to } = _rdGetTimeRange();
  const cfg = getMongoConfig();
  const params = new URLSearchParams({
    database:   cfg.db,
    collection: cfg.cols.readings || 'sensor_readings',
    from, to, buckets: 40
  });

  try {
    const res = await fetch(`/api/analytics/sensor-summary/${encodeURIComponent(rdState.centerId)}/${encodeURIComponent(rdState.nodeId)}?${params}`);
    if (!res.ok) throw new Error(await res.text());
    const { sensors, from: f, to: t } = await res.json();

    const label = document.getElementById('sc-time-label');
    if (label) label.textContent = fmtTime(f) + ' → ' + fmtTime(t);

    if (!sensors.length) {
      scEl.innerHTML = `<div style="color:var(--td);font-size:12px;padding:8px 0;grid-column:1/-1">No data in selected range</div>`;
      return;
    }

    // Build sensor toggle buttons for the chart
    const togglesEl = document.getElementById('rd-sensor-toggles');
    if (togglesEl) {
      // Init _activeSensors if new
      sensors.forEach(s => { if (rdState._activeSensors[s._id] === undefined) rdState._activeSensors[s._id] = true; });
      togglesEl.innerHTML = sensors.map(s =>
        `<button class="tr-btn ${rdState._activeSensors[s._id] ? 'active' : ''}" onclick="rdToggleSensor('${escH(s._id)}')" style="font-size:10px">${escH(s.name || s._id)}</button>`
      ).join('');
    }

    scEl.innerHTML = sensors.map(s => {
      const val   = typeof s.last  === 'number' ? s.last.toFixed(2)  : '—';
      const minV  = typeof s.min   === 'number' ? s.min.toFixed(2)   : '—';
      const maxV  = typeof s.max   === 'number' ? s.max.toFixed(2)   : '—';
      const avgV  = typeof s.avg   === 'number' ? s.avg.toFixed(2)   : '—';
      const spark = _sparklineSVG(s.series, 150, 32, s.lastAlert !== 'ok' ? '#dc2626' : '#2563eb');
      const cls   = s.lastAlert === 'ok' ? '' : (s.lastAlert === 'high' || s.lastAlert === 'gas' || s.lastAlert === 'active' ? 'sc-alert' : 'sc-warn');
      return `<div class="sc-card ${cls}">
        <div class="sc-name">${escH(s.name || s._id)}<span class="badge ${s.lastAlert === 'ok' ? 'b-ok' : (s.lastAlert === 'high' ? 'b-high' : 'b-gas')}" style="font-size:9px">${(s.lastAlert||'ok').toUpperCase()}</span></div>
        <div class="sc-val">${val}</div>
        <div class="sc-unit">${escH(s.unit || s.type || '')}</div>
        <div class="sc-stats">
          <div class="sc-stat"><div class="sc-stat-lbl">MIN</div><div class="sc-stat-val">${minV}</div></div>
          <div class="sc-stat"><div class="sc-stat-lbl">AVG</div><div class="sc-stat-val">${avgV}</div></div>
          <div class="sc-stat"><div class="sc-stat-lbl">MAX</div><div class="sc-stat-val">${maxV}</div></div>
        </div>
        <div class="sc-stat-lbl" style="font-size:9px;margin-bottom:3px">${s.count} readings</div>
        <div class="sc-spark">${spark}</div>
      </div>`;
    }).join('');

    // Store for chart use
    rdState._lastSensorSummary = sensors;

  } catch(e) {
    scEl.innerHTML = `<div style="color:var(--red);font-size:12px;grid-column:1/-1">${e.message}</div>`;
  }
}

function rdToggleSensor(sensorId) {
  rdState._activeSensors[sensorId] = !rdState._activeSensors[sensorId];
  // Update toggle button
  document.querySelectorAll('#rd-sensor-toggles .tr-btn').forEach(b => {
    if (b.onclick?.toString().includes(sensorId)) {
      b.classList.toggle('active', rdState._activeSensors[sensorId]);
    }
  });
  _renderMultiSensorChart(rdState._lastSensorSummary || []);
}

function _sparklineSVG(series, w = 120, h = 28, color = '#2563eb') {
  if (!series || series.length < 2) return `<svg width="${w}" height="${h}"></svg>`;
  const vals = series.map(p => p.value).filter(v => v != null && !isNaN(v));
  if (vals.length < 2) return `<svg width="${w}" height="${h}"></svg>`;
  const min = Math.min(...vals), max = Math.max(...vals), range = (max - min) || 1;
  const pts = vals.map((v, i) => {
    const x = ((i / (vals.length - 1)) * (w - 4) + 2).toFixed(1);
    const y = (h - 2 - ((v - min) / range) * (h - 4)).toFixed(1);
    return `${x},${y}`;
  }).join(' ');
  return `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" style="overflow:visible"><polyline points="${pts}" fill="none" stroke="${color}" stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round" opacity=".9"/></svg>`;
}

// ── Readings L3 — multi-sensor chart ─────────────────────────────────────────

const SENSOR_COLORS = ['#2563eb','#16a34a','#d97706','#dc2626','#7c3aed','#0284c7','#db2777','#059669'];

async function renderDrillChart() {
  const sensors = rdState._lastSensorSummary;
  if (sensors && sensors.length) {
    _renderMultiSensorChart(sensors);
    return;
  }
  // First load: fetch summary first
  await fetchSensorSummary();
  if (rdState._lastSensorSummary?.length) _renderMultiSensorChart(rdState._lastSensorSummary);
}

function _renderMultiSensorChart(sensors) {
  const ctx = document.getElementById('chart-readings')?.getContext('2d');
  if (!ctx) return;
  if (charts.readings) charts.readings.destroy();

  const activeSensors = sensors.filter(s => rdState._activeSensors[s._id] !== false);
  if (!activeSensors.length) return;

  // Build unified time labels from all series points
  const allTs = new Set();
  activeSensors.forEach(s => s.series.forEach(p => allTs.add(p.ts)));
  const sortedTs = [...allTs].sort((a, b) => a - b);
  const labels = sortedTs.map(ts => {
    const d = new Date(ts * 1000);
    const range = (rdState.timeTo || Math.floor(Date.now()/1000)) - (_rdGetTimeRange().from);
    return range > 86400 ? d.toLocaleDateString('en-GB',{month:'short',day:'numeric'}) : d.toLocaleTimeString('en-GB',{hour:'2-digit',minute:'2-digit'});
  });

  const datasets = activeSensors.map((s, i) => {
    const tsMap = {};
    s.series.forEach(p => { tsMap[p.ts] = p.value; });
    return {
      label: s.name || s._id,
      data: sortedTs.map(ts => tsMap[ts] ?? null),
      borderColor: SENSOR_COLORS[i % SENSOR_COLORS.length],
      backgroundColor: SENSOR_COLORS[i % SENSOR_COLORS.length] + '15',
      tension: 0.3, pointRadius: sortedTs.length > 50 ? 0 : 2,
      fill: false, spanGaps: true, borderWidth: 2
    };
  });

  charts.readings = new Chart(ctx, {
    type: 'line',
    data: { labels, datasets },
    options: {
      responsive: true, maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: true, position: 'top', labels: { font: { size: 11 }, padding: 12, usePointStyle: true } },
        tooltip: { callbacks: { label: ctx => ` ${ctx.dataset.label}: ${ctx.parsed.y?.toFixed(2) ?? '—'}` } }
      },
      scales: {
        y: { beginAtZero: false, grid: { color: 'rgba(0,0,0,.04)' } },
        x: { grid: { display: false }, ticks: { maxTicksLimit: 12, font: { size: 10 } } }
      }
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Reports
// ─────────────────────────────────────────────────────────────────────────────
async function generateReport(format) {
  const from   = document.getElementById('rpt-from').value;
  const to     = document.getElementById('rpt-to').value;
  const center = document.getElementById('rpt-center').value.trim();
  const filter = {};
  if (from) filter.occurredAt = { ...filter.occurredAt, $gte: Math.floor(new Date(from).getTime()/1000) };
  if (to)   filter.occurredAt = { ...filter.occurredAt, $lte: Math.floor(new Date(to).getTime()/1000) + 86400 };
  if (center) filter.centerId = center;
  try {
    const r = await mongo.find(col('acks'), filter, { sort: { occurredAt: -1 }, limit: 500 });
    const docs = r.documents || [];
    if (format === 'csv') {
      const rows = [['Time','Center','Node','Sensor','Alert Type','Value','Unit','Status','Acknowledged By','Note']];
      docs.forEach(a => rows.push([fmtTime(a.occurredAt),a.centerId||'',a.nodeHostname||'',a.sensorName||'',a.alertState||'',a.value||'',a.unit||'',a.status||'',a.acknowledgedBy||'',a.note||'']));
      downloadCSV(rows, 'isms_alert_report_' + (from||'all') + '.csv');
    } else {
      printAlertReport(docs, from, to);
    }
  } catch(e) { toast(e.message, 'err'); }
}

function printAlertReport(docs, from, to) {
  const w = window.open('', '_blank');
  const rows = docs.map(a => `<tr>
    <td>${fmtTime(a.occurredAt)}</td><td>${a.centerId||''}</td><td>${a.nodeHostname||''}</td>
    <td>${a.sensorName||''}</td><td style="color:${a.alertState==='high'?'#d97706':a.alertState==='gas'||a.alertState==='active'?'#dc2626':'#0284c7'}">${(a.alertState||'').toUpperCase()}</td>
    <td>${a.value||''} ${a.unit||''}</td><td>${a.status||''}</td><td>${a.acknowledgedBy||''}</td><td>${a.note||''}</td>
  </tr>`).join('');
  w.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8"><title>SIMEM Alert Report</title>
  <style>body{font-family:Arial,sans-serif;font-size:12px;margin:20px}h1{font-size:16px}table{width:100%;border-collapse:collapse}th,td{border:1px solid #ddd;padding:6px 8px;text-align:left}th{background:#f3f4f6;font-size:11px;text-transform:uppercase}tr:nth-child(even){background:#f9fafb}@media print{body{margin:0}}</style></head>
  <body><h1>SIMEM Alert Report</h1><p style="color:#666;font-size:11px">Period: ${from||'all'} to ${to||'present'} &nbsp;&nbsp; Generated: ${new Date().toLocaleString()} &nbsp;&nbsp; Records: ${docs.length}</p>
  <table><thead><tr><th>Time</th><th>Center</th><th>Node</th><th>Sensor</th><th>Alert</th><th>Value</th><th>Status</th><th>Ack By</th><th>Note</th></tr></thead><tbody>${rows}</tbody></table>
  <script>window.print();<\/script></body></html>`);
  w.document.close();
}

async function generateNodeReport() {
  const from = document.getElementById('rpt2-from').value;
  const to   = document.getElementById('rpt2-to').value;
  const filter = {};
  if (from || to) {
    filter.timestamp = {};
    if (from) filter.timestamp.$gte = Math.floor(new Date(from).getTime()/1000);
    if (to)   filter.timestamp.$lte = Math.floor(new Date(to).getTime()/1000) + 86400;
  }
  try {
    const r = await mongo.aggregate(col('readings'), [
      { $match: filter },
      { $unwind: '$nodes' },
      { $group: { _id: '$nodes.nodeId', hostname: { $first: '$nodes.hostname' }, centerId: { $first: '$centerId' }, records: { $sum: 1 }, lastSeen: { $max: '$timestamp' } } },
      { $sort: { records: -1 } }
    ]);
    const docs = r.documents || [];
    const rows = docs.map(n => `<tr><td>${n._id}</td><td>${n.hostname||''}</td><td>${n.centerId||''}</td><td>${n.records}</td><td>${fmtTime(n.lastSeen)}</td></tr>`).join('');
    const w = window.open('','_blank');
    w.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8"><title>SIMEM Node Report</title>
    <style>body{font-family:Arial,sans-serif;font-size:12px;margin:20px}h1{font-size:16px}table{width:100%;border-collapse:collapse}th,td{border:1px solid #ddd;padding:6px 8px;text-align:left}th{background:#f3f4f6}</style></head>
    <body><h1>SIMEM Node Activity Report</h1><p style="color:#666;font-size:11px">Period: ${from||'all'} to ${to||'present'} — Generated: ${new Date().toLocaleString()}</p>
    <table><thead><tr><th>Node ID</th><th>Hostname</th><th>Center</th><th>Records</th><th>Last Seen</th></tr></thead><tbody>${rows}</tbody></table>
    <script>window.print();<\/script></body></html>`);
    w.document.close();
  } catch(e) { toast(e.message, 'err'); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings
// ─────────────────────────────────────────────────────────────────────────────
function loadSettings() {
  const cfg = getMongoConfig();
  document.getElementById('cfg-database').value      = cfg.db || '';
  document.getElementById('cfg-col-readings').value  = cfg.cols.readings  || '';
  document.getElementById('cfg-col-acks').value      = cfg.cols.acks      || '';
  document.getElementById('cfg-col-nodes').value     = cfg.cols.nodes     || '';
  document.getElementById('cfg-col-incidents').value = cfg.cols.incidents || '';
}

function saveMongoSettings() {
  const db = document.getElementById('cfg-database').value.trim();
  const cR = document.getElementById('cfg-col-readings').value.trim();
  const cA = document.getElementById('cfg-col-acks').value.trim();
  const cN = document.getElementById('cfg-col-nodes').value.trim();
  const cI = document.getElementById('cfg-col-incidents').value.trim();
  if (db) localStorage.setItem('isms_mongo_db', db);
  if (cR) localStorage.setItem('isms_col_readings', cR);
  if (cA) localStorage.setItem('isms_col_acks', cA);
  if (cN) localStorage.setItem('isms_col_nodes', cN);
  if (cI) localStorage.setItem('isms_col_incidents', cI);
  toast('Settings saved', 'ok');
}

async function testConnection() {
  const status = document.getElementById('conn-status');
  status.textContent = 'Testing...';
  status.style.color = 'var(--ts)';
  try {
    const r = await mongo.find(getMongoConfig().cols.readings, {}, { limit: 1 });
    status.textContent = 'Connection successful — ' + (r.documents?.length ? '1 record found' : 'collection empty');
    status.style.color = 'var(--grn)';
    toast('Connected to MongoDB', 'ok');
  } catch(e) {
    status.textContent = 'Failed: ' + e.message;
    status.style.color = 'var(--red)';
    toast(e.message, 'err');
  }
}

async function saveAccount() {
  const name = document.getElementById('cfg-name').value.trim();
  const pass = document.getElementById('cfg-pass').value;
  const pass2 = document.getElementById('cfg-pass2').value;
  if (!firebase?.auth) { toast('Firebase not configured', 'warn'); return; }
  if (pass && pass !== pass2) { toast('Passwords do not match', 'warn'); return; }
  if (pass && pass.length < 6) { toast('Password must be at least 6 characters', 'warn'); return; }
  try {
    if (name) await firebase.auth().currentUser.updateProfile({ displayName: name });
    if (pass) await firebase.auth().currentUser.updatePassword(pass);
    toast('Account updated', 'ok');
  } catch(e) { toast(e.message, 'err'); }
}

// ─────────────────────────────────────────────────────────────────────────────
// CSV export helper
// ─────────────────────────────────────────────────────────────────────────────
function downloadCSV(rows, filename) {
  const csv = rows.map(r => r.map(c => '"' + String(c).replace(/"/g,'""') + '"').join(',')).join('\n');
  const a = document.createElement('a');
  a.href = 'data:text/csv;charset=utf-8,' + encodeURIComponent(csv);
  a.download = filename;
  a.click();
}

// ─────────────────────────────────────────────────────────────────────────────
// Device Registry
// ─────────────────────────────────────────────────────────────────────────────
function escH(s) {
  return String(s ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}

function getMasterKey() { return sessionStorage.getItem('isms_master_key') || ''; }

function setMasterKey() {
  const key = document.getElementById('dev-masterkey-input').value.trim();
  if (!key) return;
  sessionStorage.setItem('isms_master_key', key);
  document.getElementById('dev-keybar').style.display = 'none';
  loadDevices();
}

function devHeaders() {
  return { 'Content-Type': 'application/json', 'X-Master-Key': getMasterKey() };
}

async function loadDevices() {
  // Show the sync endpoint URL in the info bar
  document.getElementById('dev-sync-url').textContent = window.location.origin + '/api/sync';

  try {
    const res = await fetch('/api/devices', { headers: { 'X-Master-Key': getMasterKey() } });
    if (res.status === 401) {
      document.getElementById('dev-keybar').style.display = 'block';
      document.getElementById('dev-tbody').innerHTML =
        '<tr><td colspan="6" style="text-align:center;padding:24px;color:var(--ts)">Enter master key to view devices.</td></tr>';
      return;
    }
    document.getElementById('dev-keybar').style.display = 'none';
    const devices = await res.json();
    renderDevices(devices);
  } catch (e) {
    document.getElementById('dev-tbody').innerHTML =
      '<tr><td colspan="6" style="text-align:center;padding:24px;color:#dc2626">Failed to load devices: ' + e.message + '</td></tr>';
  }
}

function renderDevices(devices) {
  const cnt = document.getElementById('dev-count');
  if (cnt) cnt.textContent = '— ' + devices.length + ' device' + (devices.length !== 1 ? 's' : '');

  const tbody = document.getElementById('dev-tbody');
  if (!devices.length) {
    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:32px;color:var(--ts)">No devices registered yet. Click <strong>+ Register Device</strong> to add one.</td></tr>';
    return;
  }
  tbody.innerHTML = devices.map(d => {
    const statusDot = d.active
      ? '<span style="color:#16a34a;font-weight:600"><span style="font-size:1.2em;vertical-align:middle;margin-right:4px">●</span>Active</span>'
      : '<span style="color:var(--ts)"><span style="font-size:1.2em;vertical-align:middle;margin-right:4px">●</span>Inactive</span>';
    const loc = d.location ? [d.location.factory, d.location.building, d.location.room].filter(Boolean).join(' / ') : '—';
    return `<tr>
      <td><code style="font-size:12px">${escH(d.deviceCode)}</code></td>
      <td><strong>${escH(d.name)}</strong></td>
      <td style="font-size:12px;color:var(--ts)">${escH(loc)}</td>
      <td style="font-size:12px;color:var(--ts)">${fmtTime(d.createdAt)}</td>
      <td style="font-size:12px;color:var(--ts)">${d.lastSync ? fmtAgo(d.lastSync) + ' ago' : 'Never'}</td>
      <td style="font-size:12px">${statusDot}</td>
      <td>
        <button class="btn-ghost btn-sm" onclick="editDevice('${escH(d.deviceCode)}')">Edit</button>
        <button class="btn-ghost btn-sm" style="color:var(--red)" onclick="deleteDevice('${escH(d.deviceCode)}','${escH(d.name)}')">Del</button>
      </td>
    </tr>`;
  }).join('');
}

function openAddDevice() {
  document.getElementById('dev-edit-mode').value = "0";
  document.getElementById('modal-inc-title').textContent = 'Register Device'; // Using same modal title id if generic
  document.getElementById('dev-code').value    = '';
  document.getElementById('dev-code').disabled = false;
  document.getElementById('dev-gen-code-btn').style.display = 'inline-flex';
  document.getElementById('dev-name').value    = '';
  document.getElementById('dev-factory').value = '';
  document.getElementById('dev-building').value = '';
  document.getElementById('dev-room').value    = '';
  document.getElementById('dev-apikey').value  = '';
  document.getElementById('dev-key-lbl').textContent = 'API Key';
  document.getElementById('dev-key-hint').style.display = 'none';
  document.getElementById('dev-save-btn').textContent = 'Register Device';
  document.getElementById('dev-save-hint').style.display = 'none';
  document.getElementById('dev-save-btn').style.display  = 'inline-flex';
  openModal('modal-add-device');
}

async function editDevice(code) {
  try {
    const res = await fetch('/api/devices', { headers: { 'X-Master-Key': getMasterKey() } });
    const devices = await res.json();
    const d = devices.find(x => x.deviceCode === code);
    if (!d) return;

    document.getElementById('dev-edit-mode').value = "1";
    document.getElementById('dev-code').value = d.deviceCode;
    document.getElementById('dev-code').disabled = true;
    document.getElementById('dev-gen-code-btn').style.display = 'none';
    document.getElementById('dev-name').value = d.name || '';
    document.getElementById('dev-factory').value = d.location?.factory || '';
    document.getElementById('dev-building').value = d.location?.building || '';
    document.getElementById('dev-room').value = d.location?.room || '';
    document.getElementById('dev-apikey').value = '';
    document.getElementById('dev-key-lbl').textContent = 'New API Key (optional)';
    document.getElementById('dev-key-hint').style.display = 'block';
    document.getElementById('dev-save-btn').textContent = 'Update Device';
    document.getElementById('dev-save-hint').style.display = 'none';
    document.getElementById('dev-save-btn').style.display = 'inline-flex';
    openModal('modal-add-device');
  } catch (e) { toast(e.message, 'err'); }
}

function genDevCode() {
  document.getElementById('dev-code').value = 'CN-' + Math.random().toString(36).slice(2,6).toUpperCase();
}

function genDevApiKey() {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  document.getElementById('dev-apikey').value =
    Array.from(bytes).map(b => b.toString(16).padStart(2,'0')).join('');
}

async function submitAddDevice() {
  const isEdit     = document.getElementById('dev-edit-mode').value === "1";
  const deviceCode = document.getElementById('dev-code').value.trim();
  const name       = document.getElementById('dev-name').value.trim();
  const apiKey     = document.getElementById('dev-apikey').value.trim();
  const location   = {
    factory:  document.getElementById('dev-factory').value.trim(),
    building: document.getElementById('dev-building').value.trim(),
    room:     document.getElementById('dev-room').value.trim()
  };

  if (!deviceCode || !name) { toast('Code and Name are required', 'err'); return; }
  if (!isEdit && !apiKey) { toast('API Key is required for new registration', 'err'); return; }

  const method = isEdit ? 'PUT' : 'POST';
  const url    = isEdit ? `/api/devices/${encodeURIComponent(deviceCode)}` : '/api/devices';
  
  const body = { name, location };
  if (apiKey) {
    if (isEdit) body.newApiKey = apiKey;
    else body.apiKey = apiKey;
  }
  if (!isEdit) body.deviceCode = deviceCode;

  const res = await fetch(url, {
    method, headers: devHeaders(),
    body: JSON.stringify(body)
  });
  
  if (res.status === 401) { toast('Invalid master key', 'err'); return; }
  if (res.status === 409) { toast('Device code already exists', 'err'); return; }
  if (!res.ok) { const t = await res.text(); toast('Failed: ' + t, 'err'); return; }

  if (!isEdit && apiKey) {
    document.getElementById('dev-hint-url').textContent = window.location.origin + '/api/sync';
    document.getElementById('dev-hint-key').textContent = apiKey;
    document.getElementById('dev-save-hint').style.display = 'block';
    document.getElementById('dev-save-btn').style.display  = 'none';
  } else {
    closeModal('modal-add-device');
  }

  toast(isEdit ? 'Device updated' : 'Device registered', 'ok');
  loadDevices();
}

async function deleteDevice(code, name) {
  if (!confirm('Remove device "' + name + '" (' + code + ')?\nThis invalidates its API key and stops future syncs.')) return;
  const res = await fetch('/api/devices/' + encodeURIComponent(code), {
    method: 'DELETE', headers: { 'X-Master-Key': getMasterKey() }
  });
  if (!res.ok) { toast('Delete failed', 'err'); return; }
  toast('Device removed', 'ok');
  loadDevices();
}

function copyText(text) {
  navigator.clipboard.writeText(text).then(() => toast('Copied', 'ok'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Topology page
// ─────────────────────────────────────────────────────────────────────────────
async function loadTopology() {
  const el = document.getElementById('topo-grid');
  if (!el) return;
  el.innerHTML = `<div style="padding:40px;text-align:center;color:var(--ts)"><span class="spinner"></span> Loading topology…</div>`;

  try {
    const cfg = getMongoConfig();
    const params = new URLSearchParams({
      database:    cfg.db,
      readingsCol: cfg.cols.readings || 'sensor_readings',
      alertsCol:   cfg.cols.acks || 'alert_acks',
      incidentsCol: cfg.cols.incidents || 'incidents'
    });
    const res = await fetch(`/api/analytics/dashboard?${params}`);
    if (!res.ok) throw new Error(await res.text());
    const { recentReadings } = await res.json();

    // Build center → node → sensor map from latest readings
    const centers = {};
    const docsByCenter = {};
    [...(recentReadings || [])].sort((a, b) => a.timestamp - b.timestamp).forEach(doc => {
      if (!docsByCenter[doc.centerId]) docsByCenter[doc.centerId] = { ...doc, nodes: [] };
      const existing = docsByCenter[doc.centerId];
      (doc.nodes || []).forEach(node => {
        const en = existing.nodes.find(n => n.nodeId === node.nodeId);
        if (!en) existing.nodes.push({ ...node });
        else {
          // Merge latest readings
          en.online = node.online;
          (node.readings || []).forEach(r => {
            const er = en.readings?.find(x => x.sensorId === r.sensorId);
            if (!er) en.readings = [...(en.readings || []), r];
            else Object.assign(er, r);
          });
        }
      });
    });

    const centerIds = Object.keys(docsByCenter);
    if (!centerIds.length) {
      el.innerHTML = `<div class="empty"><div class="ico">📡</div><div class="lbl">No topology data</div><div class="sub">Data appears once center nodes sync to this server</div></div>`;
      return;
    }

    el.innerHTML = centerIds.map(cid => {
      const c = docsByCenter[cid];
      const loc = c.location ? [c.location.factory, c.location.building, c.location.room].filter(Boolean).join(' › ') : '';
      const nodes = c.nodes || [];
      // Determine worst state across all nodes
      let centerState = 'ok';
      nodes.forEach(n => (n.readings || []).forEach(r => {
        if (r.alertState === 'gas' || r.alertState === 'active') centerState = 'alert';
        else if (r.alertState === 'high' && centerState !== 'alert') centerState = 'warn';
      }));
      const stateColor = { ok: 'var(--grn)', warn: 'var(--amb)', alert: 'var(--red)' }[centerState] || 'var(--td)';

      const nodesHtml = nodes.map(node => {
        const readings = (node.readings || []).filter(r => r.type !== 'relay');
        let nodeState = 'ok';
        readings.forEach(r => {
          if (r.alertState === 'gas' || r.alertState === 'active') nodeState = 'alert';
          else if ((r.alertState === 'high' || r.alertState === 'low') && nodeState !== 'alert') nodeState = 'warn';
        });
        const nodeCls = { ok: '', warn: 'tn-warn', alert: 'tn-alert' }[nodeState] || '';

        const sensorsHtml = readings.map(r => {
          const sCls = { ok: 'ts-ok', high: 'ts-warn', low: 'ts-warn', gas: 'ts-alert', active: 'ts-alert' }[r.alertState] || 'ts-ok';
          const val = typeof r.value === 'number' ? r.value.toFixed(1) : '—';
          return `<div class="topo-sensor ${sCls}" title="${escH(r.name||r.sensorId)}: ${val} ${escH(r.unit||'')}">
            <span class="topo-sensor-name">${escH(r.name || r.type || r.sensorId)}</span>
            <span class="topo-sensor-val">${val}${r.unit ? ' '+escH(r.unit) : ''}</span>
          </div>`;
        }).join('');

        return `<div class="topo-node ${nodeCls}">
          <div class="topo-node-hd">
            <div>
              <div class="topo-node-name">${escH(node.hostname || node.nodeId)}</div>
              <div class="topo-node-id">${escH(node.nodeId)}</div>
            </div>
            <div style="display:flex;align-items:center;gap:6px">
              ${node.online ? '<span class="badge b-online" style="font-size:9px">ONLINE</span>' : '<span class="badge b-offline" style="font-size:9px">OFFLINE</span>'}
              <span class="topo-drill" onclick="rdDrillFromTopo('${escH(cid)}','${escH(node.nodeId)}','${escH(node.hostname||node.nodeId)}')" title="View readings">→</span>
            </div>
          </div>
          <div class="topo-sensors">${sensorsHtml || '<span style="font-size:11px;color:var(--td)">No sensor data</span>'}</div>
        </div>`;
      }).join('');

      return `<div class="topo-center">
        <div class="topo-center-hd">
          <div style="width:10px;height:10px;border-radius:50%;background:${stateColor};flex-shrink:0"></div>
          <div>
            <div class="topo-center-name">${escH(cid)}</div>
            ${loc ? `<div class="topo-center-loc">${escH(loc)}</div>` : ''}
          </div>
          <div style="margin-left:auto;display:flex;gap:8px;align-items:center">
            <span style="font-size:11px;color:var(--ts)">${nodes.length} node${nodes.length!==1?'s':''}</span>
            <button class="btn btn-out btn-sm" onclick="rdDrillFromTopo('${escH(cid)}',null,null)">Browse →</button>
          </div>
        </div>
        <div class="topo-nodes">${nodesHtml || '<div style="padding:16px;color:var(--td);font-size:12px">No nodes found</div>'}</div>
      </div>`;
    }).join('');

  } catch(e) {
    el.innerHTML = `<div style="padding:40px;text-align:center;color:var(--red)">${e.message}</div>`;
  }
}

function rdDrillFromTopo(centerId, nodeId, hostname) {
  navTo('readings');
  setTimeout(async () => {
    await rdDrillCenter(centerId);
    if (nodeId) setTimeout(() => rdDrillNode(nodeId, hostname), 300);
  }, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Boot
// ─────────────────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  initTheme();
  initFirebase();
  initSocket();
});
