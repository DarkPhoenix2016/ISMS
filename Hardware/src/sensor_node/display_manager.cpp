#include "display_manager.h"
#include "config.h"   // systemConfig, activeSensors
#include <WiFi.h>     // WiFi.localIP(), WiFi.softAPIP()
#include <LiquidCrystal.h>
#include <LittleFS.h>

// ============================================================
// SECTION 1 — HD44780 1602A DIRECT 4-BIT DRIVER
// ============================================================
// LiquidCrystal handles the HD44780 protocol.
// Pins defined in display_manager.h match the physical wiring.

static LiquidCrystal lcd(LCD_PIN_RS, LCD_PIN_EN,
                          LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);


// ============================================================
// SECTION 2 — LOW-LEVEL LCD PRIMITIVES (LiquidCrystal wrappers)
// ============================================================

static void lcdSetCursor(uint8_t col, uint8_t row)
{
    lcd.setCursor(col, row);
}

// Write a String at the current cursor, padding with spaces to 'width'.
static void lcdPrint(const String& s, uint8_t width = 16)
{
    uint8_t len = (uint8_t)min((int)s.length(), (int)width);
    for (uint8_t i = 0; i < len; i++) lcd.write((uint8_t)s[i]);
    for (uint8_t i = len; i < width; i++) lcd.write(' ');
}

static void lcdClear()
{
    lcd.clear();
}


// ============================================================
// SECTION 3 — STATE
// ============================================================

// Two cached lines (updated by displayPrint, shown in INFO mode)
static String s_line0 = "SENSOR NODE";
static String s_line1 = "BOOTING...";

// ── Idle mode ────────────────────────────────────────────────────────────────
// MARQUEE  default: scrolling ticker on line 1
// INFO     show IP/status/sensor rotation; entered by button from MARQUEE/ALERT
// ALERT    scrolling alert details; overrides MARQUEE when any alert is active
enum IdleMode { IM_MARQUEE, IM_INFO, IM_ALERT, IM_REBOOT_CONFIRM, IM_FACTORY_CONFIRM1, IM_FACTORY_CONFIRM2 };
static IdleMode      s_idleMode      = IM_MARQUEE;
static unsigned long s_infoEnterTime = 0;
static const unsigned long INFO_TIMEOUT_MS = 15000UL;

// Marquee / alert scroll state
static String        s_mqText  = "";
static int           s_mqPos   = 0;
static unsigned long s_mqTime  = 0;
static String        s_alText  = "";
static int           s_alPos   = 0;
static unsigned long s_alTime  = 0;
static const unsigned long MQ_SCROLL_MS = 300;

// INFO sensor rotation
static unsigned long s_idleLastSwitch = 0;
static int           s_idleCursor     = -1;

// ── Menu state ───────────────────────────────────────────────────────────────
static bool s_menuActive  = false;
static int  s_menuCursor  = 0;
static bool s_inSubMenu   = false;
static int  s_subCursor   = 0;
static bool s_showContent = false;
static bool s_reveal      = false;   // true while DOWN held on PASS/KEY screen

static unsigned long s_lastKeyTime = 0;
static const unsigned long MENU_TIMEOUT_MS = 30000UL;

// Scroll state for long menu content strings
static String        s_scrollText = "";
static int           s_scrollPos  = 0;
static unsigned long s_scrollTime = 0;
static const unsigned long SCROLL_MS = 380;

// Menu item labels
static const char* const MENU_LABELS[] = {
    "IP ADDRESS",
    "WIFI SSID",
    "WIFI PASS",
    "API KEY",
    "SENSORS",
    "SET AP MODE",
    "FACTORY RESET",
    "REBOOT",
    "EXIT"
};
static const int MENU_COUNT = 9;
enum MenuItem { MI_IP=0, MI_SSID, MI_PASS, MI_KEY, MI_SENS, MI_AP_MODE, MI_FACTORY, MI_REBOOT, MI_EXIT };

// Sensor snapshot cache
struct SensorSnap { String id; String name; float value; String unit; uint32_t interval; };
static SensorSnap s_snaps[12];
static int        s_snapCount = 0;


// ============================================================
// SECTION 3B — IDLE MODE HELPERS
// ============================================================

static bool hasActiveAlerts()
{
    for (const auto& s : activeSensors) {
        if (s.alertState == "ok") continue;
        if (s.type == "relay") continue;           // relay ON is not an alert
        // Vib and gas sensors fire autonomously — no alertEnabled required
        if (s.type == "vib" || s.type == "mq2" || s.type == "mq135") return true;
        // Threshold sensors must have alerts explicitly enabled
        if (s.alertEnabled) return true;
    }
    return false;
}

// Build one long scrolling string from all active alert descriptions.
static String buildAlertStr()
{
    String msg = "  ";
    for (const auto& s : activeSensors) {
        if (s.alertState == "ok") continue;
        if (s.type == "relay") continue;
        bool fires = (s.type == "vib" || s.type == "mq2" || s.type == "mq135")
                     || s.alertEnabled;
        if (!fires) continue;
        String st = s.alertState;
        st.toUpperCase();
        msg += s.name + ": " + st + "  |  ";
    }
    return msg;
}

// Build the marquee ticker: hostname | IP | status | sensor readings.
static String buildMarqueeStr()
{
    IPAddress ip   = systemConfig.standalone ? WiFi.softAPIP() : WiFi.localIP();
    String    ipStr = (ip.toString() == "0.0.0.0") ? "Connecting" : ip.toString();
    String name    = systemConfig.hostname.isEmpty() ? "SENSOR NODE" : systemConfig.hostname;
    String msg     = "  " + name + "  |  IP:" + ipStr + "  |  " + s_line1 + "  ";
    for (int i = 0; i < s_snapCount; i++) {
        msg += "|  " + s_snaps[i].name + ": "
             + String(s_snaps[i].value, 1) + s_snaps[i].unit + "  ";
    }
    return msg;
}

// ── Mode transition helpers ───────────────────────────────────────────────────

static void enterMarquee()
{
    s_idleMode = IM_MARQUEE;
    s_mqText   = "";    // force rebuild on first tick
    s_mqPos    = 0;
    s_mqTime   = millis();
    String name = systemConfig.hostname.isEmpty() ? "SENSOR NODE" : systemConfig.hostname;
    lcdSetCursor(0, 0); lcdPrint(name);
    lcdSetCursor(0, 1); lcdPrint("                ");
}

static void enterInfo()
{
    s_idleMode       = IM_INFO;
    s_infoEnterTime  = millis();
    s_idleCursor     = -1;
    s_idleLastSwitch = 0;   // force immediate draw
    lcdClear();
    lcdSetCursor(0, 0); lcdPrint(s_line0);
    lcdSetCursor(0, 1); lcdPrint(s_line1);
}

static void enterAlert()
{
    s_idleMode = IM_ALERT;
    s_alText   = "";
    s_alPos    = 0;
    s_alTime   = millis();
    lcdSetCursor(0, 0); lcdPrint("!! ALERT !!");
    lcdSetCursor(0, 1); lcdPrint("                ");
}


// ============================================================
// SECTION 4 — CONTENT HELPERS
// ============================================================

static String getMenuContent()
{
    switch (s_menuCursor) {
        case MI_IP: {
            IPAddress ip = systemConfig.standalone
                           ? WiFi.softAPIP()
                           : WiFi.localIP();
            String ipStr = ip.toString();
            return (ipStr == "0.0.0.0") ? "Not connected" : ipStr;
        }
        case MI_SSID:
            if (systemConfig.standalone || systemConfig.wifiSSID.isEmpty()) {
                String apName = "Node-" + (systemConfig.hostname.isEmpty()
                                           ? String("SensorNode")
                                           : systemConfig.hostname);
                return apName;
            }
            return systemConfig.wifiSSID;
        case MI_PASS:
            if (!s_reveal) return "** hidden **";
            return (systemConfig.standalone || systemConfig.wifiSSID.isEmpty())
                   ? systemConfig.hotspotPassword
                   : systemConfig.wifiPassword;
        case MI_KEY:
            return s_reveal ? systemConfig.apiKey : "** hidden **";
        case MI_SENS: {
            if (s_snapCount == 0) return "No sensors yet";
            int idx = constrain(s_subCursor, 0, s_snapCount - 1);
            return String(s_snaps[idx].value, 2) + " " + s_snaps[idx].unit;
        }
        default:
            return "";
    }
}

static String getSensorLine0()
{
    if (s_snapCount == 0) return "No sensors";
    int idx = constrain(s_subCursor, 0, s_snapCount - 1);
    // "Name     [1/3]" — truncate name to leave space for counter
    String counter = " [" + String(s_subCursor + 1) + "/" + String(s_snapCount) + "]";
    int nameMax = 16 - (int)counter.length();
    return s_snaps[idx].name.substring(0, nameMax) + counter;
}


// ============================================================
// SECTION 5 — SCREEN DRAWING
// ============================================================

// Redraw both lines in boot/idle mode
static void drawBoot()
{
    lcdSetCursor(0, 0);
    lcdPrint(s_line0);
    lcdSetCursor(0, 1);
    lcdPrint(s_line1);
}

// Show top-level menu navigation
static void drawMenuNav()
{
    s_scrollText = "";
    lcdSetCursor(0, 0);
    lcdPrint("=== MENU ===");
    lcdSetCursor(0, 1);
    lcdPrint("> " + String(MENU_LABELS[s_menuCursor]));
}

// Show content for the selected menu item
static void drawMenuContent()
{
    String line0, value;

    if (s_menuCursor == MI_SENS) {
        line0 = getSensorLine0();
        value = getMenuContent();
    } else {
        line0 = String(MENU_LABELS[s_menuCursor]) + ":";
        value = getMenuContent();
    }

    lcdSetCursor(0, 0);
    lcdPrint(line0);

    // Set up scrolling if value is longer than 16 chars
    if ((int)value.length() > 16) {
        if (s_scrollText != value) {
            s_scrollText = value;
            s_scrollPos  = 0;
            s_scrollTime = millis();
        }
        // Render current scroll window
        String padded  = value + "                ";   // 16 trailing spaces
        String visible = padded.substring(s_scrollPos, s_scrollPos + 16);
        lcdSetCursor(0, 1);
        lcdPrint(visible);
    } else {
        s_scrollText = "";
        lcdSetCursor(0, 1);
        lcdPrint(value);
    }
}


// ============================================================
// SECTION 6 — PUBLIC API
// ============================================================

void displayInit()
{
    lcd.begin(16, 2);
    lcd.clear();

    lcdSetCursor(0, 0);
    lcdPrint("SENSOR NODE");
    lcdSetCursor(0, 1);
    lcdPrint("BOOTING...");

    Serial.println("[DISP] 1602A LCD init OK  (4-bit direct)");
    Serial.println("[DISP] Pins: RS=G23 EN=G22 D4=G21 D5=G19 D6=G18 D7=G5");
}

// Queue a status / boot message.
// - IP addresses (≥2 dots) update s_line0 as "IP:<addr>"
// - All other strings update s_line1
// During boot (before loop) and in INFO mode the LCD is updated immediately.
// In MARQUEE/ALERT mode only the cache is updated; the marquee rebuilds on its
// next wrap so the new status appears there automatically.
void displayPrint(const String& msg)
{
    int dots = 0;
    for (unsigned int i = 0; i < msg.length(); i++)
        if (msg[i] == '.') dots++;

    if (dots >= 2) {
        s_line0 = "IP:" + msg;
        if (!s_menuActive) { lcdSetCursor(0, 0); lcdPrint(s_line0); }
    } else {
        s_line1 = msg;
        if (!s_menuActive) { lcdSetCursor(0, 1); lcdPrint(s_line1); }
    }
    s_mqText = "";   // invalidate marquee so next wrap picks up new status
}

// Called by keypad_handler when BTN_DOWN is held/released on PASS or KEY screen.
// Immediately redraws the content line to show or hide the secret.
void displaySetReveal(bool on)
{
    if (s_reveal == on) return;
    s_reveal = on;
    if (s_menuActive && s_showContent &&
        (s_menuCursor == MI_PASS || s_menuCursor == MI_KEY)) {
        s_scrollText = "";   // reset scroll so value re-renders from start
        drawMenuContent();
    }
}

void displayTriggerRebootConfirm()
{
    if (s_menuActive) return;   // ignore if user is in menu
    s_idleMode = IM_REBOOT_CONFIRM;
    lcdClear();
    lcdSetCursor(0, 0); lcdPrint("REBOOT? UP=YES");
    lcdSetCursor(0, 1); lcdPrint("DOWN=CANCEL");
}

void displaySetSensorSnapshot(const String& id, const String& name,
                              float value,       const String& unit,
                              uint32_t interval)
{
    for (int i = 0; i < s_snapCount; i++) {
        if (s_snaps[i].id == id) {
            s_snaps[i].value = value;
            s_snaps[i].unit  = unit;
            s_snaps[i].name  = name;
            s_snaps[i].interval = interval;
            return;
        }
    }
    if (s_snapCount < 12) {
        s_snaps[s_snapCount++] = { id, name, value, unit, interval };
    }
}

bool displayInMenu() { return s_menuActive; }


// ============================================================
// SECTION 7 — KEY HANDLER (called by keypad_handler)
// ============================================================

void displayHandleKey(char key)
{
    s_lastKeyTime = millis();

    // ── FACTORY RESET CONFIRM 1: UP=Next Step, DOWN/SELECT=cancel ─────────────
    if (!s_menuActive && s_idleMode == IM_FACTORY_CONFIRM1) {
        if (key == 'A' || key == '2') { // UP
            s_idleMode = IM_FACTORY_CONFIRM2;
            lcdClear();
            lcdSetCursor(0, 0); lcdPrint("CONFIRM RESET?");
            lcdSetCursor(0, 1); lcdPrint("UP=YES DOWN=NO");
        } else if (key == 'B' || key == '8' || key == 'D' || key == '#') { // DOWN or SELECT
            lcdClear();
            enterMarquee();
        }
        return;
    }

    // ── FACTORY RESET CONFIRM 2: UP=EXECUTE, DOWN/SELECT=cancel ─────────────
    if (!s_menuActive && s_idleMode == IM_FACTORY_CONFIRM2) {
        if (key == 'A' || key == '2') { // UP
            lcdClear();
            lcdSetCursor(0, 0); lcdPrint("FORMATTING...");
            Serial.println("[SYS] Factory reset triggered from keypad...");
            LittleFS.format();
            lcdSetCursor(0, 1); lcdPrint("REBOOTING...");
            delay(2000);
            ESP.restart();
        } else if (key == 'B' || key == '8' || key == 'D' || key == '#') { // DOWN or SELECT
            lcdClear();
            enterMarquee();
        }
        return;
    }

    // ── REBOOT CONFIRM: UP=restart, DOWN/SELECT=cancel ────────────────────────
    if (!s_menuActive && s_idleMode == IM_REBOOT_CONFIRM) {
        bool up   = (key == 'A' || key == '2');
        bool down = (key == 'B' || key == '8');
        if (up) {
            lcdClear();
            lcdSetCursor(0, 0); lcdPrint("REBOOTING...");
            delay(800);
            ESP.restart();
        }
        if (down || key == 'D') {
            lcdClear();
            enterMarquee();
        }
        return;
    }

    // ── MARQUEE / ALERT: any button → show INFO screen ───────────────────────
    if (!s_menuActive && (s_idleMode == IM_MARQUEE || s_idleMode == IM_ALERT)) {
        enterInfo();
        return;
    }

    // ── INFO: any button → enter menu ────────────────────────────────────────
    if (!s_menuActive && s_idleMode == IM_INFO) {
        s_menuActive  = true;
        s_menuCursor  = 0;
        s_inSubMenu   = false;
        s_showContent = false;
        s_scrollText  = "";
        drawMenuNav();
        return;
    }

    // ── Menu active ───────────────────────────────────────────────────────────
    bool up     = (key == 'A' || key == '2');
    bool down   = (key == 'B' || key == '8');
    bool select = (key == 'D' || key == '#');
    // SELECT doubles as BACK in content view (3-button UI)
    bool back   = (key == 'C' || key == '*') || (s_showContent && select);

    if (!s_showContent) {
        // ── Top-level menu navigation ──────────────────────────────────────
        if (up)   { s_menuCursor = (s_menuCursor - 1 + MENU_COUNT) % MENU_COUNT; drawMenuNav(); }
        if (down) { s_menuCursor = (s_menuCursor + 1) % MENU_COUNT;               drawMenuNav(); }

        if (select) {
            if (s_menuCursor == MI_FACTORY) {
                s_menuActive = false;
                s_idleMode = IM_FACTORY_CONFIRM1;
                lcdClear();
                lcdSetCursor(0, 0); lcdPrint("ERASE ALL?");
                lcdSetCursor(0, 1); lcdPrint("UP=YES DOWN=NO");
                return;
            }
            if (s_menuCursor == MI_AP_MODE) {
                // Set Standalone mode, save and reboot
                systemConfig.standalone = true;
                saveConfig();
                s_menuActive = false;
                lcdClear();
                lcdSetCursor(0, 0); lcdPrint("AP MODE SET!");
                for (int i = 5; i > 0; i--) {
                    lcdSetCursor(0, 1); lcdPrint("Reboot in " + String(i) + "s...");
                    delay(1000);
                }
                ESP.restart();
                return;
            }
            if (s_menuCursor == MI_REBOOT) {
                // 10-second countdown then restart
                s_menuActive = false;
                lcdClear();
                lcdSetCursor(0, 0); lcdPrint("REBOOTING...");
                for (int i = 10; i > 0; i--) {
                    lcdSetCursor(0, 1); lcdPrint("Wait " + String(i) + "s...   ");
                    delay(1000);
                }
                lcdSetCursor(0, 1); lcdPrint("Goodbye!        ");
                delay(500);
                ESP.restart();
                return;
            }
            if (s_menuCursor == MI_EXIT) {
                s_menuActive = false;
                lcdClear();
                enterMarquee();
                return;
            }
            if (s_menuCursor == MI_SENS) {
                s_inSubMenu = true;
                s_subCursor = 0;
            }
            s_showContent = true;
            s_scrollText  = "";
            drawMenuContent();
        }
        if (back) {
            s_menuActive = false;
            lcdClear();
            enterMarquee();
        }
    }
    else {
        // ── Content view ───────────────────────────────────────────────────
        if (back) {
            s_showContent = false;
            s_inSubMenu   = false;
            s_scrollText  = "";
            drawMenuNav();
            return;
        }
        if (s_inSubMenu) {
            if (up   && s_subCursor > 0)                { s_subCursor--; s_scrollText=""; drawMenuContent(); }
            if (down && s_subCursor < s_snapCount - 1)  { s_subCursor++; s_scrollText=""; drawMenuContent(); }
        }
    }
}


// ============================================================
// SECTION 8 — UPDATE (called every loop())
// ============================================================

void displayUpdate()
{
    unsigned long now = millis();

    // ── MENU: auto-exit on inactivity → back to MARQUEE ──────────────────
    if (s_menuActive) {
        if (now - s_lastKeyTime >= MENU_TIMEOUT_MS) {
            s_menuActive  = false;
            s_showContent = false;
            s_inSubMenu   = false;
            s_scrollText  = "";
            s_reveal      = false;
            lcdClear();
            enterMarquee();
            return;
        }
        // Scroll long content strings inside the menu
        if (s_showContent && !s_scrollText.isEmpty()) {
            if (now - s_scrollTime >= SCROLL_MS) {
                s_scrollTime = now;
                s_scrollPos++;
                if (s_scrollPos > (int)s_scrollText.length()) s_scrollPos = 0;
                String padded  = s_scrollText + "                ";
                String visible = padded.substring(s_scrollPos, s_scrollPos + 16);
                lcdSetCursor(0, 1);
                lcdPrint(visible);
            }
        }
        return;
    }

    // ── MARQUEE ───────────────────────────────────────────────────────────
    if (s_idleMode == IM_MARQUEE) {
        // Alert fires → switch to ALERT mode
        if (hasActiveAlerts()) {
            lcdClear();
            enterAlert();
            return;
        }
        if (now - s_mqTime >= MQ_SCROLL_MS) {
            s_mqTime = now;
            if (s_mqText.isEmpty()) {
                s_mqText = buildMarqueeStr();
                s_mqPos  = 0;
                // Refresh static line 0 (hostname may have changed after boot)
                String name = systemConfig.hostname.isEmpty() ? "SENSOR NODE" : systemConfig.hostname;
                lcdSetCursor(0, 0); lcdPrint(name);
            }
            String padded  = s_mqText + "                ";
            String visible = padded.substring(s_mqPos, s_mqPos + 16);
            lcdSetCursor(0, 1);
            lcdPrint(visible);
            s_mqPos++;
            if (s_mqPos > (int)s_mqText.length()) {
                s_mqPos  = 0;
                s_mqText = "";   // rebuild on next wrap so values stay fresh
            }
        }
        return;
    }

    // ── INFO ──────────────────────────────────────────────────────────────
    if (s_idleMode == IM_INFO) {
        // No input for INFO_TIMEOUT_MS → back to MARQUEE
        if (now - s_infoEnterTime >= INFO_TIMEOUT_MS) {
            lcdClear();
            enterMarquee();
            return;
        }
        // Alert fires while user is on INFO → switch to ALERT
        if (hasActiveAlerts()) {
            lcdClear();
            enterAlert();
            return;
        }

        // 1. Periodic Switch (Every 1.5s)
        static unsigned long lastRefresh = 0;
        bool shouldSwitch = (s_snapCount > 0 && now - s_idleLastSwitch >= 1500);
        bool shouldRefreshData = (now - lastRefresh >= 500); // Live update current value

        if (shouldSwitch || shouldRefreshData) {
            if (shouldSwitch) {
                s_idleLastSwitch = now;
                s_idleCursor++;
                if (s_idleCursor >= s_snapCount) s_idleCursor = -1;
            }
            
            lastRefresh = now;

            lcdSetCursor(0, 0);
            if (s_idleCursor == -1) {
                lcdPrint(s_line0);
                lcdSetCursor(0, 1);
                lcdPrint(s_line1);
            } else {
                lcdPrint(s_snaps[s_idleCursor].name);
                lcdSetCursor(0, 1);
                // Show value with 1 decimal point as requested
                String val = String(s_snaps[s_idleCursor].value, 1) + " " + s_snaps[s_idleCursor].unit;
                lcdPrint(val);
            }
        }

        // 2. Sub-menu Live Refresh (Every 500ms)
        if (s_showContent && s_menuCursor == MI_SENS) {
            static unsigned long lastValRefresh = 0;
            if (now - lastValRefresh >= 500) {
                lastValRefresh = now;
                drawMenuContent(); 
            }
        }
        return;
    }

    // ── ALERT ─────────────────────────────────────────────────────────────
    if (s_idleMode == IM_ALERT) {
        // All alerts cleared → back to MARQUEE
        if (!hasActiveAlerts()) {
            lcdClear();
            enterMarquee();
            return;
        }
        if (now - s_alTime >= MQ_SCROLL_MS) {
            s_alTime = now;
            if (s_alText.isEmpty()) {
                s_alText = buildAlertStr();
                s_alPos  = 0;
                lcdSetCursor(0, 0); lcdPrint("!! ALERT !!");
            }
            String padded  = s_alText + "                ";
            String visible = padded.substring(s_alPos, s_alPos + 16);
            lcdSetCursor(0, 1);
            lcdPrint(visible);
            s_alPos++;
            if (s_alPos > (int)s_alText.length()) {
                s_alPos  = 0;
                s_alText = "";   // rebuild on next wrap so new alerts appear
            }
        }
    }
}
