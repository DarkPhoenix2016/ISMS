#include <WiFi.h>
#include "wifi_manager.h"
#include "config.h"

void startConfigHotspot()
{
    WiFi.mode(WIFI_AP);
    String ssid = "CenterNode-Setup-" + chipDerivedSecret();
    String pass = chipDerivedSecret("r");
    WiFi.softAP(ssid.c_str(), pass.c_str());
    Serial.println("[WIFI] Config hotspot started (recovery mode)");
    Serial.printf("[WIFI] SSID: %s\n", ssid.c_str());
    Serial.printf("[WIFI] Pass: %s\n", pass.c_str());
}

void startStandaloneHotspot()
{
    WiFi.mode(WIFI_AP);
    String ssid = "Center-" + systemConfig.hostname;
    String pass = systemConfig.hotspotPassword;
    // S2: replace legacy default and any too-short password with chip-derived value
    if (pass.length() < 8 || pass == "123456789") {
        pass = chipDerivedSecret("p");
        systemConfig.hotspotPassword = pass;
        saveConfig();
    }
    WiFi.softAP(ssid.c_str(), pass.c_str());
    Serial.printf("[WIFI] Standalone hotspot: SSID=%s\n", ssid.c_str());
}

void connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(systemConfig.hostname.c_str());
    WiFi.setAutoReconnect(true);   // B8
    WiFi.begin(systemConfig.wifiSSID.c_str(), systemConfig.wifiPassword.c_str());

    Serial.print("[WIFI] Connecting to " + systemConfig.wifiSSID);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WIFI] Connected!");
        Serial.print("[WIFI] IP: "); Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WIFI] Connection failed — falling back to config hotspot");
        startConfigHotspot();
    }
}

void startWiFi()
{
    if (systemConfig.standalone || systemConfig.wifiSSID.isEmpty())
        startStandaloneHotspot();
    else
        connectToWiFi();
}
