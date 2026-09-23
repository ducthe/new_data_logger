#include "monre.h"
#include <time.h>
#include "app_config.h"
#include "sensors.h"
#include "storage.h"
#include "net_manager.h"
#include "ftp_client.h"
#include "mqtt_uplink.h"
#include "pins.h"

MonreUplink Uplink;

static void timestampNow(char *buf, size_t len) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  snprintf(buf, len, "%04d%02d%02d%02d%02d%02d", tmv.tm_year + 1900,
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

uint32_t MonreUplink::nextSendInMs() const {
  uint32_t iv = (uint32_t)Cfg.send_interval_s * 1000UL;
  uint32_t el = millis() - _lastSendMs;
  return el >= iv ? 0 : iv - el;
}

String MonreUplink::buildFile(String &filename) {
  char ts[16];
  timestampNow(ts, sizeof(ts));
  filename = String(Cfg.station) + "_" + ts + ".txt";
  String sep = Cfg.field_sep;
  if (sep.length() == 0) sep = "\t";
  String out;
  out.reserve(64 * Cfg.sensor_count);
  for (uint8_t i = 0; i < Cfg.sensor_count; i++) {
    const SensorCfg &s = Cfg.sensors[i];
    if (!s.enabled) continue;
    const SensorValue &v = Sensors.value(i);
    float val = v.windowAvg();
    if (isnan(val)) val = v.ok ? v.value : 0.0f;
    if (isnan(val)) val = 0.0f;
    char num[20];
    dtostrf(val, 0, s.decimals, num);
    out += String(s.name) + sep + num + sep + s.unit + sep + ts + sep +
           Sensors.paramStatus(i) + "\r\n";
  }
  return out;
}

bool MonreUplink::transmit(const String &filename, const String &content,
                           String &err) {
  Client *ctrl = Net.ctrlClient();
  Client *data = Net.dataClient();
  if (!ctrl || !data) { err = "no network link"; return false; }
  ledWrite(PIN_LED_WIFI, true);  // TX activity indicator
  bool ok = FtpUploader::put(*ctrl, *data, Cfg.ftp_host, Cfg.ftp_port,
                             Cfg.ftp_user, Cfg.ftp_pass, Cfg.ftp_dir,
                             filename.c_str(), content, err);
  ledWrite(PIN_LED_WIFI, false);
  return ok;
}

void MonreUplink::retryBuffered() {
  // push up to 3 backlog files per cycle so a long outage drains gradually
  for (int k = 0; k < 3; k++) {
    String name, content;
    if (!Store.oldestBuffered(name, content)) return;
    String err;
    if (!transmit(name, content, err)) {
      log_w("backlog send failed (%s): %s", name.c_str(), err.c_str());
      return;
    }
    log_i("backlog sent: %s", name.c_str());
    Store.removeBuffered(name);
  }
}

bool MonreUplink::sendNow() {
  sending = true;
  lastAttemptMs = millis();
  String filename;
  String content = buildFile(filename);

  // also log the snapshot locally
  {
    char ts[16];
    timestampNow(ts, sizeof(ts));
    String csv = String(ts);
    for (uint8_t i = 0; i < Cfg.sensor_count; i++) {
      const SensorValue &v = Sensors.value(i);
      float val = v.windowAvg();
      if (isnan(val)) val = v.value;
      csv += ",";
      csv += isnan(val) ? "NaN" : String(val, 3);
    }
    Store.appendCsv(csv);
  }

  // realtime MQTT mirror of the same cycle averages (independent of FTP)
  if (Net.timeSynced()) {
    char ts[16];
    timestampNow(ts, sizeof(ts));
    Mqtt.publishCycle(ts);
  }

  bool ok = false;
  const bool ftpConfigured = strlen(Cfg.ftp_host) > 0;
  String err = "no time sync";
  if (!ftpConfigured) {
    lastResult = "FTP not configured";
  } else if (Net.timeSynced()) {
    if (content.length() == 0) { err = "no data"; }
    else ok = transmit(filename, content, err);
  }
  if (ok) {
    okCount++;
    lastResult = "OK " + filename;
    log_i("MONRE sent: %s", filename.c_str());
    retryBuffered();
  } else if (ftpConfigured) {
    failCount++;
    lastResult = "FAIL: " + err;
    log_w("MONRE send failed: %s", err.c_str());
    Store.logEvent("FTP send FAILED: %.100s", err.c_str());
    // buffer only real, timestamped data
    if (Net.timeSynced() && content.length() > 0)
      Store.bufferFile(filename, content);
  }

  // reset averaging windows for the next interval
  for (uint8_t i = 0; i < Cfg.sensor_count; i++)
    const_cast<SensorValue &>(Sensors.value(i)).resetWindow();

  sending = false;
  return ok;
}

void MonreUplink::tick() {
  if (millis() - _lastSendMs < (uint32_t)Cfg.send_interval_s * 1000UL) return;
  _lastSendMs = millis();
  sendNow();
}
