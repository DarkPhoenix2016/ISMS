#ifndef CENTER_CONFIG_H
#define CENTER_CONFIG_H

#include <Arduino.h>
#include <vector>

// Cached sensor name/type from a sensor node's /api/sensors endpoint
struct SensorMeta {
    String id;
    String name;
    String type;
};

struct NodeReading {
    String sensorId;
    String name;
    String type;
    float  value;
    String unit;
    String alertState;
    uint32_t timestamp;
};

struct RemoteNode {
    String id;
    String hostname;
    String ip;
    unsigned long lastSeen;   // millis() when last polled successfully
    bool   online;
    std::vector<NodeReading> lastReadings;
    std::vector<SensorMeta>  sensorMeta;
    unsigned long            metaRefresh;  // millis() when meta was last fetched
};

struct CenterConfig {
    String hostname;
    String wifiSSID;
    String wifiPassword;
    bool   standalone;
    String factory;
    String building;
    String room;
    int    maxDataPoints;
    String hotspotPassword;
    String apiKey;          // Required header X-API-Key on mutating endpoints
    String webUsername;     // Web UI login username (default: admin)
    String webPassword;     // Web UI login password (default: admin)

    // MongoDB Settings
    String mongoUrl;        // REST Bridge endpoint URL
    String mongoApiKey;     // Optional API Key header
    String mongoDatabase;   // Database name
    String mongoCollection; // Collection name
    int    syncInterval;    // Sync interval in seconds
    bool   syncEnabled;     // Enable/disable automatic DB sync

    // NTP
    String ntpServer;       // NTP server (default: pool.ntp.org)
};

extern CenterConfig systemConfig;
extern std::vector<RemoteNode> discoveredNodes;

void initStorage();
void loadConfig();
void saveConfig();

String chipDerivedSecret(const char* prefix = "");

#endif
