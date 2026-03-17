#include "display_manager.h"
#include "config.h"
#include "node_manager.h"   // ntpSynced(), discoveredNodes
#include "db_sync.h"        // g_syncLastOk, g_syncLastTime
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

static LiquidCrystal lcd(LCD_PIN_RS, LCD_PIN_EN,
                         LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);

// ── Display modes ─────────────────────────────────────────────────────────────
enum DisplayMode : uint8_t {
    MODE_BOOT,            // setup() status messages
    MODE_MARQUEE,         // idle cycling info ticker
    MODE_MAIN_MENU,       // top-level 5-item menu
    MODE_SYSINFO,         // system info pages (hostname/IP/uptime/heap/NTP)
    MODE_NODE_LIST,       // scrollable list of discovered nodes
    MODE_NODE_DETAIL,     // readings for one selected node
    MODE_ALERTS,          // active alert list across all nodes
    MODE_SYNC,            // DB sync status + toggle
    MODE_REBOOT_CONFIRM   // reboot confirmation
};

static DisplayMode   s_mode        = MODE_BOOT;
static unsigned long s_modeEnter   = 0;
static unsigned long s_lastRedraw  = 0;
static bool          s_dirty       = true;

// Boot
static String        s_bootMsg     = "Booting...";
static bool          s_bootDone    = false;
static unsigned long s_bootDoneAt  = 0;

// Marquee
static int           s_slide       = 0;
static unsigned long s_slideNext   = 0;
static const unsigned long SLIDE_MS     = 3000;
static const unsigned long IDLE_MS      = 30000; // menu inactivity → marquee
static const unsigned long REDRAW_MS    = 500;

// Main menu
static const char* const MENU_LABELS[] = {
    "System Info",
    "Nodes",
    "Alerts",
    "DB Sync",
    "Reboot..."
};
static const int MENU_COUNT  = 5;
static int       s_menuSel   = 0;  // highlighted item
static int       s_menuTop   = 0;  // first visible item (2-line window)

// System info pages
static const int SYSINFO_PAGES = 5;
static int       s_sysPage     = 0;

// Nodes
static int s_nodeIdx    = 0;   // index into discoveredNodes
static int s_readingIdx = 0;   // index into node's filtered readings

// Alerts
static int s_alertIdx = 0;

// ── Alert snapshot struct ─────────────────────────────────────────────────────
struct AlertSnap {
    String host;
    String sensor;
    String state;
    float  value;
    String unit;
};

// ── Helpers ───────────────────────────────────────────────────────────────────
static void lcdRow(uint8_t row, const String& text)
{
    lcd.setCursor(0, row);
    String s = text;
    while ((int)s.length() < 16) s += ' ';
    lcd.print(s.substring(0, 16));
}

static void enterMode(DisplayMode m)
{
    s_mode      = m;
    s_modeEnter = millis();
    s_dirty     = true;
}

static String fmtUptime()
{
    unsigned long sec = millis() / 1000UL;
    unsigned long h   = sec / 3600; sec %= 3600;
    unsigned long m   = sec / 60;   sec %= 60;
    if (h > 0) return String(h) + "h " + String(m) + "m";
    if (m > 0) return String(m) + "m " + String(sec) + "s";
    return String(sec) + "s";
}

static String fmtAgo(uint32_t epochThen)
{
    if (!epochThen) return "never";
    uint32_t now = ntpSynced() ? (uint32_t)time(nullptr)
                               : (uint32_t)(millis() / 1000UL);
    uint32_t d = (now > epochThen) ? (now - epochThen) : 0;
    if (d < 60)   return String(d) + "s ago";
    if (d < 3600) return String(d / 60) + "m ago";
    return String(d / 3600) + "h ago";
}

static int countOnline()
{
    int n = 0;
    for (const auto& nd : discoveredNodes) if (nd.online) n++;
    return n;
}

static std::vector<AlertSnap> gatherAlerts()
{
    std::vector<AlertSnap> out;
    for (const auto& nd : discoveredNodes) {
        for (const auto& r : nd.lastReadings) {
            if (r.alertState == "ok" || r.type == "relay") continue;
            AlertSnap a;
            a.host   = nd.hostname;
            a.sensor = r.name.isEmpty() ? r.sensorId : r.name;
            a.state  = r.alertState;
            a.value  = r.value;
            a.unit   = r.unit;
            out.push_back(a);
        }
    }
    return out;
}

// ── Draw functions ────────────────────────────────────────────────────────────

static void drawBoot()
{
    lcdRow(0, "ISMS Central");
    lcdRow(1, s_bootMsg);
}

static void drawMarquee()
{
    int online = countOnline();
    int total  = (int)discoveredNodes.size();
    auto alerts = gatherAlerts();

    switch (s_slide % 4) {
        case 0:
            lcdRow(0, "ISMS Central");
            lcdRow(1, systemConfig.standalone
                      ? "Hotspot mode"
                      : WiFi.localIP().toString());
            break;
        case 1:
            lcdRow(0, "Nodes online:");
            lcdRow(1, String(online) + "/" + String(total) + " online");
            break;
        case 2:
            lcdRow(0, "Active alerts:");
            lcdRow(1, alerts.empty()
                      ? "None — all ok"
                      : String((int)alerts.size()) + " alert(s)!");
            break;
        case 3:
            lcdRow(0, "DB Sync:");
            if (!systemConfig.syncEnabled) {
                lcdRow(1, "Disabled");
            } else {
                lcdRow(1, g_syncLastOk
                          ? ("OK " + fmtAgo(g_syncLastTime))
                          : (g_syncLastTime ? "FAILED" : "Pending..."));
            }
            break;
    }
}

static void drawMainMenu()
{
    for (int line = 0; line < 2; line++) {
        int idx = s_menuTop + line;
        if (idx >= MENU_COUNT) { lcdRow(line, ""); continue; }

        String text = (s_menuSel == idx ? ">" : " ");
        text += MENU_LABELS[idx];

        // Append live counts
        if (idx == 1) { // Nodes
            text += "(" + String((int)discoveredNodes.size()) + ")";
        } else if (idx == 2) { // Alerts
            int ac = 0;
            for (const auto& nd : discoveredNodes)
                for (const auto& r : nd.lastReadings)
                    if (r.alertState != "ok" && r.type != "relay") ac++;
            if (ac) text += "(" + String(ac) + ")";
        }
        lcdRow(line, text);
    }
}

static void drawSysInfo()
{
    switch (s_sysPage) {
        case 0:
            lcdRow(0, "Hostname:");
            lcdRow(1, systemConfig.hostname);
            break;
        case 1:
            lcdRow(0, "IP Address:");
            lcdRow(1, systemConfig.standalone
                      ? WiFi.softAPIP().toString()
                      : WiFi.localIP().toString());
            break;
        case 2:
            lcdRow(0, "Uptime:");
            lcdRow(1, fmtUptime());
            break;
        case 3:
            lcdRow(0, "Free heap:");
            lcdRow(1, String(ESP.getFreeHeap() / 1024) + " KB free");
            break;
        case 4:
            lcdRow(0, "NTP:");
            lcdRow(1, ntpSynced() ? "Synced OK" : "Not synced");
            break;
    }
}

static void drawNodeList()
{
    if (discoveredNodes.empty()) {
        lcdRow(0, "No nodes found");
        lcdRow(1, "* = Back");
        return;
    }
    int n = (int)discoveredNodes.size();
    s_nodeIdx = constrain(s_nodeIdx, 0, n - 1);
    const auto& nd = discoveredNodes[s_nodeIdx];

    // Line 0: index + online state
    String hdr = "Node " + String(s_nodeIdx + 1) + "/" + String(n);
    hdr += nd.online ? " [ON] " : " [OFF]";
    lcdRow(0, hdr);

    // Line 1: hostname (9 chars) + IP
    String host = nd.hostname;
    if ((int)host.length() > 7) host = host.substring(0, 7);
    lcdRow(1, host + " " + nd.ip);
}

static void drawNodeDetail()
{
    if (discoveredNodes.empty() || s_nodeIdx >= (int)discoveredNodes.size()) {
        enterMode(MODE_NODE_LIST);
        return;
    }
    const auto& nd = discoveredNodes[s_nodeIdx];

    // Collect non-relay readings into a flat array
    std::vector<const NodeReading*> rs;
    for (const auto& r : nd.lastReadings)
        if (r.type != "relay") rs.push_back(&r);

    if (rs.empty()) {
        lcdRow(0, nd.hostname.substring(0, 16));
        lcdRow(1, "No readings yet");
        return;
    }

    s_readingIdx = constrain(s_readingIdx, 0, (int)rs.size() - 1);
    const NodeReading* r = rs[s_readingIdx];

    // Line 0: sensor name + position counter
    String name = r->name.isEmpty() ? r->sensorId : r->name;
    if ((int)name.length() > 9) name = name.substring(0, 9);
    lcdRow(0, name + " " + String(s_readingIdx + 1) + "/" + String((int)rs.size()));

    // Line 1: value + unit + alert badge
    String val   = String(r->value, 1) + r->unit;
    String badge = (r->alertState == "ok") ? "ok" : ("!" + r->alertState);
    lcdRow(1, val + " [" + badge + "]");
}

static void drawAlerts()
{
    auto alerts = gatherAlerts();
    if (alerts.empty()) {
        lcdRow(0, "No alerts");
        lcdRow(1, "All sensors ok");
        return;
    }
    s_alertIdx = constrain(s_alertIdx, 0, (int)alerts.size() - 1);
    const auto& a = alerts[s_alertIdx];

    // Line 0: alert N/total
    lcdRow(0, "Alert " + String(s_alertIdx + 1) + "/" + String((int)alerts.size()));

    // Line 1: host:sensor state val
    String host = a.host;
    if ((int)host.length() > 5) host = host.substring(0, 5);
    String snsr = a.sensor;
    if ((int)snsr.length() > 5) snsr = snsr.substring(0, 5);
    lcdRow(1, host + ":" + snsr + " " + a.state);
}

static void drawSync()
{
    // Line 0: sync on/off with toggle hint
    lcdRow(0, systemConfig.syncEnabled ? "Sync:ON  [#=off]" : "Sync:OFF [#=on] ");

    // Line 1: last result + age
    if (g_syncLastTime) {
        lcdRow(1, String(g_syncLastOk ? "OK " : "ERR ") + fmtAgo(g_syncLastTime));
    } else {
        lcdRow(1, "Never synced");
    }
}

static void drawRebootConfirm()
{
    lcdRow(0, "Reboot? #=Yes");
    lcdRow(1, "        *=No");
}

// ── Public API ────────────────────────────────────────────────────────────────

void displayInit()
{
    lcd.begin(16, 2);
    lcd.clear();
    lcdRow(0, "ISMS Central");
    lcdRow(1, "Booting...");
    s_mode      = MODE_BOOT;
    s_modeEnter = millis();
    Serial.println("[DISP] 1602A LCD init OK  (center node 4-bit)");
    Serial.printf ("[DISP] Pins: RS=G%d EN=G%d D4=G%d D5=G%d D6=G%d D7=G%d\n",
                   LCD_PIN_RS, LCD_PIN_EN,
                   LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);
}

void displayPrint(const String& msg)
{
    if (s_mode == MODE_BOOT) {
        s_bootMsg = msg;
        s_dirty   = true;
        if (msg == "RDY") {
            s_bootDone  = true;
            s_bootDoneAt = millis();
        }
    }
}

void displayUpdate()
{
    unsigned long now = millis();

    // After "RDY" shown for 2 s, transition to marquee automatically
    if (s_mode == MODE_BOOT && s_bootDone && (now - s_bootDoneAt) > 2000) {
        s_slide     = 0;
        s_slideNext = now + SLIDE_MS;
        enterMode(MODE_MARQUEE);
    }

    // Advance marquee slide
    if (s_mode == MODE_MARQUEE && now >= s_slideNext) {
        s_slide++;
        s_slideNext = now + SLIDE_MS;
        s_dirty     = true;
    }

    // Idle timeout: return to marquee from any menu after IDLE_MS of no input
    if (s_mode != MODE_BOOT && s_mode != MODE_MARQUEE &&
        s_mode != MODE_REBOOT_CONFIRM) {
        if ((now - s_modeEnter) > IDLE_MS) {
            enterMode(MODE_MARQUEE);
        }
    }

    // Throttle redraws; sys info and node detail refresh every REDRAW_MS
    if (!s_dirty && (now - s_lastRedraw) < REDRAW_MS) return;
    s_dirty      = false;
    s_lastRedraw = now;

    switch (s_mode) {
        case MODE_BOOT:           drawBoot();          break;
        case MODE_MARQUEE:        drawMarquee();       break;
        case MODE_MAIN_MENU:      drawMainMenu();      break;
        case MODE_SYSINFO:        drawSysInfo();       break;
        case MODE_NODE_LIST:      drawNodeList();      break;
        case MODE_NODE_DETAIL:    drawNodeDetail();    break;
        case MODE_ALERTS:         drawAlerts();        break;
        case MODE_SYNC:           drawSync();          break;
        case MODE_REBOOT_CONFIRM: drawRebootConfirm(); break;
    }
}

bool displayInMenu()
{
    return s_mode != MODE_BOOT && s_mode != MODE_MARQUEE;
}

void displayHandleKey(char key)
{
    s_modeEnter = millis();   // reset idle timeout
    s_dirty     = true;

    switch (s_mode) {

        // ── Boot ─────────────────────────────────────────────────────────────
        case MODE_BOOT:
            // Any key skips the remaining boot display
            s_slide     = 0;
            s_slideNext = millis() + SLIDE_MS;
            enterMode(MODE_MARQUEE);
            break;

        // ── Marquee ──────────────────────────────────────────────────────────
        case MODE_MARQUEE:
            s_menuSel = 0;
            s_menuTop = 0;
            enterMode(MODE_MAIN_MENU);
            break;

        // ── Main menu ────────────────────────────────────────────────────────
        case MODE_MAIN_MENU:
            if (key == '2' || key == 'A') {          // UP
                if (s_menuSel > 0) {
                    s_menuSel--;
                    if (s_menuSel < s_menuTop) s_menuTop = s_menuSel;
                }
            } else if (key == '8' || key == 'B') {  // DOWN
                if (s_menuSel < MENU_COUNT - 1) {
                    s_menuSel++;
                    if (s_menuSel > s_menuTop + 1) s_menuTop = s_menuSel - 1;
                }
            } else if (key == '#' || key == 'C') {  // SELECT
                switch (s_menuSel) {
                    case 0: s_sysPage = 0;   enterMode(MODE_SYSINFO);    break;
                    case 1: s_nodeIdx = 0;   enterMode(MODE_NODE_LIST);  break;
                    case 2: s_alertIdx = 0;  enterMode(MODE_ALERTS);     break;
                    case 3:                  enterMode(MODE_SYNC);        break;
                    case 4:                  enterMode(MODE_REBOOT_CONFIRM); break;
                }
            } else if (key == '*' || key == 'D') {  // BACK → marquee
                enterMode(MODE_MARQUEE);
            }
            break;

        // ── System info ──────────────────────────────────────────────────────
        case MODE_SYSINFO:
            if      (key == '2' || key == 'A')           s_sysPage = max(0, s_sysPage - 1);
            else if (key == '8' || key == 'B')           s_sysPage = min(SYSINFO_PAGES - 1, s_sysPage + 1);
            else if (key == '*' || key == 'D' ||
                     key == '#' || key == 'C')           enterMode(MODE_MAIN_MENU);
            break;

        // ── Node list ────────────────────────────────────────────────────────
        case MODE_NODE_LIST:
            if (key == '2' || key == 'A') {
                if (s_nodeIdx > 0) s_nodeIdx--;
            } else if (key == '8' || key == 'B') {
                if (s_nodeIdx < (int)discoveredNodes.size() - 1) s_nodeIdx++;
            } else if (key == '#' || key == 'C') {
                // Enter readings view for selected node
                if (!discoveredNodes.empty()) {
                    s_readingIdx = 0;
                    enterMode(MODE_NODE_DETAIL);
                }
            } else if (key == '5') {
                // Force a fresh poll of the currently selected node
                if (!discoveredNodes.empty() && s_nodeIdx < (int)discoveredNodes.size()) {
                    addNodeByIP(discoveredNodes[s_nodeIdx].ip);
                }
            } else if (key == '*' || key == 'D') {
                enterMode(MODE_MAIN_MENU);
            }
            break;

        // ── Node detail (readings) ────────────────────────────────────────────
        case MODE_NODE_DETAIL:
            if      (key == '2' || key == 'A')  { if (s_readingIdx > 0) s_readingIdx--; }
            else if (key == '8' || key == 'B')  { s_readingIdx++; }  // clamped in draw
            else if (key == '*' || key == 'D' ||
                     key == '#' || key == 'C')  { enterMode(MODE_NODE_LIST); }
            break;

        // ── Alerts ───────────────────────────────────────────────────────────
        case MODE_ALERTS:
            if      (key == '2' || key == 'A')  { if (s_alertIdx > 0) s_alertIdx--; }
            else if (key == '8' || key == 'B')  { s_alertIdx++; }    // clamped in draw
            else if (key == '*' || key == 'D' ||
                     key == '#' || key == 'C')  { enterMode(MODE_MAIN_MENU); }
            break;

        // ── DB Sync ──────────────────────────────────────────────────────────
        case MODE_SYNC:
            if (key == '#' || key == 'C') {
                // Toggle sync on/off and persist immediately
                systemConfig.syncEnabled = !systemConfig.syncEnabled;
                saveConfig();
            } else if (key == '*' || key == 'D') {
                enterMode(MODE_MAIN_MENU);
            }
            break;

        // ── Reboot confirm ───────────────────────────────────────────────────
        case MODE_REBOOT_CONFIRM:
            if (key == '#' || key == 'C') {
                lcdRow(0, "Rebooting...");
                lcdRow(1, "");
                delay(500);
                ESP.restart();
            } else if (key == '*' || key == 'D') {
                enterMode(MODE_MAIN_MENU);
            }
            break;
    }
}

void displayTriggerRebootConfirm()
{
    enterMode(MODE_REBOOT_CONFIRM);
}
