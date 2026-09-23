#include "control.h"
#include "pins.h"
#include "app_config.h"
#include "sensors.h"
#include "storage.h"

ControlManager Control;

void ControlManager::begin() {
  // relays de-energized at boot (also done first thing in setup())
  apply(0, false);
  apply(1, false);
  _pumpCycleStartMs = millis();
}

void ControlManager::apply(uint8_t idx, bool on) {
  idx &= 1;
  bool changed = (_state[idx] != on);
  _state[idx] = on;
  digitalWrite(idx == 0 ? PIN_RELAY1 : PIN_RELAY2, on ? HIGH : LOW);
  if (changed)
    Store.logEvent("relay%u (%s) -> %s", idx + 1,
                   idx == 0 ? "pump" : "valve", on ? "ON" : "OFF");
}

void ControlManager::setRelay(uint8_t idx, bool on) {
  uint8_t mode = (idx == 0) ? Cfg.r1_mode : Cfg.r2_mode;
  if (mode != RELAY_MANUAL) return;  // AUTO owns the output
  apply(idx, on);
}

void ControlManager::tick() {
  uint32_t now = millis();

  // Relay 1 AUTO: periodic sampling pump
  if (Cfg.r1_mode == RELAY_AUTO && Cfg.pump_period_s > 0) {
    uint32_t period = (uint32_t)Cfg.pump_period_s * 1000UL;
    uint32_t onTime = (uint32_t)Cfg.pump_on_s * 1000UL;
    uint32_t phase = (now - _pumpCycleStartMs) % period;
    bool want = phase < onTime;
    if (want != _state[0]) apply(0, want);
  }

  // Relay 2 AUTO: threshold alarm with hysteresis
  if (Cfg.r2_mode == RELAY_AUTO) {
    for (uint8_t i = 0; i < Cfg.sensor_count; i++) {
      if (strcmp(Cfg.sensors[i].name, Cfg.alarm_param) != 0) continue;
      const SensorValue &v = Sensors.value(i);
      if (!v.ok || isnan(v.value)) break;
      _alarmValue = v.value;
      if (!_alarm && v.value >= Cfg.alarm_high) _alarm = true;
      else if (_alarm && v.value <= Cfg.alarm_low) _alarm = false;
      break;
    }
    if (_alarm != _state[1]) apply(1, _alarm);
  }
}
