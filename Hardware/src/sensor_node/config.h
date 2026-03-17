#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <vector>

struct DynamicSensor {
    String id;
    String name;
    String type;
    int    pin1;
    int    pin2;
    unsigned long readInterval;
    unsigned long lastReadTime;
    float  lastValue;
    String alertState;      // "ok" | "high" | "low" | "active" | "gas"
    bool   alertEnabled;
    float  thresholdHigh;   // NAN = not set
    float  thresholdLow;    // NAN = not set
    // Relay-specific (only meaningful when type == "relay")
    bool   relayOn;         // authoritative software state: true = ON (coil energised)
    bool   relayAutoOff;    // auto-turn OFF when linked ACS712 exceeds HIGH threshold
    String relayLinkedId;   // sensor id of the ACS712 to watch (empty = disabled)
};

struct DeviceConfig {
    String hostname;
    String wifiSSID;
    String wifiPassword;
    bool   standalone;
    String factory;
    String building;
    String room;
    int    maxDataPoints;
    String hotspotPassword;
    String apiKey;          // Required header X-API-Key on all mutating endpoints
    String webUsername;     // Web UI login username (default: admin)
    String webPassword;     // Web UI login password (default: admin)
};

extern DeviceConfig            systemConfig;
extern std::vector<DynamicSensor> activeSensors;

void initStorage();
void loadConfig();
void saveConfig();
void loadSensors();
void saveSensors();
void addSensorConfig(DynamicSensor s);

// Generate a unique default password / API key from the ESP32 chip ID.
// Returns an 8-character hex string derived from the lower 32 bits of the MAC.
String chipDerivedSecret(const char* prefix = "");

#endif