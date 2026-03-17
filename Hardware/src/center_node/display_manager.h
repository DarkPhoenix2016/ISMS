#ifndef CENTER_DISPLAY_MANAGER_H
#define CENTER_DISPLAY_MANAGER_H

#include <Arduino.h>

// ── HD44780 1602A direct 4-bit wiring ────────────────────────────────────────
#define LCD_PIN_RS  23
#define LCD_PIN_EN  22
#define LCD_PIN_D4  21
#define LCD_PIN_D5  19
#define LCD_PIN_D6  18
#define LCD_PIN_D7   5

// ── Public API ────────────────────────────────────────────────────────────────

// Call once at the very start of setup() so boot messages appear on screen.
void displayInit();

// Call every loop() — handles marquee advance, idle timeout, LCD refresh.
void displayUpdate();

// Queue a single-line boot status message (shown during setup phase).
void displayPrint(const String& msg);

// Called by keypad_handler when a key press edge is detected.
void displayHandleKey(char key);

// Returns true while any menu screen is active (not marquee/boot).
bool displayInMenu();

// Immediately switch to the reboot-confirmation screen (called on long-hold).
void displayTriggerRebootConfirm();

#endif // CENTER_DISPLAY_MANAGER_H
