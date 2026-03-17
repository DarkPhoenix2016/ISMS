#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <cmath>
#include <esp_system.h>   // ESP.getEfuseMac()

DeviceConfig               systemConfig;
std::vector<DynamicSensor> activeSensors;

// ── Chip-derived secret ──────────────────────────────────────────────────────
String chipDerivedSecret(const char* prefix)
{
    uint32_t low = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL);
    char buf[9];
    snprintf(buf, sizeof(buf), "%08" PRIx32, low);
    return String(prefix) + String(buf);
}

// ── Storage init ─────────────────────────────────────────────────────────────
void initStorage()
{
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed — formatting...");
        LittleFS.format();
        if (!LittleFS.begin(true)) {
            Serial.println("[FS] FATAL: cannot mount LittleFS");
            return;
        }
    }
    Serial.println("[FS] LittleFS mounted OK");
}

// ── Config load / save ───────────────────────────────────────────────────────
void loadConfig()
{
    if (!LittleFS.exists("/config.json")) {
        // First boot — derive unique defaults from chip ID
        systemConfig.maxDataPoints   = 30;
        systemConfig.hotspotPassword = chipDerivedSecret("p"); // S2: unique per device
        systemConfig.webUsername     = "admin";
        systemConfig.webPassword     = chipDerivedSecret("w"); // S1: unique per device
        systemConfig.apiKey          = chipDerivedSecret("k");
        systemConfig.standalone      = true;
        systemConfig.hostname        = "SensorNode";
        Serial.printf("[CFG] First boot — hotspot pass=%s\n",
                      systemConfig.hotspotPassword.c_str());
        Serial.printf("[CFG] First boot — web pass=%s\n",
                      systemConfig.webPassword.c_str());
        Serial.printf("[CFG] First boot — API key=%s\n",
                      systemConfig.apiKey.c_str());
        
        saveConfig(); // Persist defaults
        return;
    }

    File file = LittleFS.open("/config.json", "r");
    if (!file) {
        Serial.println("[CFG] Failed to open config.json");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CFG] JSON parse error: %s — using defaults\n", err.c_str());
        return;
    }

    systemConfig.hostname        = doc["hostname"]        | "SensorNode";
    systemConfig.wifiSSID        = doc["ssid"]            | "";
    systemConfig.wifiPassword    = doc["pass"]            | "";
    systemConfig.standalone      = doc["standalone"]      | true;
    systemConfig.factory         = doc["factory"]         | "";
    systemConfig.building        = doc["building"]        | "";
    systemConfig.room            = doc["room"]            | "";
    systemConfig.maxDataPoints   = doc["maxDataPoints"]   | 30;
    systemConfig.hotspotPassword = doc["hotspotPassword"] | chipDerivedSecret("p").c_str();
    // B1: store temporary String before .c_str() to avoid dangling pointer
    const String defaultKey = chipDerivedSecret("k");
    systemConfig.apiKey      = doc["apiKey"] | defaultKey.c_str();
    systemConfig.webUsername = doc["webUsername"] | "admin";
    // S1: default web password is chip-derived, not the same across all devices
    const String defaultPass = chipDerivedSecret("w");
    systemConfig.webPassword = doc["webPassword"] | defaultPass.c_str();
}

void saveConfig()
{
    JsonDocument doc;
    doc["hostname"]        = systemConfig.hostname;
    doc["ssid"]            = systemConfig.wifiSSID;
    doc["pass"]            = systemConfig.wifiPassword;
    doc["standalone"]      = systemConfig.standalone;
    doc["factory"]         = systemConfig.factory;
    doc["building"]        = systemConfig.building;
    doc["room"]            = systemConfig.room;
    doc["maxDataPoints"]   = systemConfig.maxDataPoints;
    doc["hotspotPassword"] = systemConfig.hotspotPassword;
    doc["apiKey"]          = systemConfig.apiKey;
    doc["webUsername"]     = systemConfig.webUsername;
    doc["webPassword"]     = systemConfig.webPassword;

    File file = LittleFS.open("/config.json", "w");
    if (!file) { Serial.println("[CFG] Failed to write config.json"); return; }
    serializeJson(doc, file);
    file.close();
}

// ── Sensor load / save ───────────────────────────────────────────────────────
void loadSensors()
{
    activeSensors.clear();
    if (!LittleFS.exists("/sensors.json")) {
        saveSensors();   // write empty [] so exists() succeeds on next boot
        return;
    }

    File file = LittleFS.open("/sensors.json", "r");
    if (!file) { Serial.println("[CFG] Failed to open sensors.json"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CFG] sensors.json parse error: %s\n", err.c_str());
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array) {
        DynamicSensor s;
        s.id           = obj["id"].as<String>();
        s.name         = obj["name"].as<String>();
        s.type         = obj["type"].as<String>();
        s.pin1         = obj["pin1"]     | -1;
        s.pin2         = obj["pin2"]     | -1;
        s.readInterval = obj["interval"] | 5000UL;
        s.lastReadTime = 0;
        s.lastValue    = 0.0f;
        s.alertState   = "ok";
        s.alertEnabled = obj["alertEnabled"] | false;

        s.thresholdHigh = obj["thresholdHigh"].is<float>()
                          ? obj["thresholdHigh"].as<float>() : NAN;
        s.thresholdLow  = obj["thresholdLow"].is<float>()
                          ? obj["thresholdLow"].as<float>()  : NAN;

        s.relayOn       = false;                          // always boot OFF for safety
        s.relayAutoOff  = obj["relayAutoOff"]  | false;
        s.relayLinkedId = obj["relayLinkedId"].is<String>()
                          ? obj["relayLinkedId"].as<String>() : "";

        activeSensors.push_back(s);
    }
}

void saveSensors()
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
        obj["alertEnabled"] = s.alertEnabled;

        if (!isnan(s.thresholdHigh)) obj["thresholdHigh"] = s.thresholdHigh;
        else                          obj["thresholdHigh"] = nullptr;
        if (!isnan(s.thresholdLow))  obj["thresholdLow"]  = s.thresholdLow;
        else                          obj["thresholdLow"]  = nullptr;

        obj["relayAutoOff"]  = s.relayAutoOff;
        obj["relayLinkedId"] = s.relayLinkedId;
    }

    File file = LittleFS.open("/sensors.json", "w");
    if (!file) { Serial.println("[CFG] Failed to write sensors.json"); return; }
    serializeJson(doc, file);
    file.close();
}

void addSensorConfig(DynamicSensor s)
{
    // B3: reset only operational state; preserve thresholds pre-populated by caller
    s.alertEnabled  = false;
    s.alertState    = "ok";
    s.relayOn       = false;
    s.relayAutoOff  = false;
    s.relayLinkedId = "";
    // thresholdHigh / thresholdLow intentionally NOT reset here
    activeSensors.push_back(s);
    saveSensors();
}
