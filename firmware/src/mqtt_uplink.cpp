#include "mqtt_uplink.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SSLClient.h>
#include <esp_task_wdt.h>
#include "app_config.h"
#include "certs.h"
#include "control.h"
#include "monre.h"
#include "net_manager.h"
#include "ota.h"
#include "pins.h"
#include "sensors.h"
#include "storage.h"

MqttUplink Mqtt;

static PubSubClient s_mqtt;
static LinkType s_boundLink = LinkType::NONE;
// Software TLS (mbedTLS) wrapped around whichever transport is active:
// works uniformly over W5500 Ethernet, WiFi and TinyGSM LTE sockets.
// Compatibility + diagnostic shim between mbedTLS and whichever transport is
// active.
//
// Compatibility: the TLS layer treats a read() result of 0 as "nothing yet,
// try again" but passes anything negative to mbedTLS as a fatal error.
// EthernetClient::read() returns -1 when the W5500 has no data, so every
// TLS handshake over Ethernet died on the first idle poll. Clamping negative
// results to 0 lets mbedTLS use its normal WANT_READ retry path, which is
// far more robust than the library's own W5500 workaround (a tight 200-
// iteration spin that expires long before a round trip to the broker).
//
// Diagnostics: the counters tell a failed handshake apart from a socket that
// never delivers anything - that is how the TinyGSM read stall was found.
class TapClient : public Client {
 public:
  explicit TapClient(Client *c) : _c(c) {}
  int connect(IPAddress ip, uint16_t p) override { return _c->connect(ip, p); }
  int connect(const char *h, uint16_t p) override { return _c->connect(h, p); }
  size_t write(uint8_t b) override { _tx += 1; return _c->write(b); }
  size_t write(const uint8_t *b, size_t n) override {
    size_t w = _c->write(b, n);
    _tx += w;
    if (w != n) _short++;
    return w;
  }
  int available() override {
    int a = _c->available();
    if (a <= 0) _avail0++; else _availN++;
    return a;
  }
  int read() override { int r = _c->read(); if (r >= 0) _rx++; return r; }
  int read(uint8_t *b, size_t n) override {
    int r = _c->read(b, n);
    _reads++;
    if (r > 0) {
      _rx += r;
    } else {
      _read0++;
      if (r < 0) { _neg++; r = 0; }   // "no data yet", not a fatal error
    }
    return r;
  }
  int peek() override { return _c->peek(); }
  void flush() override { _c->flush(); }
  void stop() override { _c->stop(); }
  uint8_t connected() override { return _c->connected(); }
  operator bool() override { return (bool)*_c; }

  void report(const char *tag) {
    log_i("%s: tx=%u rx=%u reads=%u read0=%u neg=%u availN=%u avail0=%u "
          "short=%u connected=%d", tag, _tx, _rx, _reads, _read0, _neg,
          _availN, _avail0, _short, (int)_c->connected());
  }
  void reset() {
    _tx = _rx = _reads = _read0 = _neg = _availN = _avail0 = _short = 0;
  }

 private:
  Client *_c;
  uint32_t _tx = 0, _rx = 0, _reads = 0, _read0 = 0, _neg = 0;
  uint32_t _availN = 0, _avail0 = 0, _short = 0;
};

static TapClient *s_tap = nullptr;
static SSLClient *s_tls = nullptr;
// TLS material sources, in priority order:
//   1. embedded in firmware (src/certs.h)  - preferred: survives card theft,
//      cannot be swapped by inserting a different SD card
//   2. SD card fallback for entries the firmware leaves empty:
//      /mqtt_ca.pem, /mqtt_cert.pem, /mqtt_key.pem
static String s_caPem, s_certPem, s_keyPem;
static bool s_pemLoaded = false;

static String readPem(const char *path) {
  if (!Store.ready() || !SD.exists(path)) return "";
  File f = SD.open(path, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

static void loadPem(String &dst, const char *embedded, const char *sdPath,
                    const char *what) {
  if (strlen(embedded) > 0) {
    dst = embedded;
    log_i("MQTT: %s embedded in firmware (%u bytes)", what, dst.length());
  } else {
    dst = readPem(sdPath);
    if (dst.length() > 0)
      log_i("MQTT: %s from SD %s (%u bytes)", what, sdPath, dst.length());
  }
}

static void applyTlsConfig(SSLClient *tls) {
  if (!s_pemLoaded) {
    s_pemLoaded = true;
    loadPem(s_caPem, MQTT_CA_CERT, "/mqtt_ca.pem", "CA cert");
    loadPem(s_certPem, MQTT_CLIENT_CERT, "/mqtt_cert.pem", "client cert");
    loadPem(s_keyPem, MQTT_CLIENT_KEY, "/mqtt_key.pem", "client key");
  }
  if (s_caPem.length() > 0) {
    tls->setCACert(s_caPem.c_str());
  } else {
    // encrypted but no server verification - OK for tests only
    tls->setInsecure();
    log_w("MQTT: TLS without CA verification (no CA embedded or on SD)");
  }
  if (s_certPem.length() > 0 && s_keyPem.length() > 0) {
    tls->setCertificate(s_certPem.c_str());
    tls->setPrivateKey(s_keyPem.c_str());
    log_i("MQTT: mutual TLS with client cert/key");
  } else if (s_certPem.length() > 0 || s_keyPem.length() > 0) {
    log_w("MQTT: need BOTH client cert and key for mutual TLS");
  }
  // Do NOT call setTimeout() here. The library's read helper waits until
  // available() >= the number of bytes mbedTLS asked for, so a non-zero
  // timeout makes a single read burn the whole budget on a chunked transport
  // like TinyGSM (measured: one read ate all 30 s and the handshake never
  // progressed). With the read timeout left at 0 each read returns at once,
  // mbedTLS gets WANT_READ and its own loop retries every 10 ms, which is
  // the polling behaviour a modem socket needs.
  // setHandshakeTimeout() only survives while _timeout stays 0, and 30 s
  // keeps the blocking connect well clear of the 120 s task watchdog.
  tls->setHandshakeTimeout(30);   // seconds
}

String MqttUplink::baseTopic() {
  if (strlen(Cfg.mqtt_topic) > 0) return String(Cfg.mqtt_topic);
  return String("wynd/") + Cfg.station;
}

// ------------------------------------------------- downlink (<base>/cmd) --
static void ackf(const char *fmt, ...) {
  char msg[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  String topic = Mqtt.baseTopic() + "/cmd/ack";
  s_mqtt.publish(topic.c_str(), msg, false);
  log_i("MQTT cmd ack: %s", msg);
}

// Commands (JSON on <base>/cmd), acked on <base>/cmd/ack:
//   {"cmd":"status"}                     health snapshot
//   {"cmd":"send"}                       transmit a MONRE cycle now
//   {"cmd":"relay","idx":0,"on":true}    manual relay control
//   {"cmd":"mode","idx":0,"auto":false}  relay mode + persist
//   {"cmd":"interval","s":300}           send interval + persist
//   {"cmd":"calib","on":true}            calibration flag (data status 01)
//   {"cmd":"ota"}                        remote OTA manifest check
//   {"cmd":"reboot"}                     restart the station
static void onCmdMessage(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    ackf("error: bad json");
    return;
  }
  String cmd = doc["cmd"] | "";
  Store.logEvent("MQTT cmd: %.48s", cmd.c_str());

  if (cmd == "status") {
    ackf("fw=%s up=%lus heap=%uk link=%d buffered=%u ftp_ok=%lu ftp_fail=%lu",
         FW_VERSION, (unsigned long)(millis() / 1000),
         (unsigned)(ESP.getFreeHeap() / 1024), (int)Net.active(),
         Store.bufferedCount(), (unsigned long)Uplink.okCount,
         (unsigned long)Uplink.failCount);
  } else if (cmd == "send") {
    bool ok = Uplink.sendNow();
    ackf("send %s: %.120s", ok ? "OK" : "FAIL", Uplink.lastResult.c_str());
  } else if (cmd == "relay") {
    int idx = doc["idx"] | 0;
    bool on = doc["on"] | false;
    Control.setRelay(idx, on);
    ackf("relay%d -> %s (state=%s)", idx, on ? "on" : "off",
         Control.relayState(idx) ? "ON" : "OFF");
  } else if (cmd == "mode") {
    int idx = doc["idx"] | 0;
    bool autoMode = doc["auto"] | false;
    if (idx == 0) Cfg.r1_mode = autoMode ? RELAY_AUTO : RELAY_MANUAL;
    else Cfg.r2_mode = autoMode ? RELAY_AUTO : RELAY_MANUAL;
    Cfg.saveToSD();
    ackf("relay%d mode -> %s", idx, autoMode ? "auto" : "manual");
  } else if (cmd == "interval") {
    int s = doc["s"] | 0;
    if (s < 60 || s > 3600) {
      ackf("error: interval 60..3600s");
    } else {
      Cfg.send_interval_s = (uint16_t)s;
      Cfg.saveToSD();
      ackf("send interval -> %ds", s);
    }
  } else if (cmd == "calib") {
    Sensors.calibrationMode = doc["on"] | false;
    ackf("calibration mode %s", Sensors.calibrationMode ? "ON" : "off");
  } else if (cmd == "ota") {
    const char *url = doc["url"] | "";
    const char *md5 = doc["md5"] | "";
    if (strlen(url) == 0) {
      ackf("error: ota needs \"url\" (http://.../firmware.bin)");
      return;
    }
    ackf("ota: downloading %.120s", url);
    s_mqtt.loop();  // push the ack out before the long download
    String msg;
    Ota.updateFromUrl(url, md5, msg);  // reboots on success
    ackf("ota: %.150s", msg.c_str());  // reached only on failure
  } else if (cmd == "reboot") {
    ackf("rebooting");
    Store.logEvent("remote reboot via MQTT");
    delay(400);
    ESP.restart();
  } else {
    ackf("error: unknown cmd '%.32s'", cmd.c_str());
  }
}

bool MqttUplink::connected() {
  return Cfg.mqtt_enabled && s_mqtt.connected();
}

String MqttUplink::status() {
  if (!Cfg.mqtt_enabled) return "disabled";
  if (strlen(Cfg.mqtt_host) == 0) return "no broker set";
  const char *pfx = Cfg.mqtt_tls ? "tls " : "";
  if (s_mqtt.connected())
    return String(pfx) + Cfg.mqtt_host + " ok:" + String(pubOk);
  return String(pfx) + "connecting " + Cfg.mqtt_host;
}

void MqttUplink::tick() {
  if (!Cfg.mqtt_enabled || strlen(Cfg.mqtt_host) == 0) return;

  LinkType link = Net.active();
  if (link == LinkType::NONE) {
    if (s_mqtt.connected()) s_mqtt.disconnect();
    s_boundLink = LinkType::NONE;
    return;
  }
  // active link changed (e.g. Ethernet came back while on LTE): rebind, and
  // retry at once instead of sitting out the 30 s backoff on the new link
  if (link != s_boundLink) {
    if (s_mqtt.connected()) s_mqtt.disconnect();
    _lastTryMs = 0;
  }

  if (s_mqtt.connected()) {
    s_mqtt.loop();  // keepalive
    return;
  }
  if (millis() - _lastTryMs < 30000) return;
  _lastTryMs = millis();

  Client *base = Net.mqttClient();
  if (!base) return;
  Client *c = base;
  if (Cfg.mqtt_tls) {
    // (re)wrap the transport of the current link in mbedTLS
    if (!s_tls || link != s_boundLink) {
      delete s_tls;
      delete s_tap;
      s_tap = new TapClient(base);
      s_tls = new SSLClient(s_tap);
      applyTlsConfig(s_tls);
    }
    s_tap->reset();
    c = s_tls;
  }
  s_mqtt.setClient(*c);
  s_mqtt.setServer(Cfg.mqtt_host, Cfg.mqtt_port);
  s_mqtt.setBufferSize(1536);
  s_mqtt.setKeepAlive(60);
  s_boundLink = link;

  String willTopic = baseTopic() + "/status";
  const char *user = strlen(Cfg.mqtt_user) ? Cfg.mqtt_user : nullptr;
  const char *pass = strlen(Cfg.mqtt_pass) ? Cfg.mqtt_pass : nullptr;
  s_mqtt.setCallback(onCmdMessage);
  // connect() blocks for the TCP dial plus the TLS handshake; start it with a
  // full watchdog budget
  esp_task_wdt_reset();
  uint32_t t0 = millis();
  bool ok = s_mqtt.connect(Cfg.station, user, pass, willTopic.c_str(),
                           0 /*willQos*/, true /*willRetain*/, "offline");
  esp_task_wdt_reset();
  log_i("MQTT: connect attempt took %lums", (unsigned long)(millis() - t0));
  if (s_tap) s_tap->report("MQTT transport");
  if (ok) {
    s_mqtt.publish(willTopic.c_str(), "online", true);
    String cmdTopic = baseTopic() + "/cmd";
    s_mqtt.subscribe(cmdTopic.c_str());
    log_i("MQTT: connected to %s:%u, listening on %s", Cfg.mqtt_host,
          Cfg.mqtt_port, cmdTopic.c_str());
    Store.logEvent("MQTT connected %s:%u", Cfg.mqtt_host, Cfg.mqtt_port);
  } else {
    log_w("MQTT: connect failed, rc=%d", s_mqtt.state());
  }
}

void MqttUplink::publishCycle(const char *ts) {
  if (!Cfg.mqtt_enabled) return;
  if (!s_mqtt.connected()) { pubFail++; return; }

  JsonDocument doc;
  doc["station"] = Cfg.station;
  doc["time"] = ts;
  doc["fw"] = FW_VERSION;
  const char *link = "none";
  switch (Net.active()) {
    case LinkType::ETH: link = "eth"; break;
    case LinkType::WIFI: link = "wifi"; break;
    case LinkType::LTE: link = "lte"; break;
    default: break;
  }
  doc["link"] = link;
  JsonObject data = doc["data"].to<JsonObject>();
  for (uint8_t i = 0; i < Cfg.sensor_count; i++) {
    const SensorCfg &s = Cfg.sensors[i];
    if (!s.enabled) continue;
    const SensorValue &v = Sensors.value(i);
    float val = v.windowAvg();
    if (isnan(val)) val = v.ok ? v.value : NAN;
    // duplicate parameter names would produce invalid JSON (repeated keys,
    // last one wins in most parsers) - disambiguate with a suffix
    String key = s.name;
    int suffix = 2;
    while (!data[key].isNull()) key = String(s.name) + "_" + String(suffix++);
    JsonObject o = data[key].to<JsonObject>();
    if (isnan(val)) o["v"] = nullptr;
    else o["v"] = serialized(String(val, (unsigned int)s.decimals));
    o["u"] = s.unit;
    o["st"] = Sensors.paramStatus(i);
  }

  char payload[1400];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  String topic = baseTopic() + "/data";
  if (s_mqtt.publish(topic.c_str(), (const uint8_t *)payload, len, false)) {
    pubOk++;
    log_i("MQTT: published %u bytes to %s", (unsigned)len, topic.c_str());
  } else {
    pubFail++;
    log_w("MQTT: publish failed");
  }
}
