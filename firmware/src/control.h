#pragma once
#include <Arduino.h>

// Relay 1: sampling pump (manual, or AUTO duty cycle pump_on_s/pump_period_s)
// Relay 2: discharge valve / alarm output (manual, or AUTO on threshold of
//          Cfg.alarm_param with hysteresis)
class ControlManager {
 public:
  void begin();
  void tick();
  void setRelay(uint8_t idx, bool on);   // manual command (idx 0/1)
  bool relayState(uint8_t idx) const { return _state[idx & 1]; }
  bool alarmActive() const { return _alarm; }
  float alarmValue() const { return _alarmValue; }

 private:
  void apply(uint8_t idx, bool on);
  bool _state[2] = {false, false};
  bool _alarm = false;
  float _alarmValue = NAN;
  uint32_t _pumpCycleStartMs = 0;
};

extern ControlManager Control;
