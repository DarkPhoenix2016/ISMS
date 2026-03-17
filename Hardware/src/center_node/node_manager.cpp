#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "node_manager.h"
#include "config.h"

static WiFiUDP       s_udp;
static const int     UDP_PORT              = 4210;
static unsigned long s_lastDiscovery       = 0;
static unsigned long s_lastPoll            = 0;
static unsigned long s_lastMeta            = 0;
static size_t        s_pollIndex           = 0;   // B4: round-robin cursor

static const unsigned long DISCOVERY_INTERVAL_MS = 10000;  // 10 s
static const unsigned long POLL_INTERVAL_MS       = 5000;   // 5 s — one node per tick
static const unsigned long META_REFRESH_MS        = 60000;  // 60 s full meta pass
static const unsigned long OFFLINE_TIMEOUT_MS     = 15000;  // 15 s → mark offline
static const unsigned long DROP_TIMEOUT_MS        = 120000; // 2 min → prune node

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint32_t nowEpoch()
{
    time_t t = time(nullptr);
    // B7: only use millis() fallback if NTP has not synced;
    //     after 49-day millis() rollover the fallback value wraps to ~0 which
    //     produces 1970 timestamps — callers should guard with ntpSynced().
    return (t > 1000000UL) ? (uint32_t)t : (uint32_t)(millis() / 1000UL);
}

bool ntpSynced()
{
    return time(nullptr) > 1000000UL;
}

// ── Meta refresh ──────────────────────────────────────────────────────────────
// I7: separated from pollSingleNode() so it does not add its own HTTP round-trip
// to every poll cycle.  Called on its own 60-second timer in updateNodeManager().

static void refreshMeta(RemoteNode& node)
{
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    http.begin("http://" + node.ip + "/api/sensors");

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err && doc.is<JsonArray>()) {
            node.sensorMeta.clear();
            for (JsonObject s : doc.as<JsonArray>()) {
                SensorMeta m;
                m.id   = s["id"]   | "";
                m.name = s["name"] | "";
                m.type = s["type"] | "";
                if (!m.id.isEmpty()) node.sensorMeta.push_back(m);
            }
        }
    }
    http.end();
    node.metaRefresh = millis();
}

static String metaName(const RemoteNode& node, const String& id)
{
    for (const auto& m : node.sensorMeta) {
        if (m.id == id) return m.name.isEmpty() ? id : m.name;
    }
    return id;
}

static String metaType(const RemoteNode& node, const String& id)
{
    for (const auto& m : node.sensorMeta) {
        if (m.id == id) return m.type;
    }
    return "";
}

// ── Discovery ─────────────────────────────────────────────────────────────────

static void broadcastDiscovery()
{
    // B9: compute correct broadcast for any subnet mask (not just /24)
    IPAddress local = WiFi.localIP();
    IPAddress mask  = WiFi.subnetMask();
    IPAddress bcast;
    for (int i = 0; i < 4; i++) bcast[i] = local[i] | (~mask[i] & 0xFF);

    s_udp.beginPacket(bcast, UDP_PORT);
    s_udp.print("ISMS_DISCOVERY_REQUEST");
    s_udp.endPacket();
}

static void processDiscoveryReplies()
{
    // B5: drain all queued UDP packets — not just one per loop()
    while (s_udp.parsePacket()) {
        char buf[256];
        int len = s_udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = '\0';

        JsonDocument doc;
        if (deserializeJson(doc, buf)) continue;

        String nodeId = doc["id"] | "";
        if (nodeId.isEmpty()) continue;

        String type = doc["type"] | "";
        if (type == "center_node") continue;

        String remoteIp = s_udp.remoteIP().toString();

        // B6: match by either node ID or IP so a manually-added node that later
        //     responds via UDP gets its real ID assigned (no duplicate entry).
        for (auto& node : discoveredNodes) {
            if (node.id == nodeId || node.ip == remoteIp) {
                node.id       = nodeId;              // upgrade "manual_x.x.x.x" IDs
                node.hostname = doc["hostname"] | node.hostname.c_str();
                node.ip       = remoteIp;
                node.lastSeen = millis();
                node.online   = true;
                return;
            }
        }

        // Genuinely new node
        RemoteNode n;
        n.id          = nodeId;
        n.hostname    = doc["hostname"] | "SensorNode";
        n.ip          = remoteIp;
        n.lastSeen    = millis();
        n.online      = true;
        n.metaRefresh = 0;
        discoveredNodes.push_back(n);
        Serial.printf("[NODE] Discovered: %s @ %s\n", n.hostname.c_str(), n.ip.c_str());
    }
}

// ── Polling ───────────────────────────────────────────────────────────────────

static void pollSingleNode(RemoteNode& node)
{
    // I7: meta is no longer fetched here — refreshMeta() runs on its own timer
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    http.begin("http://" + node.ip + "/api/status");

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err && doc.is<JsonObject>()) {
            node.lastReadings.clear();
            for (JsonPair kv : doc.as<JsonObject>()) {
                NodeReading r;
                r.sensorId   = kv.key().c_str();
                r.value      = kv.value()["value"]      | 0.0f;
                r.alertState = kv.value()["alertState"] | "ok";
                r.unit       = kv.value()["unit"]       | "";
                r.name       = metaName(node, r.sensorId);
                r.type       = metaType(node, r.sensorId);
                r.timestamp  = nowEpoch();
                node.lastReadings.push_back(r);
            }
        }
        node.lastSeen = millis();
        node.online   = true;
    } else {
        if ((millis() - node.lastSeen) > OFFLINE_TIMEOUT_MS) {
            node.online = false;
        }
    }
    http.end();
}

// B4: poll ONE node per call (round-robin) instead of all nodes sequentially.
// With a 5 s tick and N nodes, each node is polled every N*5 s — acceptable
// for safety-system refresh rates and eliminates multi-second web-server freezes.
static void pollNextNode()
{
    if (discoveredNodes.empty()) return;

    // I4: prune nodes silent longer than DROP_TIMEOUT_MS
    unsigned long now = millis();
    for (auto it = discoveredNodes.begin(); it != discoveredNodes.end(); ) {
        if (!it->online && (now - it->lastSeen) > DROP_TIMEOUT_MS) {
            Serial.printf("[NODE] Pruned stale node: %s (%s)\n",
                          it->hostname.c_str(), it->ip.c_str());
            if (s_pollIndex > 0) s_pollIndex--;
            it = discoveredNodes.erase(it);
        } else {
            ++it;
        }
    }

    if (discoveredNodes.empty()) return;

    // Advance to next valid node (wrap around)
    s_pollIndex = s_pollIndex % discoveredNodes.size();
    pollSingleNode(discoveredNodes[s_pollIndex]);
    s_pollIndex++;
}

// I7: refresh meta for all online nodes — runs on its own 60-second timer
static void refreshMetaAll()
{
    for (auto& node : discoveredNodes) {
        if (node.online) refreshMeta(node);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void initNodeManager()
{
    s_udp.begin(UDP_PORT);
    Serial.printf("[NODE] Manager started, listening UDP port %d\n", UDP_PORT);
}

void updateNodeManager()
{
    unsigned long now = millis();

    if (now - s_lastDiscovery > DISCOVERY_INTERVAL_MS) {
        broadcastDiscovery();
        s_lastDiscovery = now;
    }

    processDiscoveryReplies();

    // B4: one node per tick
    if (now - s_lastPoll > POLL_INTERVAL_MS) {
        pollNextNode();
        s_lastPoll = now;
    }

    // I7: meta refresh on its own timer — does not block poll ticks
    if (now - s_lastMeta > META_REFRESH_MS) {
        refreshMetaAll();
        s_lastMeta = now;
    }
}

bool addNodeByIP(const String& ip)
{
    // Already known — just mark online and let the poll cycle update it.
    // B6: also update ID if it was set as "manual_x.x.x.x" previously
    for (auto& node : discoveredNodes) {
        if (node.ip == ip) {
            node.online   = true;
            node.lastSeen = millis();
            return true;
        }
    }

    // Quick reachability ping (1.5 s total budget — must not block web server long)
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(1500);
    http.begin("http://" + ip + "/api/status");
    int code = http.GET();
    http.end();

    if (code != HTTP_CODE_OK) {
        Serial.printf("[NODE] addNodeByIP %s failed (HTTP %d)\n", ip.c_str(), code);
        return false;
    }

    RemoteNode n;
    n.ip          = ip;
    n.id          = "manual_" + ip;  // real ID assigned on first UDP reply (B6)
    n.hostname    = ip;
    n.lastSeen    = millis();
    n.online      = true;
    n.metaRefresh = 0;
    discoveredNodes.push_back(n);
    Serial.printf("[NODE] Manually added: %s\n", ip.c_str());
    return true;
}
