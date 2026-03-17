#include "keypad_handler.h"
#include "display_manager.h"

static const uint8_t ROWS[4] = { KP_ROW0, KP_ROW1, KP_ROW2, KP_ROW3 };
static const uint8_t COLS[4] = { KP_COL0, KP_COL1, KP_COL2, KP_COL3 };

static const char KEYMAP[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' }
};

static const unsigned long DEBOUNCE_MS = 50;
static const unsigned long HOLD_D_MS   = 2000;  // hold 'D' → reboot confirm

static char          s_lastRaw    = '\0';
static char          s_stableKey  = '\0';
static unsigned long s_changeTime = 0;
static unsigned long s_holdStart  = 0;
static bool          s_holdFired  = false;

// ── Matrix scan ───────────────────────────────────────────────────────────────
static char scanRaw()
{
    for (int r = 0; r < 4; r++) {
        digitalWrite(ROWS[r], LOW);
        for (int c = 0; c < 4; c++) {
            if (digitalRead(COLS[c]) == LOW) {
                digitalWrite(ROWS[r], HIGH);
                return KEYMAP[r][c];
            }
        }
        digitalWrite(ROWS[r], HIGH);
    }
    return '\0';
}

// ── Public API ────────────────────────────────────────────────────────────────
void keypadInit()
{
    for (int r = 0; r < 4; r++) {
        pinMode(ROWS[r], OUTPUT);
        digitalWrite(ROWS[r], HIGH);
    }
    // GPIO 33/32 have internal pull-ups; 34/35 are input-only — no pull-up.
    pinMode(KP_COL0, INPUT_PULLUP);
    pinMode(KP_COL1, INPUT_PULLUP);
    pinMode(KP_COL2, INPUT);   // external 10 kΩ to 3.3 V required
    pinMode(KP_COL3, INPUT);   // external 10 kΩ to 3.3 V required

    Serial.println("[BTN] 4x4 keypad init OK  R0-R3=G14/27/26/25  C0-C3=G33/32/34/35");
    Serial.println("[BTN] NOTE: GPIO 34/35 (cols 2-3) need external 10k pullups to 3.3V");
}

char keypadScan()
{
    unsigned long now = millis();
    char raw = scanRaw();

    // Debounce
    if (raw != s_lastRaw) {
        s_lastRaw    = raw;
        s_changeTime = now;
    }

    if ((now - s_changeTime) >= DEBOUNCE_MS && raw != s_stableKey) {
        char prev   = s_stableKey;
        s_stableKey = raw;

        if (raw != '\0') {
            // Press edge
            s_holdStart = now;
            s_holdFired = false;
            displayHandleKey(raw);
            return raw;
        } else {
            // Release edge — cancel hold tracking
            s_holdStart = 0;
            s_holdFired = false;
        }
    }

    // Hold 'D' for 2 s → reboot confirm screen
    if (s_stableKey == 'D' && !s_holdFired && s_holdStart != 0) {
        if ((now - s_holdStart) >= HOLD_D_MS) {
            s_holdFired = true;
            displayTriggerRebootConfirm();
        }
    }

    return '\0';
}
