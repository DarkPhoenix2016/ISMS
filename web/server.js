// ─────────────────────────────────────────────────────────────────────────────
// ISMS Web Portal — Local server
// Reads credentials from .env, serves static files, and proxies all DB calls
// to a local MongoDB instance (no credentials ever reach the browser).
//
// Usage:
//   npm install
//   cp .env.example .env   # fill in your values
//   npm start
// ─────────────────────────────────────────────────────────────────────────────

require('dotenv').config();

const express    = require('express');
const path       = require('path');
const crypto     = require('crypto');
const { MongoClient } = require('mongodb');
const http = require('http');
const { Server: SocketServer } = require('socket.io');

const app  = express();
const httpServer = http.createServer(app);
const io = new SocketServer(httpServer, { cors: { origin: '*', methods: ['GET','POST'] } });
const PORT = parseInt(process.env.PORT || '3000', 10);

const MONGO_URI  = process.env.MONGODB_URI  || 'mongodb://localhost:27017';
const MONGO_DB   = process.env.MONGODB_DB   || 'isms_db';
const MASTER_KEY = process.env.MASTER_KEY   || '';   // protects /api/devices
if (!MASTER_KEY) console.warn('[WARN] MASTER_KEY not set — /api/devices is unprotected');

function sha256(str) {
  return crypto.createHash('sha256').update(str).digest('hex');
}

// ── Auth: device management (master key) ─────────────────────────────────────
function requireMasterKey(req, res, next) {
  if (!MASTER_KEY) return next();   // dev mode: no key set → open
  const key = req.headers['x-master-key'];
  if (key !== MASTER_KEY) return res.status(401).json({ error: 'Invalid master key' });
  next();
}

// ── Auth: sync endpoint (per-device API key) ──────────────────────────────────
async function requireDeviceKey(req, res, next) {
  const apiKey = req.headers['x-api-key'] || req.headers['api-key'];
  if (!apiKey) return res.status(401).json({ error: 'X-API-Key header required' });
  try {
    const db     = await getDb(MONGO_DB);
    const device = await db.collection('devices').findOne({
      apiKeyHash: sha256(apiKey),
      active: true
    });
    if (!device) return res.status(403).json({ error: 'Invalid or inactive API key' });
    req.device = device;
    next();
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
}

app.use(express.json({ limit: '1mb' }));
app.use(express.static(__dirname));       // serves index.html, etc.

// ── MongoDB connection (lazy, shared) ─────────────────────────────────────────

let _client = null;

async function getDb(dbName) {
  if (!_client) {
    _client = new MongoClient(MONGO_URI, { serverSelectionTimeoutMS: 5000 });
    await _client.connect();
    console.log(`[DB] Connected to ${MONGO_URI}`);
  }
  return _client.db(dbName);
}

// ── Firebase config endpoint ───────────────────────────────────────────────────
// Serves Firebase project credentials from .env so they never need to live
// in client-side source files that could be committed to a repo.

app.get('/api/firebase-config', (_req, res) => {
  const apiKey = process.env.FIREBASE_API_KEY;
  if (!apiKey) return res.json(null);   // null → browser enters dev/offline mode
  res.json({
    apiKey,
    authDomain:        process.env.FIREBASE_AUTH_DOMAIN        || '',
    projectId:         process.env.FIREBASE_PROJECT_ID         || '',
    storageBucket:     process.env.FIREBASE_STORAGE_BUCKET     || '',
    messagingSenderId: process.env.FIREBASE_MESSAGING_SENDER_ID || '',
    appId:             process.env.FIREBASE_APP_ID             || ''
  });
});

// ── MongoDB proxy ─────────────────────────────────────────────────────────────
// Accepts the same action names as MongoDB Atlas Data API so the browser code
// needs no changes if you later switch to Atlas.
//
// POST /api/db/:action   body: { database?, collection, filter?, document?,
//                                update?, pipeline?, sort?, limit?, skip? }

app.post('/api/db/:action', async (req, res) => {
  const { action } = req.params;
  const {
    database, collection,
    filter, document, update, pipeline,
    sort, limit, skip, projection
  } = req.body;

  if (!collection) return res.status(400).json({ error: 'collection is required' });

  const dbName = database || MONGO_DB;

  try {
    const db   = await getDb(dbName);
    const coll = db.collection(collection);

    switch (action) {

      case 'find': {
        const opts = {};
        if (sort)       opts.sort       = sort;
        if (limit)      opts.limit      = Number(limit);
        if (skip)       opts.skip       = Number(skip);
        if (projection) opts.projection = projection;
        const docs = await coll.find(filter || {}, opts).toArray();
        return res.json({ documents: docs });
      }

      case 'findOne': {
        const doc = await coll.findOne(filter || {});
        return res.json({ document: doc });
      }

      case 'insertOne': {
        if (!document) return res.status(400).json({ error: 'document is required' });
        const result = await coll.insertOne(document);
        return res.json({ insertedId: result.insertedId });
      }

      case 'updateOne': {
        if (!update) return res.status(400).json({ error: 'update is required' });
        const result = await coll.updateOne(filter || {}, update);
        return res.json({ matchedCount: result.matchedCount, modifiedCount: result.modifiedCount });
      }

      case 'deleteOne': {
        const result = await coll.deleteOne(filter || {});
        return res.json({ deletedCount: result.deletedCount });
      }

      case 'aggregate': {
        if (!Array.isArray(pipeline)) return res.status(400).json({ error: 'pipeline array is required' });
        const docs = await coll.aggregate(pipeline).toArray();
        return res.json({ documents: docs });
      }

      default:
        return res.status(400).json({ error: `Unknown action: ${action}` });
    }

  } catch (err) {
    console.error(`[DB] ${action} on ${collection} failed:`, err.message);
    return res.status(500).json({ error: err.message });
  }
});

// ── Device registry ───────────────────────────────────────────────────────────
// All endpoints protected by X-Master-Key header (value from .env MASTER_KEY).

// GET /api/devices  — list registered devices (apiKeyHash never returned)
app.get('/api/devices', requireMasterKey, async (_req, res) => {
  try {
    const db      = await getDb(MONGO_DB);
    const devices = await db.collection('devices')
      .find({}, { projection: { apiKeyHash: 0 } })
      .sort({ createdAt: -1 })
      .toArray();
    res.json(devices);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/devices  — register a new device
// Body: { deviceCode, name, apiKey }
app.post('/api/devices', requireMasterKey, async (req, res) => {
  const { deviceCode, name, apiKey } = req.body || {};
  if (!deviceCode || !name || !apiKey)
    return res.status(400).json({ error: 'deviceCode, name, and apiKey are required' });
  try {
    const db = await getDb(MONGO_DB);
    if (await db.collection('devices').findOne({ deviceCode }))
      return res.status(409).json({ error: 'Device code already exists' });

    await db.collection('devices').insertOne({
      deviceCode,
      name,
      apiKeyHash: sha256(apiKey),   // only the hash is stored
      createdAt:  Math.floor(Date.now() / 1000),
      lastSync:   null,
      active:     true
    });
    console.log(`[DEVICES] Registered: ${name} (${deviceCode})`);
    res.json({ ok: true });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// DELETE /api/devices/:code  — remove a device (invalidates its API key)
app.delete('/api/devices/:code', requireMasterKey, async (req, res) => {
  try {
    const db     = await getDb(MONGO_DB);
    const result = await db.collection('devices').deleteOne({ deviceCode: req.params.code });
    if (result.deletedCount === 0) return res.status(404).json({ error: 'Device not found' });
    console.log(`[DEVICES] Removed: ${req.params.code}`);
    res.json({ ok: true });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// ── Sync endpoint ─────────────────────────────────────────────────────────────
// Center nodes POST here instead of directly to MongoDB.
// Accepts the same payload format db_sync.cpp already produces:
//   { database?, collection?, document: { timestamp, centerId, location, nodes[] } }
// Auth: X-API-Key (or api-key for backwards compat) must match a registered device.

app.post('/api/sync', requireDeviceKey, async (req, res) => {
  const { database, collection, document } = req.body || {};
  if (!document) return res.status(400).json({ error: 'document is required' });

  const dbName  = database   || MONGO_DB;
  const colName = collection || 'sensor_readings';

  // Reject records with invalid timestamps (NTP not yet synced on the node)
  const ts = document.timestamp || 0;
  if (ts < 1000000) return res.status(400).json({ error: 'Invalid timestamp — NTP not synced' });

  try {
    const db = await getDb(dbName);

    // Tag every record with the device that sent it
    document._deviceCode = req.device.deviceCode;
    document._deviceName = req.device.name;

    await db.collection(colName).insertOne(document);
    io.emit('sync:new', { centerId: document.centerId, timestamp: document.timestamp });

    // Keep device last-sync time up to date
    await db.collection('devices').updateOne(
      { deviceCode: req.device.deviceCode },
      { $set: { lastSync: Math.floor(Date.now() / 1000) } }
    );

    console.log(`[SYNC] ${req.device.name} (${req.device.deviceCode}) → ${dbName}.${colName}`);
    res.json({ ok: true });
  } catch (err) {
    console.error('[SYNC] Failed:', err.message);
    res.status(500).json({ error: err.message });
  }
});

// ── Analytics endpoints ───────────────────────────────────────────────────────
// Server-side aggregation so the browser never needs to fetch full collections.
// All three endpoints accept ?database=&collection= to honour the portal config.

// GET /api/analytics/centers
// Returns all distinct center nodes with last-sync time, record count, node count.
app.get('/api/analytics/centers', async (req, res) => {
  const dbName  = req.query.database   || MONGO_DB;
  const colName = req.query.collection || 'sensor_readings';
  try {
    const db = await getDb(dbName);
    const pipeline = [
      { $sort: { timestamp: -1 } },
      { $group: {
        _id:         '$centerId',
        lastSync:    { $first: '$timestamp' },
        recordCount: { $sum: 1 },
        location:    { $first: '$location' },
        nodeCount:   { $first: { $size: { $ifNull: ['$nodes', []] } } }
      }},
      { $project: { _id: 0, centerId: '$_id', lastSync: 1, recordCount: 1, location: 1, nodeCount: 1 } },
      { $sort: { centerId: 1 } }
    ];
    const centers = await db.collection(colName).aggregate(pipeline).toArray();
    res.json({ centers });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/analytics/center/:centerId
// Returns center metadata plus a de-duped list of its sensor nodes.
app.get('/api/analytics/center/:centerId', async (req, res) => {
  const dbName  = req.query.database   || MONGO_DB;
  const colName = req.query.collection || 'sensor_readings';
  const { centerId } = req.params;
  try {
    const db = await getDb(dbName);

    const latest = await db.collection(colName)
      .findOne({ centerId }, { sort: { timestamp: -1 }, projection: { 'nodes.readings': 0 } });
    if (!latest) return res.status(404).json({ error: 'Center not found' });

    const [countRes, nodeDocs] = await Promise.all([
      db.collection(colName).countDocuments({ centerId }),
      db.collection(colName).aggregate([
        { $match: { centerId } },
        { $sort: { timestamp: -1 } },
        { $limit: 500 },
        { $unwind: '$nodes' },
        { $group: {
          _id:         '$nodes.nodeId',
          hostname:    { $first: '$nodes.hostname' },
          lastSeen:    { $first: '$timestamp' },
          online:      { $first: '$nodes.online' },
          sensorCount: { $first: { $size: { $ifNull: ['$nodes.readings', []] } } }
        }},
        { $project: { _id: 0, nodeId: '$_id', hostname: 1, lastSeen: 1, online: 1, sensorCount: 1 } },
        { $sort: { hostname: 1 } }
      ]).toArray()
    ]);

    res.json({
      centerId,
      location:    latest.location || null,
      lastSync:    latest.timestamp,
      recordCount: countRes,
      nodes:       nodeDocs
    });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/analytics/node/:centerId/:nodeId
// Returns paginated, flat sensor readings for one node.
// Query params: page, limit, type, from (epoch), to (epoch)
app.get('/api/analytics/node/:centerId/:nodeId', async (req, res) => {
  const dbName  = req.query.database   || MONGO_DB;
  const colName = req.query.collection || 'sensor_readings';
  const { centerId, nodeId } = req.params;
  const page  = Math.max(0, parseInt(req.query.page  || '0', 10));
  const limit = Math.min(100, Math.max(1, parseInt(req.query.limit || '25', 10)));
  const type  = req.query.type || '';
  const from  = req.query.from ? parseInt(req.query.from, 10) : null;
  const to    = req.query.to   ? parseInt(req.query.to,   10) : null;

  try {
    const db = await getDb(dbName);

    const docMatch = { centerId };
    if (from || to) {
      docMatch.timestamp = {};
      if (from) docMatch.timestamp.$gte = from;
      if (to)   docMatch.timestamp.$lte = to;
    }
    const readingMatch = type ? { type } : { type: { $ne: 'relay' } };

    const basePipeline = [
      { $match: docMatch },
      { $sort: { timestamp: -1 } },
      { $unwind: '$nodes' },
      { $match: { 'nodes.nodeId': nodeId } },
      { $unwind: '$nodes.readings' },
      { $match: Object.fromEntries(Object.entries(readingMatch).map(([k,v]) => [`nodes.readings.${k}`, v])) },
      { $replaceRoot: { newRoot: { $mergeObjects: [
        '$nodes.readings',
        { timestamp: '$timestamp', centerId: '$centerId',
          nodeId: '$nodes.nodeId', hostname: '$nodes.hostname' }
      ]}}},
    ];

    const [countRes, readings] = await Promise.all([
      db.collection(colName).aggregate([...basePipeline, { $count: 'n' }]).toArray(),
      db.collection(colName).aggregate([
        ...basePipeline,
        { $skip: page * limit },
        { $limit: limit }
      ]).toArray()
    ]);

    res.json({ readings, total: countRes[0]?.n || 0, page, limit });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/analytics/dashboard
// Returns all dashboard stats, alert trends, alert breakdown, recent readings,
// and live alerts in one parallel call.
app.get('/api/analytics/dashboard', async (req, res) => {
  const dbName      = req.query.database     || MONGO_DB;
  const readingsCol = req.query.readingsCol  || 'sensor_readings';
  const alertsCol   = req.query.alertsCol    || 'alert_acks';
  const incidentsCol= req.query.incidentsCol || 'incidents';
  try {
    const db  = await getDb(dbName);
    const now = Math.floor(Date.now() / 1000);
    const day = now - 86400;

    const [
      records24hRes,
      activeNodesRes,
      activeCentersRes,
      activeAlertsRes,
      openIncidentsCount,
      syncRateRes,
      alertsByDayArr,
      alertByTypeArr,
      recentReadings,
      liveAlerts
    ] = await Promise.all([
      // records in last 24h
      db.collection(readingsCol).aggregate([
        { $match: { timestamp: { $gte: day } } },
        { $count: 'n' }
      ]).toArray(),

      // unique active nodes in last 24h
      db.collection(readingsCol).aggregate([
        { $match: { timestamp: { $gte: day } } },
        { $unwind: '$nodes' },
        { $group: { _id: '$nodes.nodeId' } },
        { $count: 'n' }
      ]).toArray(),

      // unique active centers in last 24h
      db.collection(readingsCol).aggregate([
        { $match: { timestamp: { $gte: day } } },
        { $group: { _id: '$centerId' } },
        { $count: 'n' }
      ]).toArray(),

      // active alerts (pending or acked)
      db.collection(alertsCol).aggregate([
        { $match: { status: { $in: ['pending', 'acked'] } } },
        { $count: 'n' }
      ]).toArray(),

      // open incidents (graceful if collection missing)
      db.collection(incidentsCol).aggregate([
        { $match: { status: { $in: ['open', 'investigating'] } } },
        { $count: 'n' }
      ]).toArray().catch(() => []),

      // sync rate: records in last hour
      db.collection(readingsCol).aggregate([
        { $match: { timestamp: { $gte: now - 3600 } } },
        { $count: 'n' }
      ]).toArray(),

      // alerts by day — last 7 days (newest first, reversed before returning)
      (async () => {
        const results = [];
        for (let i = 0; i < 7; i++) {
          const dayStart = now - (i + 1) * 86400;
          const dayEnd   = now - i * 86400;
          const r = await db.collection(alertsCol).aggregate([
            { $match: { occurredAt: { $gte: dayStart, $lt: dayEnd } } },
            { $count: 'n' }
          ]).toArray().catch(() => []);
          results.push(r[0]?.n || 0);
        }
        return results.reverse(); // oldest → newest
      })(),

      // alert breakdown by type
      db.collection(alertsCol).aggregate([
        { $group: { _id: '$alertState', count: { $sum: 1 } } },
        { $sort: { count: -1 } }
      ]).toArray(),

      // recent readings (last 10 docs)
      db.collection(readingsCol)
        .find({}, { sort: { timestamp: -1 }, limit: 10 })
        .toArray(),

      // live alerts (top 8 pending/acked)
      db.collection(alertsCol)
        .find({ status: { $in: ['pending', 'acked'] } }, { sort: { occurredAt: -1 }, limit: 8 })
        .toArray()
    ]);

    res.json({
      stats: {
        records24h:    records24hRes[0]?.n    || 0,
        activeNodes:   activeNodesRes[0]?.n   || 0,
        activeCenters: activeCentersRes[0]?.n || 0,
        activeAlerts:  activeAlertsRes[0]?.n  || 0,
        openIncidents: openIncidentsCount[0]?.n || 0,
        syncRatePerHour: syncRateRes[0]?.n    || 0
      },
      alertsByDay:    alertsByDayArr,
      alertByType:    alertByTypeArr,
      recentReadings: recentReadings,
      liveAlerts:     liveAlerts
    });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/analytics/sensor-summary/:centerId/:nodeId
// Returns per-sensor stats (min/max/avg/last/count/lastAlert) plus bucketed
// time-series for the selected time window.
app.get('/api/analytics/sensor-summary/:centerId/:nodeId', async (req, res) => {
  const dbName  = req.query.database   || MONGO_DB;
  const colName = req.query.collection || 'sensor_readings';
  const { centerId, nodeId } = req.params;
  const now     = Math.floor(Date.now() / 1000);
  const from    = req.query.from ? parseInt(req.query.from, 10) : now - 86400;
  const to      = req.query.to   ? parseInt(req.query.to,   10) : now;
  const buckets = Math.min(200, Math.max(10, parseInt(req.query.buckets || '50', 10)));
  const bucketSize = Math.max(1, Math.floor((to - from) / buckets));

  try {
    const db = await getDb(dbName);

    const matchStage = {
      centerId,
      timestamp: { $gte: from, $lte: to }
    };

    const [statsArr, seriesArr] = await Promise.all([
      // Per-sensor stats aggregation
      db.collection(colName).aggregate([
        { $match: matchStage },
        { $unwind: '$nodes' },
        { $match: { 'nodes.nodeId': nodeId } },
        { $unwind: '$nodes.readings' },
        { $match: { 'nodes.readings.type': { $ne: 'relay' } } },
        { $group: {
          _id:       '$nodes.readings.sensorId',
          name:      { $last:  '$nodes.readings.name' },
          type:      { $last:  '$nodes.readings.type' },
          unit:      { $last:  '$nodes.readings.unit' },
          min:       { $min:   '$nodes.readings.value' },
          max:       { $max:   '$nodes.readings.value' },
          avg:       { $avg:   '$nodes.readings.value' },
          last:      { $last:  '$nodes.readings.value' },
          lastTs:    { $last:  '$nodes.readings.timestamp' },
          lastAlert: { $last:  '$nodes.readings.alertState' },
          count:     { $sum:   1 }
        }},
        { $sort: { name: 1 } }
      ]).toArray(),

      // Bucketed time-series aggregation
      db.collection(colName).aggregate([
        { $match: matchStage },
        { $unwind: '$nodes' },
        { $match: { 'nodes.nodeId': nodeId } },
        { $unwind: '$nodes.readings' },
        { $match: { 'nodes.readings.type': { $ne: 'relay' } } },
        { $group: {
          _id: {
            sensorId: '$nodes.readings.sensorId',
            bucket:   { $floor: { $divide: [{ $subtract: ['$timestamp', from] }, bucketSize] } }
          },
          avgValue: { $avg: '$nodes.readings.value' }
        }},
        { $sort: { '_id.sensorId': 1, '_id.bucket': 1 } }
      ]).toArray()
    ]);

    // Index series by sensorId
    const seriesBySensor = {};
    seriesArr.forEach(pt => {
      const sid = pt._id.sensorId;
      if (!seriesBySensor[sid]) seriesBySensor[sid] = [];
      seriesBySensor[sid].push({
        ts:    from + pt._id.bucket * bucketSize,
        value: pt.avgValue
      });
    });

    const sensors = statsArr.map(s => ({
      _id:       s._id,
      sensorId:  s._id,
      name:      s.name,
      type:      s.type,
      unit:      s.unit,
      min:       s.min,
      max:       s.max,
      avg:       s.avg,
      last:      s.last,
      lastTs:    s.lastTs,
      lastAlert: s.lastAlert || 'ok',
      count:     s.count,
      series:    seriesBySensor[s._id] || []
    }));

    res.json({ sensors, from, to, bucketSize });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// ── Catch-all → index.html (SPA) ─────────────────────────────────────────────
app.get('*', (_req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// ── Background: alert sync ────────────────────────────────────────────────────
// Runs every ALERT_INTERVAL ms. Reads the most-recent sensor_readings document
// per centerId, then:
//   • alertState != 'ok'  → upsert a 'pending' record into alert_acks
//   • alertState == 'ok'  → auto-resolve any pending alert_acks for that sensor
//
// This lets device-level alertState flags appear in the portal's Alerts page
// without any changes to center-node firmware.

const READINGS_COL   = process.env.READINGS_COL   || 'sensor_readings';
const ALERTS_COL     = process.env.ALERTS_COL     || 'alert_acks';
const ALERT_INTERVAL = parseInt(process.env.ALERT_INTERVAL || '30000', 10); // ms

async function syncAlertsFromReadings() {
  let db;
  try { db = await getDb(MONGO_DB); } catch { return; }   // skip if DB not ready yet

  try {
    // Latest sensor_readings document per centerId
    const latest = await db.collection(READINGS_COL).aggregate([
      { $sort: { timestamp: -1 } },
      { $group: { _id: '$centerId', doc: { $first: '$$ROOT' } } },
      { $replaceRoot: { newRoot: '$doc' } }
    ]).toArray();

    let created = 0, resolved = 0;

    for (const doc of latest) {
      for (const node of (doc.nodes || [])) {
        for (const r of (node.readings || [])) {
          if (r.type === 'relay') continue;

          const key = { centerId: doc.centerId, nodeId: node.nodeId, sensorId: r.sensorId };
          const isAlert = r.alertState && r.alertState !== 'ok';

          if (isAlert) {
            // Upsert: open a new alert only if no active one exists for this sensor
            const existing = await db.collection(ALERTS_COL).findOne(
              { ...key, status: { $in: ['pending', 'acked'] } }
            );
            if (existing) {
              // Update live fields so the portal always shows the latest value
              await db.collection(ALERTS_COL).updateOne(
                { _id: existing._id },
                { $set: {
                    sensorName: r.name || r.sensorId,
                    alertState: r.alertState,
                    value:      r.value,
                    unit:       r.unit  || '',
                    lastSeen:   doc.timestamp
                }}
              );
            } else {
              await db.collection(ALERTS_COL).insertOne({
                occurredAt:    r.timestamp || doc.timestamp,
                centerId:      doc.centerId,
                nodeHostname:  node.hostname,
                nodeId:        node.nodeId,
                sensorName:    r.name || r.sensorId,
                sensorId:      r.sensorId,
                alertState:    r.alertState,
                value:         r.value,
                unit:          r.unit  || '',
                status:        'pending',
                acknowledgedBy: null,
                note:          '',
                lastSeen:      doc.timestamp
              });
              created++;
              io.emit('alert:new', {
                sensorName: r.name || r.sensorId,
                nodeHostname: node.hostname,
                centerId: doc.centerId,
                alertState: r.alertState,
                value: r.value
              });
            }
          } else {
            // Sensor is back to OK — auto-resolve any open alerts
            const result = await db.collection(ALERTS_COL).updateMany(
              { ...key, status: 'pending' },
              { $set: { status: 'resolved', resolvedAt: Math.floor(Date.now() / 1000) } }
            );
            resolved += result.modifiedCount;
            if (result.modifiedCount > 0)
              io.emit('alert:resolved', { centerId: doc.centerId, nodeId: node.nodeId });
          }
        }
      }
    }

    if (created || resolved)
      console.log(`[ALERTS] sync: +${created} new, ${resolved} auto-resolved`);

  } catch (err) {
    console.error('[ALERTS] sync error:', err.message);
  }
}

// ── Start ─────────────────────────────────────────────────────────────────────
httpServer.listen(PORT, () => {
  console.log(`SIMEM Web Portal  →  http://localhost:${PORT}`);
  console.log(`MongoDB          →  ${MONGO_URI} / ${MONGO_DB}`);

  // Start alert sync after a short delay (let DB connection warm up)
  setTimeout(() => {
    syncAlertsFromReadings();
    setInterval(syncAlertsFromReadings, ALERT_INTERVAL);
    console.log(`[ALERTS] sync running every ${ALERT_INTERVAL / 1000}s`);
  }, 5000);
});
