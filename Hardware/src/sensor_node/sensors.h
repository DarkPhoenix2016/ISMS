#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "config.h"   // DynamicSensor — needed for relay function signatures

// Must be called once in setup() before any sensor reads.
// Initialises hardware pins and performs ADC calibration.
void initSensors();

// Cleanly removes all sensor instances and clears state maps.
void clearSensors();

// Non-blocking main-loop dispatcher.  Call every iteration of loop().
void readDynamicSensors();
void updateDiscovery();

// Individual sensor read functions (also callable directly for diagnostics).
float  readTemperature(int pin);
float  readGas(int pin, float loadResistance);
float  readParticle(int analogPin, int ledPin);
float  readCurrent(int pin);
int    readVibration(int pin);

// Returns the cached air-quality classification string for a gas sensor pin.
String getAirStatus(int pin);

// Relay output control (CW-020 on GPIO 25, active-HIGH).
// setRelayState updates both the hardware pin and s.lastValue.
void setRelayState(DynamicSensor& s, bool on);
bool getRelayState(const DynamicSensor& s);

#endif