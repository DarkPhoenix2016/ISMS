#ifndef CENTER_KEYPAD_HANDLER_H
#define CENTER_KEYPAD_HANDLER_H

#include <Arduino.h>

// ── 4×4 matrix keypad pin assignments ────────────────────────────────────────
// Rows are driven LOW one at a time (OUTPUT).
// Columns are read (INPUT_PULLUP for GPIO 33/32; INPUT for GPIO 34/35).
// !! GPIO 34 and 35 are input-only — they have no internal pull-up. Add
//    external 10 kΩ resistors (Col2/Col3 to 3.3 V) for reliable scanning. !!

#define KP_ROW0  14   // R0 — top row:    1 2 3 A
#define KP_ROW1  27   // R1              : 4 5 6 B
#define KP_ROW2  26   // R2              : 7 8 9 C
#define KP_ROW3  25   // R3 — bottom row: * 0 # D

#define KP_COL0  33   // C0 — 1 4 7 *   (internal pull-up ok)
#define KP_COL1  32   // C1 — 2 5 8 0   (internal pull-up ok)
#define KP_COL2  13   // C2 — 3 6 9 #   (external 10 kΩ to 3.3 V required)
#define KP_COL3  4   // C3 — A B C D   (external 10 kΩ to 3.3 V required)

// Key map (row × col):
//   '1''2''3''A'
//   '4''5''6''B'
//   '7''8''9''C'
//   '*''0''#''D'
//
// Navigation convention used by display_manager:
//   '2' / 'A' → UP       '8' / 'B' → DOWN
//   '#' / 'C' → SELECT   '*' / 'D' → BACK
//   Hold 'D' 2 s         → reboot confirm

void keypadInit();

// Call every loop() — non-blocking, returns pressed key char on edge, '\0' if none.
// Also calls displayHandleKey() internally for all navigation.
char keypadScan();

#endif // CENTER_KEYPAD_HANDLER_H
