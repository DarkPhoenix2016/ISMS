#ifndef KEYPAD_HANDLER_H
#define KEYPAD_HANDLER_H

#include <Arduino.h>

// ── 3-button pin assignments (active-LOW, other side to GND) ─────────────────
#define BTN_UP    17   // G17 — Button 1: navigate UP
#define BTN_DOWN  16   // G16 — Button 2: navigate DOWN
#define BTN_SEL    4   // G4  — Button 3: SELECT (also BACK when in content view)

// Call once in setup().
void keypadInit();

// Call every loop() — non-blocking.
// Returns the key character on the press EDGE only (not held).
// Also calls displayHandleKey() internally for menu navigation.
// Returns '\0' if no new key pressed.
//   BTN_UP   → 'A'
//   BTN_DOWN → 'B'
//   BTN_SEL  → 'D'
char keypadScan();

#endif // KEYPAD_HANDLER_H
