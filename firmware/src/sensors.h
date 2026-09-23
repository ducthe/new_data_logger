#pragma once
#include <Arduino.h>
#include "app_config.h"
#include "modbus_rtu.h"

// MONRE device status codes carried per parameter
#define ST_NORMAL  "00"
#define ST_CALIB   "01"
#define ST_ERROR   "02"

struct SensorValue {
  float    value = NAN;
  float    filt = NAN;          // EMA state for analog channels
  bool     ok = false;          // last read succeeded
  uint32_t lastOkMs = 0;
  uint32_t errStreak = 0;
  float    rawMa = NAN;         // for analog channels: measured loop mA
  // averaging window for the transmit interval
  double   sum = 0;
  uint32_t cnt = 0;
  bool     windowErr = false;   // any failure during current window

  void accumulate() {
    if (ok && !isnan(value)) { sum += value; cnt++; }
    else windowErr = true;
  }
  float windowAvg() const { return cnt ? (float)(sum / cnt) : NAN; }
  void resetWindow() { sum = 0; cnt = 0; windowErr = false; }
};

class SensorManager {
 public:
  void begin();
  void tick();                       // call from the main loop
  const SensorValue &value(uint8_t i) const { return _vals[i]; }
  bool calibrationMode = false;      // inspectors: flags data as "01"
  const char *paramStatus(uint8_t i) const;
  bool di1Active() const;            // true when field voltage present
  ModbusRTU bus;

 private:
  void readSensor(uint8_t i);
  float readAnalog(const SensorCfg &s, float &rawMa);
  SensorValue _vals[MAX_SENSORS];
  uint8_t _next = 0;
  uint32_t _lastStepMs = 0;
};

extern SensorManager Sensors;
