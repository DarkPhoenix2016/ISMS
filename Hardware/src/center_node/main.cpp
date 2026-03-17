#include <Arduino.h>
#include <WebServer.h>
#include <time.h>
#include <esp_task_wdt.h>   // I3: hardware watchdog
#include "config.h"
#include "wifi_manager.h"
#include "node_manager.h"
#include "db_sync.h"
#include "web_server.h"
#include "display_manager.h"
#include "keypad_handler.h"

extern WebServer server;

void setup() {
    Serial.begin(115200);
    delay(500);

    // Display and keypad first so all boot messages appear on screen
    displayInit();
    keypadInit();
    displayPrint("BOOT");

    // I3: 30-second watchdog — resets if loop() stalls during a long HTTP call
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    Serial.println("[BOOT] Central Node starting...");

    initStorage();
    displayPrint("FS");

    loadConfig();
    displayPrint("CFG");

    startWiFi();
    displayPrint("WIFI");

    // Sync time via NTP (non-blocking — time() returns 0 until sync completes)
    if (!systemConfig.standalone && !systemConfig.wifiSSID.isEmpty()) {
        configTime(0, 0, systemConfig.ntpServer.c_str());
        Serial.printf("[NTP] Configured server: %s\n", systemConfig.ntpServer.c_str());
        displayPrint("NTP");
    }

    initNodeManager();
    displayPrint("NODE");

    initDBSync();
    startWebServer();
    displayPrint("RDY");   // triggers 2-second hold then auto-transition to marquee

    Serial.println("[BOOT] Central Node Ready");
}

void loop() {
    esp_task_wdt_reset();   // I3: keep watchdog alive

    // B8: log WiFi link drops
    if (!systemConfig.standalone && WiFi.status() != WL_CONNECTED) {
        static unsigned long s_lastLog = 0;
        if (millis() - s_lastLog > 30000) {
            s_lastLog = millis();
            Serial.println("[WIFI] Link down — waiting for auto-reconnect...");
        }
    }

    updateNodeManager();
    updateDBSync();
    server.handleClient();

    displayUpdate();
    keypadScan();
}
