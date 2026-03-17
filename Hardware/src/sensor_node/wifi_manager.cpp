#include <WiFi.h>
#include "wifi_manager.h"
#include "config.h"
#include "display_manager.h"   // displayPrint — mirror WiFi status to 74HC595 display

// ── Recovery hotspot ─────────────────────────────────────────────────────────
// Launched when normal WiFi connection fails and no stored config exists.
// Uses a chip-derived unique password so every node has a different
// recovery credential (no shared "12345678" default).
void startConfigHotspot()
{
    WiFi.mode(WIFI_AP);

    String ssid = "SensorNode-Setup-" + chipDerivedSecret();
    String pass = chipDerivedSecret("r");   // 9-char unique recovery password

    WiFi.softAP(ssid.c_str(), pass.c_str());

    Serial.println("[WIFI] Config hotspot started (recovery mode)");
    Serial.printf("[WIFI] SSID: %s\n", ssid.c_str());
    Serial.printf("[WIFI] Pass: %s\n", pass.c_str());
    Serial.print("[WIFI] IP: "); Serial.println(WiFi.softAPIP());
    displayPrint("CONF");
    displayPrint(WiFi.softAPIP().toString());
}

// ── Standalone hotspot ───────────────────────────────────────────────────────
void startStandaloneHotspot()
{
    WiFi.mode(WIFI_AP);

    String ssid = "Node-" + systemConfig.hostname;
    String pass = systemConfig.hotspotPassword;

    // S2: replace legacy default "123456789" and any too-short password with a
    // chip-derived value so every device has a unique hotspot credential.
    if (pass.length() < 8 || pass == "123456789") {
        Serial.println("[WIFI] hotspotPassword reset to chip-derived default");
        pass = chipDerivedSecret("p");
        systemConfig.hotspotPassword = pass;
        saveConfig();
    }

    WiFi.softAP(ssid.c_str(), pass.c_str());

    Serial.printf("[WIFI] Standalone hotspot: SSID=%s\n", ssid.c_str());
    Serial.print("[WIFI] IP: "); Serial.println(WiFi.softAPIP());
    Serial.print("[WIFI] Pass: "); Serial.println(systemConfig.hotspotPassword);
    displayPrint("AP");
    displayPrint(WiFi.softAPIP().toString());

}

// ── Station mode ─────────────────────────────────────────────────────────────
void connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(systemConfig.hostname.c_str());
    WiFi.setAutoReconnect(true);   // B8: re-connect automatically on link drop
    WiFi.begin(systemConfig.wifiSSID.c_str(), systemConfig.wifiPassword.c_str());

    displayPrint("CONN");
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
        displayPrint(WiFi.localIP().toString());
    } else {
        Serial.println("[WIFI] Connection failed — falling back to config hotspot");
        displayPrint("FAIL");
        startConfigHotspot();
    }
}

// ── Top-level selector ───────────────────────────────────────────────────────
void startWiFi()
{
    if (systemConfig.standalone || systemConfig.wifiSSID.isEmpty())
        startStandaloneHotspot();
    else
        connectToWiFi();
}