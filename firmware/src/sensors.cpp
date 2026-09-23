#include "sensors.h"
#include <algorithm>
#include "pins.h"

SensorManager Sensors;

void SensorManager::begin() {
  bus.begin(Serial2, Cfg.rs485_baud, Cfg.rs485_parity,
            PIN_RS485_RX, PIN_RS485_TX, PIN_RS485_DE);
  pinMode(PIN_DI1, INPUT);
  // 4-20mA inputs land at 0.4-2.0V: default 11dB attenuation covers this
  analogReadResolution(12);
}

bool SensorManager::di1Active() const {
  // Field 3-24V pulls the GPIO low through Q9
  return digitalRead(PIN_DI1) == LOW;
}

const char *SensorManager::paramStatus(uint8_t i) const {
  if (calibrationMode) return ST_CALIB;
  if (_vals[i].windowErr || !_vals[i].ok) return ST_ERROR;
  return ST_NORMAL;
}

float SensorManager::readAnalog(const SensorCfg &s, float &rawMa) {
  int pin = -1;
  switch (s.source) {
    case SRC_AI1: pin = PIN_AI1; break;
    case SRC_AI2: pin = PIN_AI2; break;
    case SRC_AI3: pin = PIN_AI3; break;
    case SRC_AI4: pin = PIN_AI4; break;
    case SRC_VI1: pin = PIN_VI1; break;
    default: return NAN;
  }
  // 32-sample trimmed mean spread across one 50Hz mains cycle (20ms) so
  // supply/loop ripple integrates to zero; drop extremes, average the rest.
  uint16_t smp[32];
  for (int k = 0; k < 32; k++) {
    smp[k] = analogReadMilliVolts(pin);
    delayMicroseconds(500);
  }
  std::sort(smp, smp + 32);
  uint32_t mv = 0;
  for (int k = 2; k < 30; k++) mv += smp[k];
  mv /= 28;

  if (s.source == SRC_VI1) {
    // 0-24V scaled to 0-2V
    rawMa = NAN;
    float volts = mv * 12.0f / 1000.0f;
    return s.in_min + volts / 24.0f * (s.in_max - s.in_min);
  }
  // 4-20mA through 100R effective: 0.4-2.0V
  rawMa = mv / 100.0f;
  if (rawMa < 3.0f) return NAN;  // open loop / sensor fault
  float v = s.in_min + (rawMa - 4.0f) / 16.0f * (s.in_max - s.in_min);
  return v * s.scale + s.offset;
}

void SensorManager::readSensor(uint8_t i) {
  const SensorCfg &s = Cfg.sensors[i];
  SensorValue &v = _vals[i];
  float result = NAN;
  bool ok = false;

  if (s.source == SRC_MODBUS) {
    uint16_t words[2] = {0, 0};
    uint16_t count = (s.dtype == DT_U16 || s.dtype == DT_S16) ? 1 : 2;
    if (bus.readRegisters(s.slave, s.func, s.reg, count, words)) {
      float raw;
      switch (s.dtype) {
        case DT_U16: raw = words[0]; break;
        case DT_S16: raw = (int16_t)words[0]; break;
        case DT_U32_BE: raw = ((uint32_t)words[0] << 16) | words[1]; break;
        case DT_FLOAT_CDAB: {
          uint32_t u = ((uint32_t)words[1] << 16) | words[0];
          memcpy(&raw, &u, 4);
          break;
        }
        case DT_FLOAT_ABCD:
        default: {
          uint32_t u = ((uint32_t)words[0] << 16) | words[1];
          memcpy(&raw, &u, 4);
          break;
        }
      }
      result = raw * s.scale + s.offset;
      ok = !isnan(result) && !isinf(result);
    }
  } else {
    result = readAnalog(s, v.rawMa);
    ok = !isnan(result);
    if (ok) {
      if (isnan(v.filt)) {
        v.filt = result;          // first reading seeds the filter, no ramp
      } else {
        // A real step (supply switched on, valve opened, probe swapped) must
        // not crawl in over many polls, so re-seed when the reading moves
        // more than filter_jump_pct of the channel span; otherwise apply the
        // EMA, which only smooths residual noise.
        float span = fabsf(s.in_max - s.in_min);
        if (span <= 0.0f) span = fabsf(v.filt);
        float jump = span * (Cfg.filter_jump_pct / 100.0f);
        if (Cfg.filter_jump_pct > 0 && jump > 0.0f &&
            fabsf(result - v.filt) > jump)
          v.filt = result;
        else
          v.filt += Cfg.filter_alpha * (result - v.filt);
      }
      result = v.filt;
    } else {
      v.filt = NAN;  // restart filter after loop fault
    }
  }

  v.value = result;
  v.ok = ok;
  if (ok) { v.lastOkMs = millis(); v.errStreak = 0; }
  else v.errStreak++;
  v.accumulate();
}

void SensorManager::tick() {
  // Round-robin: one sensor per step so a dead RS485 slave (300ms timeout)
  // never stalls the UI noticeably.
  // Floor of 25ms (not 200ms) so poll_interval_s = 1 gives a genuine 1s
  // sweep even with a dozen channels; one analog read costs ~20ms.
  uint32_t step = max<uint32_t>(25, (uint32_t)Cfg.poll_interval_s * 1000UL /
                                        max<uint8_t>(1, Cfg.sensor_count));
  if (millis() - _lastStepMs < step) return;
  _lastStepMs = millis();
  if (Cfg.sensor_count == 0) return;
  uint8_t i = _next % Cfg.sensor_count;
  _next = (i + 1) % Cfg.sensor_count;
  if (Cfg.sensors[i].enabled) readSensor(i);
}
