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
const { MongoClient } = require('mongodb');

const app  = express();
const PORT = parseInt(process.env.PORT || '3000', 10);

const MONGO_URI = process.env.MONGODB_URI || 'mongodb://localhost:27017';
const MONGO_DB  = process.env.MONGODB_DB  || 'isms_db';

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

// ── Catch-all → index.html (SPA) ─────────────────────────────────────────────
app.get('*', (_req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// ── Start ─────────────────────────────────────────────────────────────────────
app.listen(PORT, () => {
  console.log(`ISMS Web Portal  →  http://localhost:${PORT}`);
  console.log(`MongoDB          →  ${MONGO_URI} / ${MONGO_DB}`);
});
