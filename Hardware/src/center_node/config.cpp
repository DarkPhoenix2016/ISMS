#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_system.h>

CenterConfig systemConfig;
std::vector<RemoteNode> discoveredNodes;

String chipDerivedSecret(const char* prefix)
{
    uint32_t low = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL);
    char buf[9];
    snprintf(buf, sizeof(buf), "%08" PRIx32, low);
    return String(prefix) + String(buf);
}

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

void loadConfig()
{
    if (!LittleFS.exists("/config.json")) {
        // B2: First-boot defaults — persisted immediately so exists() works next boot
        systemConfig.hostname        = "CentralNode";
        systemConfig.standalone      = true;
        systemConfig.hotspotPassword = chipDerivedSecret("p"); // S2: unique per device
        systemConfig.webUsername     = "admin";
        systemConfig.webPassword     = chipDerivedSecret("w"); // S1: unique per device
        systemConfig.apiKey          = chipDerivedSecret("k");
        systemConfig.syncInterval    = 30;
        systemConfig.syncEnabled     = false;
        systemConfig.maxDataPoints   = 30;
        systemConfig.ntpServer       = "pool.ntp.org";
        Serial.printf("[CFG] First boot — hotspot pass=%s\n",
                      systemConfig.hotspotPassword.c_str());
        Serial.printf("[CFG] First boot — web pass=%s\n",
                      systemConfig.webPassword.c_str());
        Serial.printf("[CFG] First boot — API key=%s\n",
                      systemConfig.apiKey.c_str());
        saveConfig();
        return;
    }

    File file = LittleFS.open("/config.json", "r");
    if (!file) return;

    JsonDocument doc;
    deserializeJson(doc, file);
    file.close();

    systemConfig.hostname        = doc["hostname"]        | "CentralNode";
    systemConfig.wifiSSID        = doc["ssid"]            | "";
    systemConfig.wifiPassword    = doc["pass"]            | "";
    systemConfig.standalone      = doc["standalone"]      | true;
    systemConfig.factory         = doc["factory"]         | "";
    systemConfig.building        = doc["building"]        | "";
    systemConfig.room            = doc["room"]            | "";
    systemConfig.maxDataPoints   = doc["maxDataPoints"]   | 30;
    // B1: store temporary String before .c_str() to avoid dangling pointer
    const String defaultKey = chipDerivedSecret("k");
    systemConfig.apiKey     = doc["apiKey"] | defaultKey.c_str();

    systemConfig.mongoUrl        = doc["mongoUrl"]        | "";
    systemConfig.mongoApiKey     = doc["mongoApiKey"]     | "";
    systemConfig.mongoDatabase   = doc["mongoDatabase"]   | "isms_db";
    systemConfig.mongoCollection = doc["mongoCollection"] | "sensor_readings";
    systemConfig.syncInterval    = doc["syncInterval"]    | 30;
    systemConfig.syncEnabled     = doc["syncEnabled"]     | false;

    // S2/S1: fall back to chip-derived defaults (not shared static strings)
    const String defaultHotspot = chipDerivedSecret("p");
    const String defaultWebPass = chipDerivedSecret("w");
    systemConfig.hotspotPassword = doc["hotspotPassword"] | defaultHotspot.c_str();
    systemConfig.webUsername     = doc["webUsername"]     | "admin";
    systemConfig.webPassword     = doc["webPassword"]     | defaultWebPass.c_str();
    systemConfig.ntpServer       = doc["ntpServer"]       | "pool.ntp.org";
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
    doc["apiKey"]          = systemConfig.apiKey;

    doc["mongoUrl"]        = systemConfig.mongoUrl;
    doc["mongoApiKey"]     = systemConfig.mongoApiKey;
    doc["mongoDatabase"]   = systemConfig.mongoDatabase;
    doc["mongoCollection"] = systemConfig.mongoCollection;
    doc["syncInterval"]    = systemConfig.syncInterval;
    doc["syncEnabled"]     = systemConfig.syncEnabled;

    doc["hotspotPassword"] = systemConfig.hotspotPassword;
    doc["webUsername"]     = systemConfig.webUsername;
    doc["webPassword"]     = systemConfig.webPassword;
    doc["ntpServer"]       = systemConfig.ntpServer;

    File file = LittleFS.open("/config.json", "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
    }
}
