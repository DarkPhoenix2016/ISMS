#include <math.h>

// ===== PIN DEFINITIONS =====
#define MQ135_AO 33
#define MQ135_DO 19

#define MQ2_AO   25
#define MQ2_DO   21

// ===== THRESHOLDS (ADC 0–4095) =====
#define MQ2_THRESHOLD   160
#define MQ135_THRESHOLD 300

// ===== SETTINGS =====
unsigned long readingDelay = 500;  // 0.5 seconds

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(MQ135_DO, INPUT);
  pinMode(MQ2_DO, INPUT);

  Serial.println("===== Gas Monitoring System =====");
  Serial.println("Warming up sensors (60 sec)...");
  delay(60000);
  Serial.println("===== System Ready =====");
}

void loop() {

  // ===== ANALOG READ =====
  int adc2   = analogRead(MQ2_AO);
  int adc135 = analogRead(MQ135_AO);

  // ===== THRESHOLD CHECK =====
  bool mq2_alert   = adc2   > MQ2_THRESHOLD;
  bool mq135_alert = adc135 > MQ135_THRESHOLD;

  // ===== DIGITAL READ =====
  int mq2_alarm   = digitalRead(MQ2_DO);
  int mq135_alarm = digitalRead(MQ135_DO);

  // ===== PRINT OUTPUT =====
  Serial.println("=====================================");

  Serial.print("MQ-2  | ADC: ");
  Serial.print(adc2);
  Serial.print(" | Analog Alert: ");
  Serial.print(mq2_alert ? "TRIGGERED" : "SAFE");
  // Serial.print(" | Digital Alarm: ");
  // Serial.println(mq2_alarm ? "TRIGGERED" : "SAFE");

  Serial.print("MQ-135| ADC: ");
  Serial.print(adc135);
  Serial.print(" | Analog Alert: ");
  Serial.print(mq135_alert ? "TRIGGERED" : "SAFE");
  // Serial.print(" | Digital Alarm: ");
  // Serial.println(mq135_alarm ? "TRIGGERED" : "SAFE");

  delay(readingDelay);
}