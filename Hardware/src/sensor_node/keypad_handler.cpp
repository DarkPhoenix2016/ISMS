#include "keypad_handler.h"
#include "display_manager.h"   // displayHandleKey()

// ── Button table ─────────────────────────────────────────────────────────────
static const uint8_t BTN_PINS[3]  = { BTN_UP,  BTN_DOWN, BTN_SEL  };
static const char    BTN_CHARS[3] = { 'A',     'B',      'D'      };

// ── Per-button debounce / hold state ─────────────────────────────────────────
static const unsigned long DEBOUNCE_MS = 50;
static const unsigned long HOLD_MS     = 600;  // hold threshold for reveal
static bool          s_lastRaw[3]    = { false, false, false };
static bool          s_stable[3]     = { false, false, false };
static unsigned long s_changeTime[3] = { 0, 0, 0 };
static unsigned long s_pressTime[3]  = { 0, 0, 0 };  // when button became stably pressed
static bool          s_holdFired[3]  = { false, false, false };

// ── Chord state: SELECT (idx 2) + DOWN (idx 1) held 2 s → reboot confirm ─────
static const unsigned long CHORD_MS    = 2000;
static unsigned long       s_chordTime  = 0;    // when both first pressed together
static bool                s_chordFired = false;

// ── Public API ────────────────────────────────────────────────────────────────

void keypadInit()
{
    for (int i = 0; i < 3; i++) {
        pinMode(BTN_PINS[i], INPUT_PULLUP);
        s_lastRaw[i]    = false;
        s_stable[i]     = false;
        s_changeTime[i] = 0;
        s_pressTime[i]  = 0;
        s_holdFired[i]  = false;
    }
    Serial.println("[BTN] 3-button init OK  UP=G17  DOWN=G16  SEL=G4");
}

// Returns the key character once per physical press (edge detection + debounce).
// Forwards every confirmed press to displayHandleKey().
char keypadScan()
{
    unsigned long now = millis();

    for (int i = 0; i < 3; i++) {
        bool pressed = (digitalRead(BTN_PINS[i]) == LOW);   // active-LOW

        if (pressed != s_lastRaw[i]) {
            s_lastRaw[i]    = pressed;
            s_changeTime[i] = now;                           // restart debounce timer
        }

        if ((now - s_changeTime[i]) >= DEBOUNCE_MS) {
            if (pressed != s_stable[i]) {
                s_stable[i] = pressed;
                if (pressed) {                               // press edge
                    s_pressTime[i] = now;
                    s_holdFired[i] = false;
                    displayHandleKey(BTN_CHARS[i]);
                    return BTN_CHARS[i];
                } else {                                     // release edge
                    if (s_holdFired[i]) {
                        displaySetReveal(false);             // stop revealing on release
                    }
                    s_holdFired[i] = false;
                }
            }

            // Hold detection: BTN_DOWN (index 1) held for HOLD_MS → reveal secrets
            if (i == 1 && s_stable[i] && !s_holdFired[i] &&
                (now - s_pressTime[i]) >= HOLD_MS) {
                s_holdFired[i] = true;
                displaySetReveal(true);
            }
        }
    }
    // ── Chord: both DOWN (1) and SEL (2) must be stably pressed ──────────────
    if (s_stable[1] && s_stable[2]) {
        if (s_chordTime == 0) s_chordTime = now;
        if (!s_chordFired && (now - s_chordTime) >= CHORD_MS) {
            s_chordFired = true;
            displayTriggerRebootConfirm();
        }
    } else {
        s_chordTime  = 0;
        s_chordFired = false;
    }

    return '\0';
}
