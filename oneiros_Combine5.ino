/*
  ONEIROS - AD8232 ECG + MAX30105 (BPM + SpO2) + MPU6050 head droop
  on Arduino Nano ESP32

  Wiring:
    AD8232:   OUTPUT -> A0, LO+ -> D2, LO- -> D3, 3.3V, GND
    MAX30105: SDA -> A4, SCL -> A5, VIN -> 3.3V, GND
    MPU6050:  SDA -> A4, SCL -> A5, VCC -> 3.3V, GND, AD0 -> GND (address 0x68)
              (shares the same two I2C wires as the MAX30105)

  Output (one line every SAMPLE_INTERVAL):
    Time:<ms>,ADC:<0-4095>,Volts:<v>,LeadsOff:<0/1>,BPM:<n>,SpO2:<n>,TempF:<f>,Pitch:<deg>,Roll:<deg>,Droop:<0/1>
  Works in Serial Plotter and Serial Monitor.
  BPM, SpO2, Pitch, Roll and Droop update slowly; their last value is repeated on each line.

  Head droop:
    At startup, look straight ahead and hold still for the 10 s calibration.
    Pitch/Roll are relative to that "forward" pose. Droop = 1 when the head is nodded
    down (or tilted sideways) past the threshold for DROOP_HOLD_MS.
    Type 'c' in the Serial Monitor to recalibrate (this pauses ECG output for ~13 s).
*/

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// ---------- AD8232 ----------
const int ECG_PIN = A0;
const int LO_PLUS_PIN = D2;
const int LO_MINUS_PIN = D3;
const float ADC_REF_VOLTS = 3.3;
const int ADC_MAX = 4095;                    // 12-bit
const unsigned long SAMPLE_INTERVAL = 6000;  // us
unsigned long nextSampleMicros = 0;

// ---------- MAX30105 ----------
MAX30105 particleSensor;

const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg = 0;

#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
byte bufferIndex = 0;
int32_t spo2 = 0;
int8_t validSPO2 = 0;
int32_t heartRateAlgo;
int8_t validHeartRate;

// ---------- Temperature (non-blocking) ----------
// Calibration offset in degrees F, ADDED to the sensor reading. Set to match your old sketch.
const float TEMP_OFFSET_F = 0.0;
const unsigned long TEMP_INTERVAL_MS = 10000;  // take a temperature reading every 10 s
const uint8_t MAX_ADDR = 0x57;
float temperatureF = 0;
unsigned long tempStartedAt = 0;
unsigned long lastTempStart = 0;
bool tempConverting = false;

// ---------- MPU6050 head droop ----------
const uint8_t  MPU_ADDRESS     = 0x68;    // 0x69 if AD0 is pulled high
const uint32_t CALIBRATION_MS  = 10000;   // 10 s calibration
const uint16_t MPU_HZ          = 100;     // MPU sample rate

const float ALPHA              = 0.98;    // complementary filter: gyro vs accel trust

// Flip a sign if your mounting makes "nodding down" come out negative.
const float PITCH_SIGN         = 1.0;     // set to -1.0 to flip
const float ROLL_SIGN          = 1.0;

const float    DROOP_PITCH_DEG = 20.0;    // nodding down beyond this = droop
const float    DROOP_ROLL_DEG  = 20.0;    // tilting sideways (either way) beyond this = droop
const uint32_t DROOP_HOLD_MS   = 2500;    // must stay past the threshold this long

const float MAX_ANGLE_RANGE    = 5.0;     // deg of spread allowed during calibration
const float MAX_GYRO_STD       = 2.0;     // deg/s allowed during calibration

const uint8_t REG_PWR_MGMT_1   = 0x6B;
const uint8_t REG_CONFIG       = 0x1A;
const uint8_t REG_GYRO_CONFIG  = 0x1B;
const uint8_t REG_ACCEL_CONFIG = 0x1C;
const uint8_t REG_ACCEL_XOUT_H = 0x3B;

const float ACCEL_LSB_PER_G    = 16384.0; // +/-2 g
const float GYRO_LSB_PER_DPS   = 131.0;   // +/-250 dps

float gyroBiasX = 0, gyroBiasY = 0, gyroBiasZ = 0;
float pitchZero = 0, rollZero = 0;        // calibrated "forward" pose
float pitch = 0, roll = 0;                // filtered absolute angles
float pitchRel = 0, rollRel = 0;          // angles relative to forward pose (these are printed)

unsigned long lastMpuMicros = 0;
unsigned long nextMpuUs = 0;
unsigned long droopStartMs = 0;
bool droopTiming = false;
bool droopActive = false;

// ================= MPU6050 helpers =================
void mpuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool mpuRead(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDRESS, 14) != 14) return false;

  int16_t rax = (Wire.read() << 8) | Wire.read();
  int16_t ray = (Wire.read() << 8) | Wire.read();
  int16_t raz = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();  // temperature, unused
  int16_t rgx = (Wire.read() << 8) | Wire.read();
  int16_t rgy = (Wire.read() << 8) | Wire.read();
  int16_t rgz = (Wire.read() << 8) | Wire.read();

  ax = rax / ACCEL_LSB_PER_G;
  ay = ray / ACCEL_LSB_PER_G;
  az = raz / ACCEL_LSB_PER_G;
  gx = rgx / GYRO_LSB_PER_DPS;
  gy = rgy / GYRO_LSB_PER_DPS;
  gz = rgz / GYRO_LSB_PER_DPS;
  return true;
}

float accelPitch(float ax, float ay, float az) {
  return PITCH_SIGN * atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
}

float accelRoll(float ay, float az) {
  return ROLL_SIGN * atan2(ay, az) * 180.0 / PI;
}

// Gyro bias + forward pose averaged over CALIBRATION_MS. Returns true if the head stayed still.
bool mpuCalibrate() {
  Serial.println();
  Serial.println(F("=== HEAD CALIBRATION ==="));
  Serial.println(F("Look straight ahead and hold still for 10 seconds."));
  for (int i = 3; i > 0; i--) {
    Serial.print(F("Starting in "));
    Serial.println(i);
    delay(1000);
  }
  Serial.println(F("Hold still..."));

  double sumGX = 0, sumGY = 0, sumGZ = 0;
  double sumGX2 = 0, sumGY2 = 0, sumGZ2 = 0;
  double sumPitch = 0, sumRoll = 0;
  float minPitch = 999, maxPitch = -999, minRoll = 999, maxRoll = -999;
  uint32_t n = 0;

  const unsigned long intervalUs = 1000000UL / MPU_HZ;
  unsigned long start = millis();
  unsigned long nextUs = micros();
  unsigned long lastDot = start;

  while (millis() - start < CALIBRATION_MS) {
    while ((long)(micros() - nextUs) < 0) { }
    nextUs += intervalUs;

    float ax, ay, az, gx, gy, gz;
    if (!mpuRead(ax, ay, az, gx, gy, gz)) continue;

    float p = accelPitch(ax, ay, az);
    float r = accelRoll(ay, az);

    sumGX += gx; sumGY += gy; sumGZ += gz;
    sumGX2 += (double)gx * gx; sumGY2 += (double)gy * gy; sumGZ2 += (double)gz * gz;
    sumPitch += p; sumRoll += r;
    if (p < minPitch) minPitch = p;
    if (p > maxPitch) maxPitch = p;
    if (r < minRoll) minRoll = r;
    if (r > maxRoll) maxRoll = r;
    n++;

    if (millis() - lastDot >= 1000) {
      Serial.print('.');
      lastDot = millis();
    }
  }
  Serial.println();

  if (n < 100) {
    Serial.println(F("Calibration failed: not enough MPU6050 readings. Check wiring."));
    return false;
  }

  float mgx = sumGX / n, mgy = sumGY / n, mgz = sumGZ / n;
  float stdX = sqrt(fmax(0.0, sumGX2 / n - (double)mgx * mgx));
  float stdY = sqrt(fmax(0.0, sumGY2 / n - (double)mgy * mgy));
  float stdZ = sqrt(fmax(0.0, sumGZ2 / n - (double)mgz * mgz));

  if (stdX > MAX_GYRO_STD || stdY > MAX_GYRO_STD || stdZ > MAX_GYRO_STD ||
      (maxPitch - minPitch) > MAX_ANGLE_RANGE || (maxRoll - minRoll) > MAX_ANGLE_RANGE) {
    Serial.println(F("Calibration rejected: head moved too much. Retrying..."));
    return false;
  }

  gyroBiasX = mgx; gyroBiasY = mgy; gyroBiasZ = mgz;
  pitchZero = sumPitch / n;
  rollZero  = sumRoll / n;

  Serial.println(F("Calibration OK."));
  Serial.print(F("Forward pose  pitch=")); Serial.print(pitchZero, 1);
  Serial.print(F("  roll="));              Serial.println(rollZero, 1);
  Serial.println(F("Type 'c' to recalibrate."));
  Serial.println();
  return true;
}

void mpuSetup() {
  // Make sure the MPU6050 is actually on the bus
  Wire.beginTransmission(MPU_ADDRESS);
  if (Wire.endTransmission() != 0) {
    Serial.println("MPU6050 was not found. Check wiring/power and the 0x68/0x69 address.");
    while (1);
  }

  mpuWriteReg(REG_PWR_MGMT_1, 0x00);    // wake up
  delay(100);
  mpuWriteReg(REG_CONFIG, 0x03);        // low-pass filter ~44 Hz
  mpuWriteReg(REG_GYRO_CONFIG, 0x00);   // +/-250 dps
  mpuWriteReg(REG_ACCEL_CONFIG, 0x00);  // +/-2 g
  delay(500);

  while (!mpuCalibrate()) { }

  pitch = pitchZero;
  roll = rollZero;
  lastMpuMicros = micros();
  nextMpuUs = micros();
}

// Non-blocking: does one filter update when it is time, otherwise returns right away.
void updateMpu() {
  // Recalibrate on request
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'c' || c == 'C') {
      while (!mpuCalibrate()) { }
      pitch = pitchZero;
      roll = rollZero;
      droopTiming = false;
      droopActive = false;
      lastMpuMicros = micros();
      nextMpuUs = micros();
      return;
    }
  }

  const unsigned long intervalUs = 1000000UL / MPU_HZ;
  if ((long)(micros() - nextMpuUs) < 0) return;
  nextMpuUs += intervalUs;

  float ax, ay, az, gx, gy, gz;
  if (!mpuRead(ax, ay, az, gx, gy, gz)) return;

  unsigned long nowUs = micros();
  float dt = (nowUs - lastMpuMicros) / 1e6;
  lastMpuMicros = nowUs;

  gx -= gyroBiasX;
  gy -= gyroBiasY;

  // Complementary filter. Pitch rotates about the sensor Y axis, roll about X.
  // If your mounting differs, swap/flip these gyro axes.
  float pitchRate = PITCH_SIGN * -gy;
  float rollRate  = ROLL_SIGN * gx;

  pitch = ALPHA * (pitch + pitchRate * dt) + (1.0 - ALPHA) * accelPitch(ax, ay, az);
  roll  = ALPHA * (roll  + rollRate  * dt) + (1.0 - ALPHA) * accelRoll(ay, az);

  pitchRel = pitch - pitchZero;   // + = nodding down (with PITCH_SIGN = 1 and standard mounting)
  rollRel  = roll - rollZero;

  // Droop: beyond threshold, held for DROOP_HOLD_MS
  bool beyond = (pitchRel > DROOP_PITCH_DEG) || (fabs(rollRel) > DROOP_ROLL_DEG);
  unsigned long nowMs = millis();

  if (beyond) {
    if (!droopTiming) {
      droopTiming = true;
      droopStartMs = nowMs;
    }
    if (nowMs - droopStartMs >= DROOP_HOLD_MS) droopActive = true;
  } else {
    droopTiming = false;
    droopActive = false;
  }
}

// ================= Setup =================
void setup() {
  Serial.begin(230400);

  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);
  analogReadResolution(12);

  Wire.begin();
  Wire.setClock(400000);

  // Calibrate the head sensor FIRST so the MAX30105 FIFO isn't sitting idle (and
  // overflowing) during the 10 s hold-still period.
  mpuSetup();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 was not found. Please check wiring/power.");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);
  particleSensor.setPulseAmplitudeGreen(0);

  delay(1000);
  nextSampleMicros = micros();
}

void updateMax30105() {
  particleSensor.check();

  while (particleSensor.available()) {
    long irValue = particleSensor.getFIFOIR();
    long redValue = particleSensor.getFIFORed();

    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();
      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;
        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
      }
    }

    irBuffer[bufferIndex] = irValue;
    redBuffer[bufferIndex] = redValue;
    bufferIndex++;

    if (bufferIndex == BUFFER_SIZE) {
      maxim_heart_rate_and_oxygen_saturation(
        irBuffer, BUFFER_SIZE, redBuffer,
        &spo2, &validSPO2, &heartRateAlgo, &validHeartRate);
      bufferIndex = 0;
    }

    particleSensor.nextSample();
  }
}

// Start a die-temp conversion every TEMP_INTERVAL_MS, read the result ~40 ms later.
// Never waits, so ECG sampling is not disturbed.
void updateTemperature() {
  unsigned long ms = millis();

  if (!tempConverting && ms - lastTempStart >= TEMP_INTERVAL_MS) {
    lastTempStart = ms;
    tempStartedAt = ms;
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(0x21);  // DIE_TEMP_CONFIG
    Wire.write(0x01);  // start one conversion
    Wire.endTransmission();
    tempConverting = true;
  } else if (tempConverting && ms - tempStartedAt >= 40) {
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(0x1F);  // DIE_TEMP_INT, then DIE_TEMP_FRAC
    Wire.endTransmission(false);
    Wire.requestFrom(MAX_ADDR, (uint8_t)2);
    if (Wire.available() >= 2) {
      int8_t whole = (int8_t)Wire.read();
      uint8_t frac = Wire.read();
      float tempC = (float)whole + ((float)frac * 0.0625);
      temperatureF = tempC * 9.0 / 5.0 + 32.0 + TEMP_OFFSET_F;
    }
    tempConverting = false;
  }
}

void loop() {
  // Keep sensors serviced (fast, non-blocking)
  updateMax30105();
  updateTemperature();
  updateMpu();

  // ECG sample on a fixed schedule
  unsigned long now = micros();
  if ((long)(now - nextSampleMicros) >= 0) {
    nextSampleMicros += SAMPLE_INTERVAL;

    unsigned long timeMs = millis();
    int adc = analogRead(ECG_PIN);
    float volts = adc * ADC_REF_VOLTS / ADC_MAX;
    int leadsOff = (digitalRead(LO_PLUS_PIN) == HIGH || digitalRead(LO_MINUS_PIN) == HIGH);

    Serial.print("Time:");
    Serial.print(timeMs);
    Serial.print(",ADC:");
    Serial.print(adc);
    Serial.print(",Volts:");
    Serial.print(volts, 4);
    Serial.print(",LeadsOff:");
    Serial.print(leadsOff);
    Serial.print(",BPM:");
    Serial.print(beatAvg);
    Serial.print(",SpO2:");
    Serial.print(validSPO2 ? spo2 : 0);
    Serial.print(",TempF:");
    Serial.print(temperatureF, 1);
    Serial.print(",Pitch:");
    Serial.print(pitchRel, 1);
    Serial.print(",Roll:");
    Serial.print(rollRel, 1);
    Serial.print(",Droop:");
    Serial.println(droopActive ? 1 : 0);
  }
}
