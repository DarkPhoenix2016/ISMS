#include <Arduino.h>
#include <WebServer.h>
#include <esp_task_wdt.h>   // I3: hardware watchdog
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "sensors.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "display_manager.h"
#include "keypad_handler.h"

WebServer server(80);

void setup()
{
    Serial.begin(115200);
    delay(500);

    // Disable brownout detector so relay coil back-EMF transients on the
    // power rail do not trigger an unwanted reset when the relay switches.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    // I3: 30-second hardware watchdog — resets the node if loop() stalls
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    // 0. Display and keypad must be first so all boot messages are visible
    displayInit();
    keypadInit();
    displayPrint("BOOT");

    // 1. Mount LittleFS
    initStorage();
    displayPrint("FS");

    // 2. Load persistent config and sensor list from flash
    loadConfig();
    loadSensors();
    displayPrint("CFG");

    // 3. Initialise hardware pins + ADC calibration + MQ baseline sampling
    initSensors();
    displayPrint("SENS");

    // 4. Connect to WiFi or start hotspot
    // (wifi_manager.cpp calls displayPrint with IP/status internally)
    startWiFi();

    // 5. Register HTTP routes and start the web server
    startWebServer();
    displayPrint("RDY");
}

void loop()
{
    esp_task_wdt_reset();   // I3: keep watchdog alive

    // B8: re-connect if station mode link dropped (setAutoReconnect handles
    // the actual reconnect; this just logs it and prevents stale state)
    if (!systemConfig.standalone && WiFi.status() != WL_CONNECTED) {
        static unsigned long s_lastLog = 0;
        if (millis() - s_lastLog > 30000) {
            s_lastLog = millis();
            Serial.println("[WIFI] Link down — waiting for auto-reconnect...");
            displayPrint("DISC");
        }
    }

    // Non-blocking discovery service
    updateDiscovery();

    // Non-blocking sensor read + threshold evaluation
    readDynamicSensors();

    // Process pending HTTP requests
    server.handleClient();

    // Refresh LCD display (scroll, menu timeout)
    displayUpdate();

    // Scan buttons — non-blocking, forwards presses to display menu
    keypadScan();
}