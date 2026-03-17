#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "db_sync.h"
#include "config.h"
#include "node_manager.h"   // ntpSynced()

bool     g_syncLastOk   = false;
uint32_t g_syncLastTime = 0;

static unsigned long s_lastSync      = 0;
static uint32_t      s_lastSyncHash  = 0;  // I8: delta detection

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint32_t nowEpoch()
{
    time_t t = time(nullptr);
    return (t > 1000000UL) ? (uint32_t)t : (uint32_t)(millis() / 1000UL);
}

// I8: cheap hash over all current readings and alert states.
// Sync is skipped when the hash matches the previous sync AND no alerts are active,
// preventing the DB filling with identical records on quiet intervals.
// Any active alert always forces a sync regardless of hash.
static uint32_t computeReadingsHash()
{
    uint32_t h = 2166136261UL;  // FNV-1a offset basis
    bool hasAlert = false;
    for (const auto& node : discoveredNodes) {
        for (const auto& r : node.lastReadings) {
            if (r.type == "relay") continue;
            // Mix value (scaled to int) and alert state length into hash
            uint32_t v = (uint32_t)(r.value * 100.0f);
            h ^= v;  h *= 16777619UL;
            h ^= (uint32_t)r.alertState.length();  h *= 16777619UL;
            if (r.alertState != "ok") hasAlert = true;
        }
    }
    // Encode hasAlert in MSB so an alert always produces a different hash
    if (hasAlert) h |= 0x80000000UL;
    return h;
}

// ── Sync ──────────────────────────────────────────────────────────────────────

static void syncToMongo()
{
    // I5: never insert records with millis()-derived timestamps (would appear
    //     at epoch 0 / 1970 in every report and chart).
    if (!ntpSynced()) {
        Serial.println("[DB] Skipping sync — NTP not yet synced");
        return;
    }

    if (systemConfig.mongoUrl.isEmpty()) {
        g_syncLastOk = false;
        return;
    }

    // I8: skip if readings unchanged and no active alerts
    uint32_t hash = computeReadingsHash();
    bool hasAlert = (hash & 0x80000000UL) != 0;
    if (hash == s_lastSyncHash && !hasAlert && g_syncLastOk) {
        Serial.println("[DB] Skipping sync — no change in readings");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.begin(client, systemConfig.mongoUrl);
    http.addHeader("Content-Type", "application/json");
    if (!systemConfig.mongoApiKey.isEmpty()) {
        http.addHeader("api-key", systemConfig.mongoApiKey);
    }

    JsonDocument doc;
    doc["database"]   = systemConfig.mongoDatabase;
    doc["collection"] = systemConfig.mongoCollection;

    JsonObject document = doc["document"].to<JsonObject>();
    document["timestamp"] = nowEpoch();
    document["centerId"]  = systemConfig.hostname;

    JsonObject loc = document["location"].to<JsonObject>();
    loc["factory"]  = systemConfig.factory;
    loc["building"] = systemConfig.building;
    loc["room"]     = systemConfig.room;

    JsonArray nodesArr = document["nodes"].to<JsonArray>();
    for (const auto& node : discoveredNodes) {
        JsonObject nObj = nodesArr.add<JsonObject>();
        nObj["nodeId"]   = node.id;
        nObj["hostname"] = node.hostname;
        nObj["online"]   = node.online;

        JsonArray readings = nObj["readings"].to<JsonArray>();
        for (const auto& r : node.lastReadings) {
            if (r.type == "relay") continue;
            JsonObject rObj = readings.add<JsonObject>();
            rObj["sensorId"]   = r.sensorId;
            rObj["name"]       = r.name;
            rObj["type"]       = r.type;
            rObj["value"]      = r.value;
            rObj["unit"]       = r.unit;
            rObj["alertState"] = r.alertState;
            rObj["timestamp"]  = r.timestamp;
        }
    }

    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    http.end();

    if (code > 0 && code < 400) {
        g_syncLastOk   = true;
        g_syncLastTime = nowEpoch();
        s_lastSyncHash = hash;
        Serial.printf("[DB] Sync OK (HTTP %d)\n", code);
    } else {
        g_syncLastOk = false;
        Serial.printf("[DB] Sync FAILED: %s\n", http.errorToString(code).c_str());
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void initDBSync()
{
    g_syncLastOk   = false;
    g_syncLastTime = 0;
    s_lastSyncHash = 0;
}

void updateDBSync()
{
    if (!systemConfig.syncEnabled) return;

    // Guard against syncInterval = 0 (would busy-loop)
    unsigned long interval = (unsigned long)max(systemConfig.syncInterval, 5) * 1000UL;
    if (millis() - s_lastSync >= interval) {
        syncToMongo();
        s_lastSync = millis();
    }
}
