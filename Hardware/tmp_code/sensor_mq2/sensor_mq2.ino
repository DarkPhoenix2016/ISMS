/******** ESP32 MQ2 + MQ135 GAS MONITOR WITH ABC ********/

#define MQ2_ANALOG_PIN 34
#define MQ135_ANALOG_PIN 35

#define SENSOR_SUPPLY_VOLTAGE 5.0
#define ADC_MAX_VALUE 4095.0

#define CALIBRATION_SAMPLE_COUNT 50
#define SENSOR_READ_SAMPLE_COUNT 10

// Adjustable delay between readings
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
  // MQ2 adaptive correction
  if(mq2Ratio > 6 && mq2Ratio < mq2MinimumObservedRatio)
  {
    mq2MinimumObservedRatio = mq2Ratio;
    mq2BaselineResistance = mq2BaselineResistance * 0.9 +
                            (mq2BaselineResistance * mq2Ratio / 9.83) * 0.1;
  }

  // MQ135 adaptive correction
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



/******** SETUP ********/

void setup()
{
  Serial.begin(115200);

  Serial.println();
  Serial.println("Gas Monitoring System Starting...");
  Serial.println("Place sensors in CLEAN AIR for calibration");

  delay(8000);


  mq2BaselineResistance =
    calibrateSensor(MQ2_ANALOG_PIN, mq2LoadResistance, 9.83);

  mq135BaselineResistance =
    calibrateSensor(MQ135_ANALOG_PIN, mq135LoadResistance, 3.6);


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


  // Apply adaptive baseline correction
  updateAdaptiveBaseline(mq2Ratio, mq135Ratio);


  String alertStatus =
    classifyAirCondition(mq2Ratio, mq135Ratio);



  Serial.println("========================================");
  Serial.println("Gas Monitoring System Report");

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