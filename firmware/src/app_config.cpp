#include "app_config.h"
#include <ArduinoJson.h>
#include <SD.h>

AppConfig Cfg;

static void setSensor(SensorCfg &s, const char *name, const char *unit,
                      uint8_t source, uint8_t slave, uint16_t reg,
                      uint8_t dtype, float in_min, float in_max,
                      uint8_t decimals) {
  s.enabled = true;
  strlcpy(s.name, name, sizeof(s.name));
  strlcpy(s.unit, unit, sizeof(s.unit));
  s.source = source;
  s.slave = slave;
  s.func = 3;
  s.reg = reg;
  s.dtype = dtype;
  s.scale = 1.0f;
  s.offset = 0.0f;
  s.in_min = in_min;
  s.in_max = in_max;
  s.decimals = decimals;
}

void AppConfig::setDefaults() {
  // NOTE: no memset(this,...) — IPAddress members have vtables
  memset(sensors, 0, sizeof(sensors));
  memset(apn_user, 0, sizeof(apn_user));
  memset(apn_pass, 0, sizeof(apn_pass));
  strlcpy(station, "WYND_STATION_01", sizeof(station));
  strlcpy(ftp_host, "192.168.1.100", sizeof(ftp_host));
  ftp_port = 21;
  strlcpy(ftp_user, "station", sizeof(ftp_user));
  strlcpy(ftp_pass, "station", sizeof(ftp_pass));
  strlcpy(ftp_dir, "/", sizeof(ftp_dir));
  strlcpy(field_sep, "\t", sizeof(field_sep));
  send_interval_s = 300;
  poll_interval_s = 1;   // one full sweep of every channel per second
  strlcpy(apn, "v-internet", sizeof(apn));
  wifi_enabled = false;
  memset(wifi_ssid, 0, sizeof(wifi_ssid));
  memset(wifi_pass, 0, sizeof(wifi_pass));
  mqtt_enabled = false;
  mqtt_tls = false;
  memset(mqtt_host, 0, sizeof(mqtt_host));
  mqtt_port = 1883;
  memset(mqtt_user, 0, sizeof(mqtt_user));
  memset(mqtt_pass, 0, sizeof(mqtt_pass));
  memset(mqtt_topic, 0, sizeof(mqtt_topic));
  retention_days = 365;
  buffer_max = 3000;
  filter_alpha = 0.4f;
  filter_jump_pct = 15;
  eth_dhcp = true;
  ip = IPAddress(192, 168, 1, 210);
  gw = IPAddress(192, 168, 1, 1);
  mask = IPAddress(255, 255, 255, 0);
  dns = IPAddress(8, 8, 8, 8);
  rs485_baud = 9600;
  rs485_parity = 0;
  r1_mode = RELAY_MANUAL;
  r2_mode = RELAY_AUTO;
  pump_period_s = 3600;
  pump_on_s = 60;
  strlcpy(alarm_param, "pH", sizeof(alarm_param));
  alarm_high = 9.0f;
  alarm_low = 8.5f;

  // Default map: 4-20mA temperature transmitter on AI1 (0-150C) plus a
  // multiparameter probe on RS485 (slave 1, consecutive float registers).
  // Adjust in /config.json.
  uint8_t n = 0;
  setSensor(sensors[n++], "Temp", "C",    SRC_AI1,    0, 0,  DT_U16, 0.0f, 150.0f, 1);
  setSensor(sensors[n++], "pH",   "-",    SRC_MODBUS, 1, 2,  DT_FLOAT_ABCD, 0, 0, 2);
  setSensor(sensors[n++], "DO",   "mg/L", SRC_MODBUS, 1, 4,  DT_FLOAT_ABCD, 0, 0, 2);
  setSensor(sensors[n++], "EC",   "uS/cm",SRC_MODBUS, 1, 6,  DT_FLOAT_ABCD, 0, 0, 0);
  setSensor(sensors[n++], "TUR",  "NTU",  SRC_MODBUS, 1, 8,  DT_FLOAT_ABCD, 0, 0, 1);
  setSensor(sensors[n++], "COD",  "mg/L", SRC_MODBUS, 2, 0,  DT_FLOAT_ABCD, 0, 0, 1);
  setSensor(sensors[n++], "TSS",  "mg/L", SRC_MODBUS, 2, 2,  DT_FLOAT_ABCD, 0, 0, 1);
  // Analog channel templates: flip "enabled" and set name/range in
  // /config.json when a transmitter is wired. Same filtering path as AI1.
  setSensor(sensors[n++], "AI2",  "-",    SRC_AI2,    0, 0,  DT_U16, 0.0f, 100.0f, 1);
  sensors[n - 1].enabled = false;
  setSensor(sensors[n++], "AI3",  "-",    SRC_AI3,    0, 0,  DT_U16, 0.0f, 100.0f, 1);
  sensors[n - 1].enabled = false;
  setSensor(sensors[n++], "AI4",  "-",    SRC_AI4,    0, 0,  DT_U16, 0.0f, 100.0f, 1);
  sensors[n - 1].enabled = false;
  setSensor(sensors[n++], "VI1",  "V",    SRC_VI1,    0, 0,  DT_U16, 0.0f, 24.0f, 2);
  sensors[n - 1].enabled = false;
  sensor_count = n;
}

static void sensorToJson(JsonObject o, const SensorCfg &s) {
  o["enabled"] = s.enabled;
  o["name"] = s.name;
  o["unit"] = s.unit;
  const char *src = "modbus";
  switch (s.source) {
    case SRC_AI1: src = "ai1"; break;
    case SRC_AI2: src = "ai2"; break;
    case SRC_AI3: src = "ai3"; break;
    case SRC_AI4: src = "ai4"; break;
    case SRC_VI1: src = "vi1"; break;
  }
  o["source"] = src;
  o["slave"] = s.slave;
  o["func"] = s.func;
  o["reg"] = s.reg;
  const char *dt = "u16";
  switch (s.dtype) {
    case DT_S16: dt = "s16"; break;
    case DT_U32_BE: dt = "u32"; break;
    case DT_FLOAT_ABCD: dt = "float"; break;
    case DT_FLOAT_CDAB: dt = "float_sw"; break;
  }
  o["type"] = dt;
  o["scale"] = s.scale;
  o["offset"] = s.offset;
  o["range_min"] = s.in_min;
  o["range_max"] = s.in_max;
  o["decimals"] = s.decimals;
}

static void sensorFromJson(JsonObject o, SensorCfg &s) {
  s.enabled = o["enabled"] | true;
  strlcpy(s.name, o["name"] | "?", sizeof(s.name));
  strlcpy(s.unit, o["unit"] | "-", sizeof(s.unit));
  String src = String((const char *)(o["source"] | "modbus"));
  if (src == "ai1") s.source = SRC_AI1;
  else if (src == "ai2") s.source = SRC_AI2;
  else if (src == "ai3") s.source = SRC_AI3;
  else if (src == "ai4") s.source = SRC_AI4;
  else if (src == "vi1") s.source = SRC_VI1;
  else s.source = SRC_MODBUS;
  s.slave = o["slave"] | 1;
  s.func = o["func"] | 3;
  s.reg = o["reg"] | 0;
  String dt = String((const char *)(o["type"] | "float"));
  if (dt == "u16") s.dtype = DT_U16;
  else if (dt == "s16") s.dtype = DT_S16;
  else if (dt == "u32") s.dtype = DT_U32_BE;
  else if (dt == "float_sw") s.dtype = DT_FLOAT_CDAB;
  else s.dtype = DT_FLOAT_ABCD;
  s.scale = o["scale"] | 1.0f;
  s.offset = o["offset"] | 0.0f;
  s.in_min = o["range_min"] | 0.0f;
  s.in_max = o["range_max"] | 100.0f;
  s.decimals = o["decimals"] | 2;
}

bool AppConfig::saveToSD() {
  JsonDocument doc;
  doc["station"] = station;
  JsonObject ftp = doc["ftp"].to<JsonObject>();
  ftp["host"] = ftp_host;
  ftp["port"] = ftp_port;
  ftp["user"] = ftp_user;
  ftp["pass"] = ftp_pass;
  ftp["dir"] = ftp_dir;
  doc["field_sep"] = field_sep;
  doc["send_interval_s"] = send_interval_s;
  doc["poll_interval_s"] = poll_interval_s;
  JsonObject lte = doc["lte"].to<JsonObject>();
  lte["apn"] = apn;
  lte["user"] = apn_user;
  lte["pass"] = apn_pass;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["enabled"] = wifi_enabled;
  wifi["ssid"] = wifi_ssid;
  wifi["pass"] = wifi_pass;
  // NOTE: "tls" is intentionally not written: it derives from the port
  // (8883 = TLS, other = plain); hand-add "tls" in the file to override.
  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = mqtt_enabled;
  mq["host"] = mqtt_host;
  mq["port"] = mqtt_port;
  mq["user"] = mqtt_user;
  mq["pass"] = mqtt_pass;
  mq["topic"] = mqtt_topic;
  JsonObject st = doc["storage"].to<JsonObject>();
  st["retention_days"] = retention_days;
  st["buffer_max"] = buffer_max;
  JsonObject flt = doc["filter"].to<JsonObject>();
  flt["alpha"] = filter_alpha;
  flt["jump_pct"] = filter_jump_pct;
  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["dhcp"] = eth_dhcp;
  eth["ip"] = ip.toString();
  eth["gateway"] = gw.toString();
  eth["mask"] = mask.toString();
  eth["dns"] = dns.toString();
  JsonObject rs = doc["rs485"].to<JsonObject>();
  rs["baud"] = rs485_baud;
  rs["parity"] = rs485_parity;
  JsonObject ctl = doc["control"].to<JsonObject>();
  ctl["relay1_mode"] = r1_mode == RELAY_AUTO ? "auto" : "manual";
  ctl["relay2_mode"] = r2_mode == RELAY_AUTO ? "auto" : "manual";
  ctl["pump_period_s"] = pump_period_s;
  ctl["pump_on_s"] = pump_on_s;
  ctl["alarm_param"] = alarm_param;
  ctl["alarm_high"] = alarm_high;
  ctl["alarm_low"] = alarm_low;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < sensor_count; i++)
    sensorToJson(arr.add<JsonObject>(), sensors[i]);

  SD.remove("/config.json");
  File f = SD.open("/config.json", FILE_WRITE);
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();
  return true;
}

bool AppConfig::loadFromSD() {
  File f = SD.open("/config.json", FILE_READ);
  if (!f) {
    // First boot with a blank card: write the default template so operators
    // can edit it on a PC.
    saveToSD();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    log_e("config.json parse error: %s", err.c_str());
    return false;
  }
  strlcpy(station, doc["station"] | station, sizeof(station));
  JsonObject ftp = doc["ftp"];
  if (!ftp.isNull()) {
    strlcpy(ftp_host, ftp["host"] | ftp_host, sizeof(ftp_host));
    ftp_port = ftp["port"] | ftp_port;
    strlcpy(ftp_user, ftp["user"] | ftp_user, sizeof(ftp_user));
    strlcpy(ftp_pass, ftp["pass"] | ftp_pass, sizeof(ftp_pass));
    strlcpy(ftp_dir, ftp["dir"] | ftp_dir, sizeof(ftp_dir));
  }
  strlcpy(field_sep, doc["field_sep"] | field_sep, sizeof(field_sep));
  send_interval_s = doc["send_interval_s"] | send_interval_s;
  poll_interval_s = doc["poll_interval_s"] | poll_interval_s;
  JsonObject lte = doc["lte"];
  if (!lte.isNull()) {
    strlcpy(apn, lte["apn"] | apn, sizeof(apn));
    strlcpy(apn_user, lte["user"] | apn_user, sizeof(apn_user));
    strlcpy(apn_pass, lte["pass"] | apn_pass, sizeof(apn_pass));
  }
  JsonObject wifi = doc["wifi"];
  if (!wifi.isNull()) {
    wifi_enabled = wifi["enabled"] | wifi_enabled;
    strlcpy(wifi_ssid, wifi["ssid"] | wifi_ssid, sizeof(wifi_ssid));
    strlcpy(wifi_pass, wifi["pass"] | wifi_pass, sizeof(wifi_pass));
  }
  JsonObject mq = doc["mqtt"];
  if (!mq.isNull()) {
    mqtt_enabled = mq["enabled"] | mqtt_enabled;
    strlcpy(mqtt_host, mq["host"] | mqtt_host, sizeof(mqtt_host));
    mqtt_port = mq["port"] | mqtt_port;
    // convention: 8883 = TLS, any other port = plain MQTT.
    // An explicit "tls" key in config.json still overrides.
    mqtt_tls = mq["tls"] | (mqtt_port == 8883);
    strlcpy(mqtt_user, mq["user"] | mqtt_user, sizeof(mqtt_user));
    strlcpy(mqtt_pass, mq["pass"] | mqtt_pass, sizeof(mqtt_pass));
    strlcpy(mqtt_topic, mq["topic"] | mqtt_topic, sizeof(mqtt_topic));
  }
  JsonObject st = doc["storage"];
  if (!st.isNull()) {
    retention_days = st["retention_days"] | retention_days;
    buffer_max = st["buffer_max"] | buffer_max;
  }
  JsonObject flt = doc["filter"];
  if (!flt.isNull()) {
    filter_alpha = flt["alpha"] | filter_alpha;
    if (filter_alpha < 0.05f) filter_alpha = 0.05f;
    if (filter_alpha > 1.0f) filter_alpha = 1.0f;
    filter_jump_pct = flt["jump_pct"] | filter_jump_pct;
  }
  JsonObject eth = doc["ethernet"];
  if (!eth.isNull()) {
    eth_dhcp = eth["dhcp"] | eth_dhcp;
    IPAddress tmp;
    if (tmp.fromString((const char *)(eth["ip"] | ""))) ip = tmp;
    if (tmp.fromString((const char *)(eth["gateway"] | ""))) gw = tmp;
    if (tmp.fromString((const char *)(eth["mask"] | ""))) mask = tmp;
    if (tmp.fromString((const char *)(eth["dns"] | ""))) dns = tmp;
  }
  JsonObject rs = doc["rs485"];
  if (!rs.isNull()) {
    rs485_baud = rs["baud"] | rs485_baud;
    rs485_parity = rs["parity"] | rs485_parity;
  }
  JsonObject ctl = doc["control"];
  if (!ctl.isNull()) {
    r1_mode = String((const char *)(ctl["relay1_mode"] | "manual")) == "auto" ? RELAY_AUTO : RELAY_MANUAL;
    r2_mode = String((const char *)(ctl["relay2_mode"] | "auto")) == "auto" ? RELAY_AUTO : RELAY_MANUAL;
    pump_period_s = ctl["pump_period_s"] | pump_period_s;
    pump_on_s = ctl["pump_on_s"] | pump_on_s;
    strlcpy(alarm_param, ctl["alarm_param"] | alarm_param, sizeof(alarm_param));
    alarm_high = ctl["alarm_high"] | alarm_high;
    alarm_low = ctl["alarm_low"] | alarm_low;
  }
  JsonArray arr = doc["sensors"];
  if (!arr.isNull() && arr.size() > 0) {
    sensor_count = 0;
    for (JsonObject o : arr) {
      if (sensor_count >= MAX_SENSORS) break;
      sensorFromJson(o, sensors[sensor_count++]);
    }
  }
  return true;
}
