#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>

// ── HD44780 1602A direct 4-bit wiring (no shift register) ───────────────────
#define LCD_PIN_RS  23   // G23 → LCD RS
#define LCD_PIN_EN  22   // G22 → LCD EN
#define LCD_PIN_D4  21   // G21 → LCD D4
#define LCD_PIN_D5  19   // G19 → LCD D5
#define LCD_PIN_D6  18   // G18 → LCD D6
#define LCD_PIN_D7   5   // G5  → LCD D7

// ── Public API ───────────────────────────────────────────────────────────────

// Call once in setup() — must be the very first peripheral init so
// status messages during WiFi/sensor boot appear on the display.
void displayInit();

// Call every loop() iteration — handles mux refresh, scroll, menu timeout.
// Lightweight: only shifts hardware every 5 ms; otherwise just checks timers.
void displayUpdate();

// Queue a status message for boot-mode scrolling (safe from any .cpp file).
// Long strings scroll automatically. Replaces any pending message immediately.
void displayPrint(const String& msg);

// Feed a sensor value into the "Sensor Readings" sub-menu snapshot cache.
// Call from readDynamicSensors() or wherever sensor values are updated.
void displaySetSensorSnapshot(const String& id, const String& name,
                              float value,       const String& unit,
                              uint32_t interval);

// Called by keypad_handler.cpp when a key press edge is detected.
// Routes the key to menu navigation or boots into menu mode.
void displayHandleKey(char key);

// Returns true while the menu is active (keypad keeps display in menu mode).
bool displayInMenu();

// Hold-to-reveal: call with true while BTN_DOWN is held on the PASS/KEY screen,
// false when released. Immediately redraws the content line.
void displaySetReveal(bool on);

// Hardware chord shortcut: hold SELECT+DOWN for 2 s.
// Shows a reboot confirmation screen.
// UP button confirms (ESP.restart()), DOWN cancels (back to MARQUEE).
void displayTriggerRebootConfirm();

#endif // DISPLAY_MANAGER_H
