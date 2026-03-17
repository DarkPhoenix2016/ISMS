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

const app  = express();
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

// ── Catch-all → index.html (SPA) ─────────────────────────────────────────────
app.get('*', (_req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// ── Start ─────────────────────────────────────────────────────────────────────
app.listen(PORT, () => {
  console.log(`ISMS Web Portal  →  http://localhost:${PORT}`);
  console.log(`MongoDB          →  ${MONGO_URI} / ${MONGO_DB}`);
});
