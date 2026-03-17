#include "sensors.h"
#include "config.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_adc_cal.h>
#include <map>
#include <cmath>
#include <algorithm>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "display_manager.h"


// ============================================================
// SECTION 1 — COMPILE-TIME CONSTANTS
// ============================================================

static const float SUPPLY_VOLTAGE    = 5.0f;
static const float ADC_MAX           = 4095.0f;
static const adc_atten_t  ADC_ATTEN  = ADC_ATTEN_DB_12;
static const adc_bits_width_t ADC_BITS = ADC_WIDTH_BIT_12;

// MQ sampling
static const int  MQ_RS_SAMPLES     = 9;
static const int  MQ_CALIB_SAMPLES  = 30;

// MQ sensor load resistances (kOhm)
static const float MQ2_LOAD_OHM     = 5.0f;
static const float MQ135_LOAD_OHM   = 10.0f;

// MQ clean-air Rs/R0 ratios (datasheet)
static const float MQ2_CLEAN_RATIO   = 9.83f;
static const float MQ135_CLEAN_RATIO = 3.60f;

// Adaptive drift thresholds
static const float MQ2_ADAPT_CLEAN_MIN   = 6.0f;
static const float MQ135_ADAPT_CLEAN_MIN = 3.0f;

// Air quality classification threshold (Rs/R0)
static const float AQ_THRESH = 2.5f;

// DS18B20 conversion time for 12-bit resolution (ms)
static const unsigned long DS18B20_CONV_MS = 750UL;

// ACS712 sensitivity (mV/A).
// Change to match module variant: 185 = 5A, 100 = 20A, 66 = 30A
static const float ACS712_MV_PER_A   = 66.0f;

// ACS712 dead-band.
// Raised to 0.30 A to eliminate phantom 0.2–0.4 A noise-floor readings.
// The self-zeroing RMS approach should bring the noise well below this.
static const float ACS712_DEADBAND_A = 0.30f;

// ACS712 sampling.
// 200 samples at 500 µs spacing = 100 ms total.
// 100 ms covers 5 complete cycles of 50 Hz and 6 cycles of 60 Hz,
// giving a stable RMS estimate regardless of grid frequency.
static const int   CURRENT_SAMPLES   = 200;

// Sharp GP2Y1010AU0F constants (datasheet-derived).
//   Clean-air output voltage  ≈ 0.9 V
//   Sensitivity               ≈ 0.5 V per 100 µg/m³
//   => density (µg/m³)        = (Vout - 0.9) / 0.5 * 100 = (Vout - 0.9) * 200
static const float PARTICLE_BASELINE_V  = 0.9f;
static const float PARTICLE_SENS        = 200.0f;   // µg/m³ per volt above baseline

// GP2Y1010AU0F LED timing (µs) — must not be changed; from datasheet
static const int PARTICLE_LED_ON_US  = 280;
static const int PARTICLE_LED_OFF_US = 40;
static const int PARTICLE_CYCLE_US   = 9680;
static const int PARTICLE_SAMPLES    = 5;

// 801S Vibration Sensor Switch — behaviour notes:
//   The 801S contains a spring-and-ball contact that closes momentarily
//   on vibration/shoc k.  With themodule's internal pull-up the DO line
//   goes LOW for as little as 1–5 ms per event.
//
//   VIB_DEBOUNCE_MS — minimum stable-low time required to accept an event.
//                     Kept short (1 ms) because the 801S contact bounce is
//                     brief; the old 20 ms window was missing real events.
//
//   VIB_HOLD_MS     — once a validated event is detected the "active" state
//                     is held for this many ms.  Without a hold window the
//                     dashboard 2-second poll would almost never see the
//                     momentary pulse.
static const unsigned long VIB_DEBOUNCE_MS = 1UL;
static const unsigned long VIB_HOLD_MS     = 100UL;

// Stuck-sensor detection.
// If a sensor returns the same value (within STALE_EPSILON) for
// STALE_LIMIT consecutive reads, the dispatcher logs a warning and
// attempts a soft re-initialisation of that sensor.
static const int   STALE_LIMIT   = 50;
static const float STALE_EPSILON = 0.001f;


// ============================================================
// SECTION 2 — MODULE-LEVEL STATE
// ============================================================

// ADC factory calibration
static esp_adc_cal_characteristics_t s_adcChars;
static bool                          s_adcCalOk = false;

// MQ baseline / adaptive drift state
static std::map<int, float>  gasBaselines;
static std::map<int, float>  gasAdaptiveMin;
static std::map<int, String> airStatusCache;

// DS18B20 instances and two-phase request tracking
static std::map<int, OneWire*>           oneWireInstances;
static std::map<int, DallasTemperature*> tempSensorInstances;
static std::map<int, unsigned long>      tempRequestTime;
static std::map<int, bool>              tempRequestPending;

// 801S vibration — three independent per-pin timers:
//   vibRawState     — debounced raw input state (0/1)
//   vibLastChange   — millis() of last debounced state transition
//   vibActiveUntil  — millis() timestamp when the hold-latch expires
static std::map<int, int>           vibRawState;
static std::map<int, unsigned long> vibLastChange;
static std::map<int, unsigned long> vibActiveUntil;

// Stuck-sensor counters, keyed by sensor ID string
static std::map<String, int>   staleCount;
static std::map<String, float> stalePrev;


// ============================================================
// SECTION 3 — ADC HELPERS
// ============================================================

static void initADCCal()
{
    esp_adc_cal_value_t calType = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN, ADC_BITS, 1100, &s_adcChars);
    s_adcCalOk = (calType != ESP_ADC_CAL_VAL_NOT_SUPPORTED);

    const char* calName =
        calType == ESP_ADC_CAL_VAL_EFUSE_TP   ? "Two-Point eFuse" :
        calType == ESP_ADC_CAL_VAL_EFUSE_VREF ? "Vref eFuse"      :
                                                 "Default (no eFuse)";
    Serial.printf("  [ADC] Calibration: %s  ok=%d\n", calName, (int)s_adcCalOk);
}

static inline float adcToVolts(int raw)
{
    if (s_adcCalOk) {
        uint32_t mV = esp_adc_cal_raw_to_voltage((uint32_t)raw, &s_adcChars);
        return mV * 0.001f;
    }
    return raw * (3.3f / ADC_MAX);
}

static float medianADCVolts(int pin, int n = MQ_RS_SAMPLES)
{
    if (n < 1)  n = 1;
    if (n > 31) n = 31;
    float buf[31];
    for (int i = 0; i < n; i++) {
        buf[i] = adcToVolts(analogRead(pin));
        delayMicroseconds(500);
    }
    std::sort(buf, buf + n);
    return buf[n / 2];
}


// ============================================================
// SECTION 4 — RESISTANCE MEASUREMENT (MQ sensors)
// ============================================================

static float measureResistance(int analogPin, float loadResistance)
{
    float vout = medianADCVolts(analogPin);
    if (vout < 0.001f) vout = 0.001f;
    return ((SUPPLY_VOLTAGE - vout) / vout) * loadResistance;
}


// ============================================================
// SECTION 5 — MQ CALIBRATION
// ============================================================

static float calibrateGasSensor(int pin, float load, float cleanFactor)
{
    Serial.printf("  [CAL] MQ on GPIO %d — sampling R0...\n", pin);
    float sum = 0.0f;
    for (int i = 0; i < MQ_CALIB_SAMPLES; i++) {
        sum += measureResistance(pin, load);
        delay(5);
    }
    float r0 = (sum / MQ_CALIB_SAMPLES) / cleanFactor;
    Serial.printf("  [CAL] GPIO %d  R0 = %.4f kOhm\n", pin, r0);
    return r0;
}


// ============================================================
// SECTION 6 — ADAPTIVE BASELINE DRIFT (MQ sensors)
// ============================================================

static void applyAdaptiveDrift(int pin, float ratio,
                               float cleanMin, float cleanFactor)
{
    float& adaptMin = gasAdaptiveMin[pin];
    float& baseline = gasBaselines[pin];
    if (ratio > cleanMin && ratio < adaptMin) {
        adaptMin = ratio;
        float newEstimate = baseline * ratio / cleanFactor;
        baseline = baseline * 0.9f + newEstimate * 0.1f;
        Serial.printf("  [DRIFT] GPIO %d  R0 -> %.4f kOhm\n", pin, baseline);
    }
}


// ============================================================
// SECTION 7 — AIR QUALITY CLASSIFICATION
// ============================================================

static String classifyAir(float mq2Ratio, float mq135Ratio)
{
    bool mq2Clean   = (mq2Ratio   >= AQ_THRESH);
    bool mq135Clean = (mq135Ratio >= AQ_THRESH);
    if (!mq2Clean &&  mq135Clean) return "Combustible Gas Detected";
    if (!mq2Clean && !mq135Clean) return "Smoke / VOC Presence";
    if ( mq2Clean && !mq135Clean) return "Air Pollution Detected";
    return "Clean Air";
}

String getAirStatus(int pin)
{
    auto it = airStatusCache.find(pin);
    return (it != airStatusCache.end()) ? it->second : "Unknown";
}


// ============================================================
// SECTION 8 — THRESHOLD EVALUATION
// ============================================================

static void evaluateThreshold(DynamicSensor& s)
{
    // Relay: auto-off when linked ACS712 exceeds its HIGH threshold.
    // Compares the live value against thresholdHigh directly so the trip
    // works even if alertEnabled is false on the current sensor.
    if (s.type == "relay") {
        if (s.relayAutoOff && !s.relayLinkedId.isEmpty()) {
            for (auto& other : activeSensors) {
                if (other.id == s.relayLinkedId) {
                    bool overcurrent = !isnan(other.thresholdHigh) &&
                                       other.lastValue > other.thresholdHigh;
                    if (overcurrent && !s.relayOn) {
                        setRelayState(s, true);
                        Serial.printf("[RELAY] Auto-off: %s  %.2f A > %.2f A threshold\n",
                                      other.name.c_str(),
                                      other.lastValue, other.thresholdHigh);
                    }
                    break;
                }
            }
        }
        s.alertState = s.relayOn ? "active" : "ok";
        return;
    }

    // Vibration: binary
    if (s.type == "vib") {
        String prev = s.alertState;
        s.alertState = (s.lastValue >= 1.0f) ? "active" : "ok";
        if (s.alertState == "active" && prev != "active")
            Serial.printf("[ALERT] %s  vibration detected\n", s.name.c_str());
        return;
    }

    // MQ gas: air classifier is the primary alert
    if (s.type == "mq2" || s.type == "mq135") {
        String air = getAirStatus(s.pin1);
        bool gasAlarm = (air != "Clean Air" && air != "Calibrating" && air != "Unknown");
        if (gasAlarm) {
            if (s.alertState != "gas")
                Serial.printf("[ALERT] %s  air=%s\n", s.name.c_str(), air.c_str());
            s.alertState = "gas";
            return;
        }
    }

    // Numeric threshold
    if (!s.alertEnabled) { s.alertState = "ok"; return; }

    bool highSet = !isnan(s.thresholdHigh);
    bool lowSet  = !isnan(s.thresholdLow);

    if (highSet && s.lastValue > s.thresholdHigh) {
        if (s.alertState != "high")
            Serial.printf("[ALERT] %s  %.3f > HIGH %.3f\n",
                          s.name.c_str(), s.lastValue, s.thresholdHigh);
        s.alertState = "high";
    }
    else if (lowSet && s.lastValue < s.thresholdLow) {
        if (s.alertState != "low")
            Serial.printf("[ALERT] %s  %.3f < LOW %.3f\n",
                          s.name.c_str(), s.lastValue, s.thresholdLow);
        s.alertState = "low";
    }
    else {
        s.alertState = "ok";
    }
}


// ============================================================
// SECTION 9 — HARDWARE INITIALISATION
// ============================================================

void clearSensors()
{
    Serial.println("[HW] Clearing all sensor instances...");
    
    // Cleanup DallasTemperature instances
    for (std::map<int, DallasTemperature*>::iterator it = tempSensorInstances.begin(); it != tempSensorInstances.end(); ++it) {
        delete it->second;
    }
    // Cleanup OneWire instances
    for (std::map<int, OneWire*>::iterator it = oneWireInstances.begin(); it != oneWireInstances.end(); ++it) {
        delete it->second;
    }
    
    oneWireInstances.clear();
    tempSensorInstances.clear();
    tempRequestTime.clear();
    tempRequestPending.clear();
    
    gasBaselines.clear();
    gasAdaptiveMin.clear();
    airStatusCache.clear();
    
    vibRawState.clear();
    vibLastChange.clear();
    vibActiveUntil.clear();
    
    staleCount.clear();
    stalePrev.clear();
}

void initSensors()
{
    Serial.println("\n========================================");
    Serial.println("  Sensor Node — Hardware Init");
    Serial.println("========================================");

    // Ensure we start from a clean slate to prevent mixed-up readings
    clearSensors();

    // First pass: drive relay pins HIGH immediately before any blocking
    // calibration. Active-LOW hardware: HIGH = relay OFF (safe default).
    // This prevents the CW-020 internal pull-up from floating the IN pin
    // and energising the coil during the 150 ms MQ calibration window.
    for (const auto& s : activeSensors) {
        if (s.type == "relay") {
            digitalWrite(s.pin1, HIGH);
            pinMode(s.pin1, OUTPUT);
        }
    }

    initADCCal();

    for (auto& s : activeSensors) {

        // ── 801S Vibration Sensor ────────────────────────────────────────────
        // The 801S module's DO line is pulled HIGH internally.
        // Contact closure on vibration pulls DO LOW.  INPUT_PULLUP is still
        // used as a safety fallback for boards where the module is not present.
        if (s.type == "vib") {
            pinMode(s.pin1, INPUT_PULLDOWN);
            vibRawState[s.pin1]   = 0;
            vibLastChange[s.pin1] = 0;
            vibActiveUntil[s.pin1]= 0;
            Serial.printf("  [PIN] 801S Vibration  GPIO %d  (debounce %lu ms, hold %lu ms)\n",
                          s.pin1, VIB_DEBOUNCE_MS, VIB_HOLD_MS);
        }

        // ── Sharp GP2Y1010AU0F Particle Sensor ───────────────────────────────
        // pin1 = AOUT (data, e.g. GPIO 36 — input-only)
        // pin2 = LED  (e.g. GPIO 16)
        // Connect a 150 Ω resistor + 220 µF cap between Vcc and LED pin
        // on the sensor module as per the Sharp application note.
        else if (s.type == "particle") {
            pinMode(s.pin1, INPUT);
            pinMode(s.pin2, OUTPUT);
            digitalWrite(s.pin2, HIGH);   // LED off at rest (active LOW drive)
            Serial.printf("  [PIN] GP2Y1010  AOUT=GPIO%d  LED=GPIO%d\n",
                          s.pin1, s.pin2);
        }

        else if (s.type == "temp") {
            if (!oneWireInstances.count(s.pin1)) {
                oneWireInstances[s.pin1]    = new OneWire(s.pin1);
                tempSensorInstances[s.pin1] = new DallasTemperature(oneWireInstances[s.pin1]);
                tempSensorInstances[s.pin1]->begin();
                tempSensorInstances[s.pin1]->setResolution(12);
                tempSensorInstances[s.pin1]->setWaitForConversion(false);
                tempRequestPending[s.pin1]  = false;
                tempRequestTime[s.pin1]     = 0;
                Serial.printf("  [PIN] DS18B20  GPIO %d (12-bit async)\n", s.pin1);
            }
        }

        else if (s.type == "mq2") {
            pinMode(s.pin1, INPUT);
            gasBaselines[s.pin1]   = calibrateGasSensor(s.pin1, MQ2_LOAD_OHM,
                                                         MQ2_CLEAN_RATIO);
            gasAdaptiveMin[s.pin1] = 100.0f;
            airStatusCache[s.pin1] = "Calibrating";
        }

        else if (s.type == "mq135") {
            pinMode(s.pin1, INPUT);
            gasBaselines[s.pin1]   = calibrateGasSensor(s.pin1, MQ135_LOAD_OHM,
                                                         MQ135_CLEAN_RATIO);
            gasAdaptiveMin[s.pin1] = 100.0f;
            airStatusCache[s.pin1] = "Calibrating";
        }

        // Current sensor: no boot-time calibration needed.
        // readCurrent() self-zeros from the batch mean on every call,
        // eliminating temperature and supply-voltage drift.
        else if (s.type == "current") {
            pinMode(s.pin1, INPUT);
            Serial.printf("  [PIN] ACS712  GPIO %d  (%.0f mV/A, self-zeroing)\n",
                          s.pin1, ACS712_MV_PER_A);
        }

        // CW-020 relay module — active-LOW (LOW = relay ON, HIGH = relay OFF).
        // Pre-load HIGH before pinMode so relay stays de-energised at boot.
        else if (s.type == "relay") {
            s.relayOn = false;          // authoritative state = OFF at boot
            digitalWrite(s.pin1, HIGH);
            pinMode(s.pin1, OUTPUT);
            Serial.printf("  [PIN] Relay CW-020  GPIO %d  (boot=HIGH=OFF)\n", s.pin1);
        }
    }

    Serial.println("  Hardware init complete.");
    Serial.println("========================================\n");
}


// ============================================================
// SECTION 10 — INDIVIDUAL SENSOR READS
// ============================================================

/**
 * DS18B20 — two-phase non-blocking read.
 * Phase A: issues requestTemperatures(), returns NAN (conversion not ready).
 * Phase B: called after DS18B20_CONV_MS; reads and validates result.
 */
float readTemperature(int pin)
{
    if (!tempSensorInstances.count(pin)) return -127.0f;
    DallasTemperature* sensor = tempSensorInstances[pin];
    unsigned long now = millis();

    if (!tempRequestPending[pin]) {
        sensor->requestTemperatures();
        tempRequestTime[pin]    = now;
        tempRequestPending[pin] = true;
        return NAN;
    }
    if (now - tempRequestTime[pin] < DS18B20_CONV_MS) return NAN;

    tempRequestPending[pin] = false;
    float temp = sensor->getTempCByIndex(0);
    if (temp == DEVICE_DISCONNECTED_C) {
        Serial.printf("[WARN] DS18B20 GPIO %d disconnected\n", pin);
        return NAN;
    }
    return temp;
}

/**
 * MQ gas sensor — Rs/R0 ratio with adaptive drift correction.
 */
float readGas(int pin, float loadResistance)
{
    float rs       = measureResistance(pin, loadResistance);
    float baseline = gasBaselines.count(pin) ? gasBaselines[pin] : 10.0f;
    float ratio    = rs / baseline;

    if (loadResistance == MQ2_LOAD_OHM)
        applyAdaptiveDrift(pin, ratio, MQ2_ADAPT_CLEAN_MIN,   MQ2_CLEAN_RATIO);
    else
        applyAdaptiveDrift(pin, ratio, MQ135_ADAPT_CLEAN_MIN, MQ135_CLEAN_RATIO);

    return ratio;
}

/**
 * Sharp GP2Y1010AU0F dust / particle sensor.
 *
 * Hardware: AOUT -> pin1 (e.g. GPIO 36), LED -> pin2 (e.g. GPIO 16).
 * Requires a 150 Ω series resistor and 220 µF decoupling cap on the LED line.
 *
 * Timing is LED-pulse critical:
 *   1. Pull LED LOW (on) for 280 µs
 *   2. Read AOUT at the 280 µs peak
 *   3. Pull LED HIGH (off) immediately after
 *   4. Wait the remainder of the 10 ms cycle
 *
 * Conversion from datasheet:
 *   Clean air baseline ≈ 0.9 V
 *   Sensitivity         ≈ 0.5 V per 100 µg/m³  (= 200 µg/m³ per volt)
 *   density (µg/m³)     = max(0, (Vout - 0.9) * 200)
 *
 * Five samples are taken and the median is returned to reject
 * single-cycle noise and LED flicker artefacts.
 */
float readParticle(int analogPin, int ledPin)
{
    float samples[PARTICLE_SAMPLES];
    float rawSum = 0;

    for (int i = 0; i < PARTICLE_SAMPLES; i++) {
        // LED on — active-LOW drive
        digitalWrite(ledPin, LOW);
        delayMicroseconds(PARTICLE_LED_ON_US);   // 280 µs — read window

        int raw = analogRead(analogPin);
        rawSum += raw;

        delayMicroseconds(PARTICLE_LED_OFF_US);  // 40 µs
        digitalWrite(ledPin, HIGH);               // LED off
        delayMicroseconds(PARTICLE_CYCLE_US);     // rest of 10 ms cycle

        float v       = adcToVolts(raw);
        float density = (v - PARTICLE_BASELINE_V) * PARTICLE_SENS;
        samples[i]    = (density < 0.0f) ? 0.0f : density;
    }
    
    // DEBUG: Show raw average
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 2500) {
        Serial.printf("[DEBUG] Dust Sensor Pin %d: AvgRaw=%.1f, LEDPin=%d\n", 
                      analogPin, (rawSum/PARTICLE_SAMPLES), ledPin);
        lastPrint = millis();
    }

    std::sort(samples, samples + PARTICLE_SAMPLES);
    return samples[PARTICLE_SAMPLES / 2];   // median
}

/**
 * ACS712 current sensor — self-zeroing true RMS.
 *
 * Root cause of phantom 0.2–0.4 A readings with the old approach:
 *   The boot-time zero calibration drifted with temperature and supply
 *   voltage, causing a persistent DC offset in the RMS calculation.
 *
 * Fix — self-zeroing batch method:
 *   1. Collect CURRENT_SAMPLES ADC readings.
 *   2. Compute the batch mean as the live DC zero reference.
 *      This automatically compensates for temperature drift without
 *      requiring a separate calibration step or load-off guarantee.
 *   3. Compute the RMS of the mean-centred samples (AC component only).
 *   4. Floor values below ACS712_DEADBAND_A to 0.
 *
 * Sampling: 200 samples × 500 µs = 100 ms total.
 * 100 ms covers 5 complete cycles of 50 Hz and 6 cycles of 60 Hz,
 * giving a stable RMS estimate regardless of grid frequency.
 *
 * Note: this method assumes the load current is predominantly AC
 * (motors, heaters, lighting).  For pure DC loads the self-zeroing
 * mean will subtract the DC component from the measurement.  If DC
 * current measurement is required, re-enable the stored zero from
 * boot-time calibration instead.
 */
float readCurrent(int pin)
{
    // ── Phase 1: collect raw voltage samples ────────────────────────────────
    float samples[CURRENT_SAMPLES];
    float sum = 0.0f;

    // Fast sampling — read raw ADC first
    for (int i = 0; i < CURRENT_SAMPLES; i++) {
        int raw = analogRead(pin);
        float volts = adcToVolts(raw);
        samples[i]  = volts;
        sum        += volts;
        delayMicroseconds(200);   // Faster sampling for AC peak capture
    }

    // ── Phase 2: compute batch mean (live DC zero) ───────────────────────────
    float zero = sum / CURRENT_SAMPLES;
    
    // DEBUG: Show raw center point
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 2000) {
        Serial.printf("[DEBUG] Current Sensor Pin %d: ZeroV=%.3fV, Deadband=%.2fA\n", 
                      pin, zero, ACS712_DEADBAND_A);
        lastPrint = millis();
    }

    // ── Phase 3: RMS of AC (mean-centred) component ──────────────────────────
    float sumSq = 0.0f;
    for (int i = 0; i < CURRENT_SAMPLES; i++) {
        float cur  = (samples[i] - zero) / (ACS712_MV_PER_A * 0.001f);
        sumSq     += cur * cur;
    }
    float rms = sqrtf(sumSq / CURRENT_SAMPLES);

    // ── Phase 4: dead-band floor ─────────────────────────────────────────────
    return (rms < ACS712_DEADBAND_A) ? 0.0f : rms;
}

/**
 * 801S Vibration Sensor Switch — debounce + hold-latch.
 *
 * The 801S emits momentary HIGH pulses (as short as 1–5 ms per event).
 * A simple debounce with VIB_DEBOUNCE_MS = 1 ms rejects contact bounce
 * while still accepting real events.
 *
 * The hold-latch keeps the "active" return value high for VIB_HOLD_MS
 * (500 ms) after the last validated event.  Without the hold, the
 * dashboard 2-second poll interval would almost never capture the pulse.
 *
 * Logic:
 *   1. Debounce the raw digital input.
 *   2. On each debounced HIGH (active) sample, extend vibActiveUntil.
 *   3. Return 1 if current time is within the hold window, else 0.
 */
int readVibration(int pin)
{
    int  raw = (digitalRead(pin) == HIGH) ? 1 : 0;
    unsigned long now = millis();

    // Debounce: only accept a state change after VIB_DEBOUNCE_MS of stability
    if (raw != vibRawState[pin]) {
        if (now - vibLastChange[pin] >= VIB_DEBOUNCE_MS) {
            vibRawState[pin]  = raw;
            vibLastChange[pin] = now;
        }
    }

    // Extend hold window every time the sensor is actively triggered
    if (vibRawState[pin] == 1) {
        vibActiveUntil[pin] = now + VIB_HOLD_MS;
    }

    return (now < vibActiveUntil[pin]) ? 1 : 0;
}


// ============================================================
// SECTION 10b — RELAY CONTROL (CW-020)
// ============================================================

/**
 * setRelayState — energise or de-energise the CW-020 relay.
 *
 * The CW-020 IN pin is active-LOW:
 *   LOW  → relay coil energised → COM→NO closed, green LED ON  (on=true)
 *   HIGH → relay de-energised  → COM→NC closed, fail-safe       (on=false)
 *
 * s.relayOn is the single source of truth for relay state.
 * The GPIO and s.lastValue are both derived from it — nothing else
 * writes these for relay sensors, so the sensor loop cannot override
 * a manual toggle.
 */
void setRelayState(DynamicSensor& s, bool on)
{
    s.relayOn   = on;                          // authoritative state
    digitalWrite(s.pin1, on ? LOW : HIGH);     // active-LOW hardware
    s.lastValue = on ? 1.0f : 0.0f;
    Serial.printf("[RELAY] %s → %s (GPIO%d=%s)\n",
                  s.name.c_str(), on ? "ON" : "OFF",
                  s.pin1,         on ? "LOW" : "HIGH");
}

bool getRelayState(const DynamicSensor& s)
{
    return s.relayOn;   // read software state, never poll GPIO
}


// ============================================================
// SECTION 11 — STUCK SENSOR DETECTION
// ============================================================

/**
 * Called from dispatchSensorRead() after every successful read.
 *
 * Compares the new value against the last reported value.  If the
 * difference is below STALE_EPSILON for STALE_LIMIT consecutive reads,
 * a warning is printed and a soft re-init is attempted:
 *   DS18B20  — reset pending flag so a new conversion cycle starts.
 *   MQ       — re-run calibration (re-establishes R0 baseline).
 *   Current  — nothing extra needed; self-zeroing already handles drift.
 *   Particle — nothing extra; LED timing is hardware-driven.
 *   Vibration— nothing extra; binary output cannot "stick" meaningfully.
 *
 * The stale counter resets to 0 after logging so re-alerts are spaced
 * at least STALE_LIMIT reads apart.
 */
static void checkStale(DynamicSensor& s, float newValue)
{
    // Relay output is intentionally stable (0 or 1) — skip stuck-sensor logic
    if (s.type == "relay") return;

    float prev = stalePrev[s.id];

    if (fabsf(newValue - prev) < STALE_EPSILON) {
        staleCount[s.id]++;
        if (staleCount[s.id] >= STALE_LIMIT) {
            Serial.printf("[STALE] %s (%s)  value=%.4f unchanged for %d reads — attempting recovery\n",
                          s.name.c_str(), s.id.c_str(), newValue, STALE_LIMIT);

            // Soft recovery per sensor type
            if (s.type == "temp") {
                // Force a new conversion cycle
                tempRequestPending[s.pin1] = false;
                Serial.printf("[STALE] DS18B20 GPIO %d  reset pending flag\n", s.pin1);
            }
            else if (s.type == "mq2" || s.type == "mq135") {
                // Re-establish baseline
                float load = (s.type == "mq2") ? MQ2_LOAD_OHM : MQ135_LOAD_OHM;
                float cf   = (s.type == "mq2") ? MQ2_CLEAN_RATIO : MQ135_CLEAN_RATIO;
                gasBaselines[s.pin1]   = calibrateGasSensor(s.pin1, load, cf);
                gasAdaptiveMin[s.pin1] = 100.0f;
                Serial.printf("[STALE] MQ GPIO %d  R0 recalibrated\n", s.pin1);
            }

            staleCount[s.id] = 0;   // reset counter after recovery attempt
        }
    } else {
        staleCount[s.id] = 0;
        stalePrev[s.id]  = newValue;
    }
}


// ============================================================
// SECTION 12 — MAIN LOOP DISPATCHER
// ============================================================

static bool dispatchSensorRead(DynamicSensor& s)
{
    float value = NAN;

    if (s.type == "temp") {
        value = readTemperature(s.pin1);
        if (isnan(value)) return false;   // conversion not yet complete
    }
    else if (s.type == "mq2") {
        value = readGas(s.pin1, MQ2_LOAD_OHM);

        float mq135 = 99.0f;
        for (const auto& o : activeSensors)
            if (o.type == "mq135") { mq135 = o.lastValue; break; }

        airStatusCache[s.pin1] = classifyAir(value, mq135);
        Serial.printf("[MQ2]    GPIO%d  ratio=%.3f  -> %s\n",
                      s.pin1, value, airStatusCache[s.pin1].c_str());
    }
    else if (s.type == "mq135") {
        value = readGas(s.pin1, MQ135_LOAD_OHM);

        float mq2 = 99.0f;
        for (const auto& o : activeSensors)
            if (o.type == "mq2") { mq2 = o.lastValue; break; }

        airStatusCache[s.pin1] = classifyAir(mq2, value);
        Serial.printf("[MQ135]  GPIO%d  ratio=%.3f  -> %s\n",
                      s.pin1, value, airStatusCache[s.pin1].c_str());
    }
    else if (s.type == "particle") {
        value = readParticle(s.pin1, s.pin2);
        Serial.printf("[DUST]   GPIO%d  %.1f ug/m3\n", s.pin1, value);
    }
    else if (s.type == "vib") {
        value = (float)readVibration(s.pin1);
    }
    else if (s.type == "current") {
        value = readCurrent(s.pin1);
        Serial.printf("[ACS712] GPIO%d  %.3f A RMS\n", s.pin1, value);
    }
    else if (s.type == "relay") {
        // Read from software state only — never poll GPIO.
        // GPIO is an OUTPUT; reading it back can return stale values if
        // the CW-020 pull-up fights the line briefly after a state change,
        // which was silently overwriting the actuator state every read cycle.
        value = s.relayOn ? 1.0f : 0.0f;
    }

    // Update value and run stuck-sensor check
    s.lastValue = value;
    checkStale(s, value);

    return true;
}

/**
 * Non-blocking main-loop dispatcher.
 *
 * Each sensor is checked independently against its own readInterval.
 * DS18B20 conversion (750 ms) is handled asynchronously via two-phase
 * logic in readTemperature() — loop() is never blocked.
 * ACS712 sampling takes ~100 ms per call; move to a FreeRTOS task if
 * even that latency is unacceptable.
 */
#include <WiFiUdp.h>
static WiFiUDP s_discoveryUDP;
static const int DISCOVERY_PORT = 4210;
static unsigned long s_lastHeartbeat = 0;

void updateDiscovery()
{
    static bool udpStarted = false;
    if (!udpStarted) {
        s_discoveryUDP.begin(DISCOVERY_PORT);
        udpStarted = true;
    }

    // 1. Respond to direct discovery requests
    int packetSize = s_discoveryUDP.parsePacket();
    if (packetSize) {
        char buf[64];
        int len = s_discoveryUDP.read(buf, 63);
        if (len > 0) buf[len] = 0;

        if (String(buf) == "ISMS_DISCOVERY_REQUEST") {
            JsonDocument doc;
            doc["id"] = chipDerivedSecret("n");
            doc["hostname"] = systemConfig.hostname;
            doc["type"] = "sensor_node";
            
            s_discoveryUDP.beginPacket(s_discoveryUDP.remoteIP(), DISCOVERY_PORT);
            serializeJson(doc, s_discoveryUDP);
            s_discoveryUDP.endPacket();
        }
    }

    // 2. Periodic heartbeat broadcast
    if (millis() - s_lastHeartbeat > 30000) {
        IPAddress broadcastIP = WiFi.localIP();
        broadcastIP[3] = 255;
        s_discoveryUDP.beginPacket(broadcastIP, DISCOVERY_PORT);
        
        JsonDocument doc;
        doc["id"] = chipDerivedSecret("n");
        doc["hostname"] = systemConfig.hostname;
        doc["type"] = "sensor_node";
        serializeJson(doc, s_discoveryUDP);
        s_discoveryUDP.endPacket();
        
        s_lastHeartbeat = millis();
    }
}

void readDynamicSensors()
{
    unsigned long now = millis();

    for (auto& s : activeSensors) {
        if (now - s.lastReadTime < s.readInterval) continue;

        bool updated = dispatchSensorRead(s);
        if (!updated) continue;

        s.lastReadTime = now;
        evaluateThreshold(s);

        // Update the physical display's menu snapshot cache
        String unit = "";
        if      (s.type == "temp")     unit = "C";
        else if (s.type == "current")  unit = "A";
        else if (s.type == "particle") unit = "ug/m3";
        else if (s.type == "vib")      unit = "";
        else if (s.type == "relay")    unit = "";
        else                           unit = "ratio";
        displaySetSensorSnapshot(s.id, s.name, s.lastValue, unit, s.readInterval);
        }
        }