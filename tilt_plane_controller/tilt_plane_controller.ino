/*
  Tilt Flight controller: Arduino Uno + MPU6050 + pushbutton.
  MPU6050: VCC according to the breakout's specification, GND->GND,
  SDA->A4, SCL->A5, AD0->GND (address 0x68).
  Button: D2 to one side, GND to the other (INPUT_PULLUP).

  Serial at 115200: pitch_deg,roll_deg,yaw_rate_dps,button (CSV)
  Pitch/roll are complementary-filtered accel+gyro angles; yaw is gyro Z rate.
  The MPU6050 has no compass, so it cannot provide an absolute heading.
*/
#include <Wire.h>

const uint8_t MPU_ADDR = 0x68;
const uint8_t BUTTON_PIN = 2;
// LEDs: connect each pin to an LED anode through a resistor; cathodes go to GND.
const uint8_t GREEN_LED_PIN = 4;
const uint8_t RED_LED_PIN = 5;
const float ACCEL_LSB_PER_G = 16384.0f;     // +/-2 g after reset
const float GYRO_LSB_PER_DPS = 131.0f;      // +/-250 deg/s after reset
const float COMPLEMENTARY_ALPHA = 0.96f;
const uint16_t CAL_SAMPLES = 180;

float gyroBiasX = 0, gyroBiasY = 0, gyroBiasZ = 0;
float pitchDeg = 0, rollDeg = 0, yawRateDps = 0;
bool anglesReady = false;
unsigned long previousMicros = 0;
int buttonStable = HIGH, buttonCandidate = HIGH;
unsigned long buttonChangedAt = 0;
bool flightActive = true;
bool crashLedOn = false;
unsigned long lastCrashLedToggle = 0;

void readFlightStateCommand() {
  while (Serial.available() > 0) {
    char command = Serial.read();
    if (command == 'F') {
      flightActive = true;
      crashLedOn = false;
      digitalWrite(GREEN_LED_PIN, HIGH);
      digitalWrite(RED_LED_PIN, LOW);
    } else if (command == 'C') {
      flightActive = false;
      crashLedOn = true;
      lastCrashLedToggle = millis();
      digitalWrite(GREEN_LED_PIN, LOW);
      digitalWrite(RED_LED_PIN, HIGH);
    }
  }
}

void updateLeds() {
  if (flightActive) return;
  unsigned long now = millis();
  if (now - lastCrashLedToggle >= 500) {
    lastCrashLedToggle = now;
    crashLedOn = !crashLedOn;
    digitalWrite(RED_LED_PIN, crashLedOn ? HIGH : LOW);
  }
}

bool readFrame(int16_t &ax, int16_t &ay, int16_t &az,
               int16_t &gx, int16_t &gy, int16_t &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // accel X high byte; accel, temperature, then gyro
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14, (uint8_t)true) != 14) {
    while (Wire.available()) Wire.read();
    return false;
  }
  int16_t values[7];
  for (uint8_t i = 0; i < 7; ++i) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    values[i] = (int16_t)(((uint16_t)hi << 8) | lo);
  }
  ax = values[0]; ay = values[1]; az = values[2];
  gx = values[4]; gy = values[5]; gz = values[6];
  return true;
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(400000);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // PWR_MGMT_1: wake the MPU6050
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);

  // Keep the board still and level for this brief gyro-bias calibration.
  int32_t sx = 0, sy = 0, sz = 0;
  uint16_t good = 0;
  for (uint16_t i = 0; i < CAL_SAMPLES; ++i) {
    int16_t ax, ay, az, gx, gy, gz;
    if (readFrame(ax, ay, az, gx, gy, gz)) {
      sx += gx; sy += gy; sz += gz; ++good;
    }
    delay(4);
  }
  if (good) {
    gyroBiasX = (float)sx / good;
    gyroBiasY = (float)sy / good;
    gyroBiasZ = (float)sz / good;
  }
  previousMicros = micros();
}

void loop() {
  readFlightStateCommand();
  updateLeds();
  int16_t ax, ay, az, rawGx, rawGy, rawGz;
  if (!readFrame(ax, ay, az, rawGx, rawGy, rawGz)) {
    delay(20);
    return;
  }
  unsigned long now = micros();
  float dt = (now - previousMicros) * 0.000001f;
  previousMicros = now;
  if (dt <= 0 || dt > 0.1f) dt = 0.02f;

  float accX = ax / ACCEL_LSB_PER_G;
  float accY = ay / ACCEL_LSB_PER_G;
  float accZ = az / ACCEL_LSB_PER_G;
  float accelPitch = atan2(-accX, sqrt(accY * accY + accZ * accZ)) * 57.29578f;
  float accelRoll = atan2(accY, accZ) * 57.29578f;
  float gx = (rawGx - gyroBiasX) / GYRO_LSB_PER_DPS;
  float gy = (rawGy - gyroBiasY) / GYRO_LSB_PER_DPS;
  float gz = (rawGz - gyroBiasZ) / GYRO_LSB_PER_DPS;

  if (!anglesReady) {
    pitchDeg = accelPitch;
    rollDeg = accelRoll;
    anglesReady = true;
  } else {
    // Board X rotation is roll; board Y rotation is pitch.
    pitchDeg = COMPLEMENTARY_ALPHA * (pitchDeg + gy * dt)
             + (1.0f - COMPLEMENTARY_ALPHA) * accelPitch;
    rollDeg = COMPLEMENTARY_ALPHA * (rollDeg + gx * dt)
            + (1.0f - COMPLEMENTARY_ALPHA) * accelRoll;
  }
  yawRateDps = gz;

  int reading = digitalRead(BUTTON_PIN);
  if (reading != buttonCandidate) {
    buttonCandidate = reading;
    buttonChangedAt = millis();
  }
  if (millis() - buttonChangedAt >= 25) buttonStable = buttonCandidate;

  Serial.print(pitchDeg, 2); Serial.print(',');
  Serial.print(rollDeg, 2); Serial.print(',');
  Serial.print(yawRateDps, 2); Serial.print(',');
  Serial.println(buttonStable == LOW ? 1 : 0);
  delay(15);
}
