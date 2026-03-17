/******** ESP32 MQ2 + MQ135 GAS MONITOR WITH ABC + TEMPERATURE + CURRENT ********/

#include <OneWire.h>
#include <DallasTemperature.h>

#define MQ2_ANALOG_PIN 34
#define MQ135_ANALOG_PIN 35
#define ACS712_ANALOG_PIN 32

#define DS18B20_PIN 4

#define SENSOR_SUPPLY_VOLTAGE 5.0
#define ADC_MAX_VALUE 4095.0

#define CALIBRATION_SAMPLE_COUNT 50
#define SENSOR_READ_SAMPLE_COUNT 10

unsigned long readingDelay = 500;


// Load resistance values (kΩ)
float mq2LoadResistance = 5.0;
float mq135LoadResistance = 10.0;


// Clean air baseline resistances
float mq2BaselineResistance = 10.0;
float mq135BaselineResistance = 10.0;


// Adaptive baseline limits
float mq2MinimumObservedRatio = 100;
float mq135MinimumObservedRatio = 100;


/******** ACS712 PARAMETERS ********/

float currentSensorSensitivity = 0.066; // 66mV per Amp (30A module)
float currentSensorZeroVoltage = 0;


/******** DS18B20 SETUP ********/

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);



/******** SENSOR RESISTANCE CALCULATION ********/

float calculateSensorResistance(int analogPin, float loadResistance)
{
  float resistanceSum = 0;

  for(int i=0;i<SENSOR_READ_SAMPLE_COUNT;i++)
  {
    int analogValue = analogRead(analogPin);

    float sensorVoltage =
      analogValue * (SENSOR_SUPPLY_VOLTAGE / ADC_MAX_VALUE);

    if(sensorVoltage <= 0) sensorVoltage = 0.0001;

    float sensorResistance =
      ((SENSOR_SUPPLY_VOLTAGE - sensorVoltage) / sensorVoltage) * loadResistance;

    resistanceSum += sensorResistance;

    delay(10);
  }

  return resistanceSum / SENSOR_READ_SAMPLE_COUNT;
}



/******** INITIAL CLEAN AIR CALIBRATION ********/

float calibrateSensor(int analogPin, float loadResistance, float cleanAirFactor)
{
  float resistanceSum = 0;

  for(int i=0;i<CALIBRATION_SAMPLE_COUNT;i++)
  {
    resistanceSum += calculateSensorResistance(analogPin, loadResistance);
    delay(100);
  }

  float averageResistance = resistanceSum / CALIBRATION_SAMPLE_COUNT;

  return averageResistance / cleanAirFactor;
}



/******** ADAPTIVE BASELINE CORRECTION ********/

void updateAdaptiveBaseline(float mq2Ratio, float mq135Ratio)
{
  if(mq2Ratio > 6 && mq2Ratio < mq2MinimumObservedRatio)
  {
    mq2MinimumObservedRatio = mq2Ratio;
    mq2BaselineResistance = mq2BaselineResistance * 0.9 +
                            (mq2BaselineResistance * mq2Ratio / 9.83) * 0.1;
  }

  if(mq135Ratio > 3 && mq135Ratio < mq135MinimumObservedRatio)
  {
    mq135MinimumObservedRatio = mq135Ratio;
    mq135BaselineResistance = mq135BaselineResistance * 0.9 +
                              (mq135BaselineResistance * mq135Ratio / 3.6) * 0.1;
  }
}



/******** ALERT CLASSIFICATION ********/

String classifyAirCondition(float mq2Ratio, float mq135Ratio)
{
  if(mq2Ratio < 2.5 && mq135Ratio > 2.5)
    return "Combustible Gas Detected";

  if(mq2Ratio < 2.5 && mq135Ratio < 2.5)
    return "Smoke or VOC Presence";

  if(mq2Ratio > 2.5 && mq135Ratio < 2.5)
    return "Air Pollution Detected";

  return "Clean Air";
}



/******** CURRENT SENSOR ZERO CALIBRATION ********/

void calibrateCurrentSensor()
{
  Serial.println("Calibrating current sensor... Keep load OFF");

  float voltageSum = 0;

  for(int i=0;i<200;i++)
  {
    int analogValue = analogRead(ACS712_ANALOG_PIN);

    float voltage = analogValue * (3.3 / ADC_MAX_VALUE);

    voltageSum += voltage;

    delay(5);
  }

  currentSensorZeroVoltage = voltageSum / 200;

  Serial.print("Current sensor zero voltage: ");
  Serial.println(currentSensorZeroVoltage);
}



/******** CURRENT SENSOR READING ********/

float readCurrent()
{
  float voltageSum = 0;

  for(int i=0;i<50;i++)
  {
    int analogValue = analogRead(ACS712_ANALOG_PIN);

    float voltage = analogValue * (3.3 / ADC_MAX_VALUE);

    voltageSum += voltage;

    delay(2);
  }

  float averageVoltage = voltageSum / 50;

  float current =
    (averageVoltage - currentSensorZeroVoltage) / currentSensorSensitivity;

  // Noise dead-zone filter
  if(abs(current) < 0.1)
    current = 0;

  return current;
}



/******** SETUP ********/

void setup()
{
  Serial.begin(115200);

  temperatureSensor.begin();

  Serial.println();
  Serial.println("Gas Monitoring System Starting...");
  Serial.println("Place sensors in CLEAN AIR for calibration");

  delay(8000);


  mq2BaselineResistance =
    calibrateSensor(MQ2_ANALOG_PIN, mq2LoadResistance, 9.83);

  mq135BaselineResistance =
    calibrateSensor(MQ135_ANALOG_PIN, mq135LoadResistance, 3.6);


  calibrateCurrentSensor();


  Serial.println();
  Serial.println("Calibration Completed");

  Serial.print("MQ2 Baseline Resistance: ");
  Serial.println(mq2BaselineResistance);

  Serial.print("MQ135 Baseline Resistance: ");
  Serial.println(mq135BaselineResistance);

  Serial.println();
}



/******** MAIN LOOP ********/

void loop()
{
  float mq2Resistance =
    calculateSensorResistance(MQ2_ANALOG_PIN, mq2LoadResistance);

  float mq135Resistance =
    calculateSensorResistance(MQ135_ANALOG_PIN, mq135LoadResistance);


  float mq2Ratio = mq2Resistance / mq2BaselineResistance;
  float mq135Ratio = mq135Resistance / mq135BaselineResistance;


  updateAdaptiveBaseline(mq2Ratio, mq135Ratio);


  String alertStatus =
    classifyAirCondition(mq2Ratio, mq135Ratio);


  /******** TEMPERATURE READING ********/

  temperatureSensor.requestTemperatures();
  float temperatureC = temperatureSensor.getTempCByIndex(0);


  /******** CURRENT READING ********/

  float currentAmp = readCurrent();



  Serial.println("========================================");
  Serial.println("Gas Monitoring System Report");

  Serial.println();

  Serial.print("Temperature: ");
  Serial.print(temperatureC);
  Serial.println(" C");

  Serial.print("Electrical Current: ");
  Serial.print(currentAmp);
  Serial.println(" A");

  Serial.println();

  Serial.print("MQ2 Combustible Gas Activity Ratio: ");
  Serial.println(mq2Ratio);

  Serial.print("MQ135 Air Quality Ratio: ");
  Serial.println(mq135Ratio);

  Serial.println();

  Serial.print("Air Condition Classification: ");
  Serial.println(alertStatus);

  Serial.println("========================================");

  delay(readingDelay);
}