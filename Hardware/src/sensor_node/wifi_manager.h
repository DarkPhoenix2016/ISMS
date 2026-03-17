#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

// Starts a recovery-mode hotspot with a chip-derived unique password.
void startConfigHotspot();

// Starts the user-facing standalone hotspot (SSID = "Node-<hostname>").
void startStandaloneHotspot();

// Attempts to join the configured SSID; falls back to startConfigHotspot() on failure.
void connectToWiFi();

// Top-level entry point called from setup().  Selects the appropriate mode.
void startWiFi();

#endif