#include "net_manager.h"
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <TinyGsmClient.h>
#include <Network.h>  // must precede WiFi.h so the LDF pulls the core lib in
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include "pins.h"
#include "app_config.h"
#include "storage.h"

StationNetwork Net;

static TinyGsm modem(Serial1);
static TinyGsmClient lteCtrl(modem, 0);
static TinyGsmClient lteData(modem, 1);
static EthernetClient ethCtrl;
static EthernetClient ethData;
static WiFiClient wifiCtrl;
static WiFiClient wifiData;
static TinyGsmClient lteMqtt(modem, 2);
static EthernetClient ethMqtt;
static WiFiClient wifiMqtt;

static byte s_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};

void StationNetwork::begin() {
  // W5500 hardware reset
  pinMode(PIN_ETH_RST, OUTPUT);
  digitalWrite(PIN_ETH_RST, LOW);
  delay(10);
  digitalWrite(PIN_ETH_RST, HIGH);
  delay(100);
  Ethernet.init(PIN_CS_ETH);
  // derive a unique MAC from the chip id
  uint64_t chipid = ESP.getEfuseMac();
  s_mac[3] = (chipid >> 16) & 0xFF;
  s_mac[4] = (chipid >> 8) & 0xFF;
  s_mac[5] = chipid & 0xFF;

  // Modem UART; power-on handled by the LTE state machine
  Serial1.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  pinMode(PIN_MODEM_RESET, OUTPUT);
  digitalWrite(PIN_MODEM_RESET, LOW);

  // Wi-Fi: optional link, between Ethernet and LTE in priority
  if (Cfg.wifi_enabled && strlen(Cfg.wifi_ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(Cfg.wifi_ssid, Cfg.wifi_pass);
    log_i("WiFi: connecting to '%s'", Cfg.wifi_ssid);
    // ADC2 is shared with the Wi-Fi radio on the ESP32-S3
    for (uint8_t i = 0; i < Cfg.sensor_count; i++) {
      const SensorCfg &s = Cfg.sensors[i];
      if (s.enabled && s.source >= SRC_AI2 && s.source <= SRC_VI1)
        log_w("WiFi: sensor '%s' uses ADC2 (AI2-AI4/VI1) - unreliable while "
              "Wi-Fi is on! Move it to AI1 or disable Wi-Fi.", s.name);
    }
  } else {
    WiFi.mode(WIFI_OFF);
  }

  // Vietnam timezone for local timestamps
  setenv("TZ", "<+07>-7", 1);
  tzset();
}

bool StationNetwork::wifiUp() const {
  return Cfg.wifi_enabled && WiFi.status() == WL_CONNECTED;
}

String StationNetwork::wifiStatus() const {
  if (!Cfg.wifi_enabled) return "disabled";
  if (WiFi.status() != WL_CONNECTED) return "connecting...";
  return WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + "dBm)";
}

void StationNetwork::wifiTick() {
  if (!Cfg.wifi_enabled) return;
  uint32_t now = millis();
  if (now - _wifiLastCheckMs < 2000) return;
  _wifiLastCheckMs = now;
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != _wifiWasUp) {
    _wifiWasUp = up;
    if (up) {
      log_i("WiFi: up, IP %s RSSI %d", WiFi.localIP().toString().c_str(),
            WiFi.RSSI());
      Store.logEvent("WiFi up %s rssi=%d", WiFi.localIP().toString().c_str(),
                     WiFi.RSSI());
    } else {
      log_w("WiFi: disconnected");
      Store.logEvent("WiFi down");
    }
  }
  ledWrite(PIN_LED_WIFI, up);
}

LinkType StationNetwork::active() const {
  if (_ethUp) return LinkType::ETH;
  if (wifiUp()) return LinkType::WIFI;
  if (_lteState == LTE_READY) return LinkType::LTE;
  return LinkType::NONE;
}

Client *StationNetwork::ctrlClient() {
  switch (active()) {
    case LinkType::ETH: return &ethCtrl;
    case LinkType::WIFI: return &wifiCtrl;
    case LinkType::LTE: return &lteCtrl;
    default: return nullptr;
  }
}

Client *StationNetwork::dataClient() {
  switch (active()) {
    case LinkType::ETH: return &ethData;
    case LinkType::WIFI: return &wifiData;
    case LinkType::LTE: return &lteData;
    default: return nullptr;
  }
}

Client *StationNetwork::mqttClient() {
  switch (active()) {
    case LinkType::ETH: return &ethMqtt;
    case LinkType::WIFI: return &wifiMqtt;
    case LinkType::LTE: return &lteMqtt;
    default: return nullptr;
  }
}

String StationNetwork::ethIp() const {
  if (!_ethUp) return "-";
  return Ethernet.localIP().toString();
}

const char *StationNetwork::lteStateName() const {
  switch (_lteState) {
    case LTE_OFF: return "off";
    case LTE_PWR_PULSE: return "power-on";
    case LTE_WAIT_AT: return "starting";
    case LTE_INIT: return "init";
    case LTE_WAIT_NET: return "searching";
    case LTE_ATTACH: return "attaching";
    case LTE_READY: return "connected";
    case LTE_BACKOFF: return "retry-wait";
    case LTE_NO_SIM: return "NO SIM";
  }
  return "?";
}

void StationNetwork::ethTick() {
  uint32_t now = millis();
  if (now - _ethLastCheckMs < 2000) return;
  _ethLastCheckMs = now;

  bool cable = (Ethernet.linkStatus() != LinkOFF);
  ledWrite(PIN_LED_ETH, _ethUp);

  if (!cable) {
    if (_ethUp) {
      log_i("ETH: link lost");
      Store.logEvent("ETH down (cable)");
    }
    _ethUp = false;
    _ethStarted = false;
    return;
  }
  if (_ethUp) {
    if (Cfg.eth_dhcp) Ethernet.maintain();
    return;
  }
  // cable present but not configured — (re)try, rate-limited
  if (_ethStarted && now - _ethLastTryMs < 15000) return;
  _ethLastTryMs = now;
  _ethStarted = true;
  if (Cfg.eth_dhcp) {
    log_i("ETH: DHCP...");
    if (Ethernet.begin(s_mac, 8000, 4000) == 1) {
      _ethUp = true;
      log_i("ETH: up, IP %s", Ethernet.localIP().toString().c_str());
      Store.logEvent("ETH up %s", Ethernet.localIP().toString().c_str());
    } else {
      log_w("ETH: DHCP failed");
    }
  } else {
    Ethernet.begin(s_mac, Cfg.ip, Cfg.dns, Cfg.gw, Cfg.mask);
    _ethUp = true;
    log_i("ETH: static IP %s", Ethernet.localIP().toString().c_str());
  }
}

void StationNetwork::lteTick() {
  uint32_t now = millis();
  auto enter = [&](LteState s) { _lteState = s; _lteStateMs = now; };

  switch (_lteState) {
    case LTE_OFF:
      // The modem keeps VBAT across MCU resets, so it may already be running:
      // pulsing PWRKEY then would power it OFF. Probe first.
      if (modem.testAT(300)) {
        log_i("LTE: modem already on");
        enter(LTE_INIT);
        break;
      }
      // A7682S PWRKEY: drive control line HIGH >= 1.5s (transistor pulls
      // PWRKEY low), then release.
      digitalWrite(PIN_MODEM_PWRKEY, HIGH);
      log_i("LTE: PWRKEY pulse");
      enter(LTE_PWR_PULSE);
      break;

    case LTE_PWR_PULSE:
      if (now - _lteStateMs >= 1500) {
        digitalWrite(PIN_MODEM_PWRKEY, LOW);
        enter(LTE_WAIT_AT);
      }
      break;

    case LTE_WAIT_AT:
      if (now - _lteStateMs < 3000) break;   // boot time before first probe
      if (modem.testAT(500)) {
        log_i("LTE: AT OK");
        enter(LTE_INIT);
      } else if (now - _lteStateMs > 30000) {
        log_w("LTE: no AT response");
        enter(LTE_BACKOFF);
      }
      break;

    case LTE_INIT:
      // Quick SIM probe first: modem.init() retries internally for ~20s when
      // no SIM is present, which would stall the whole superloop (UI included)
      if (modem.getSimStatus(2000) != 1 /*SIM_READY*/) {
        log_w("LTE: SIM not ready / not inserted");
        enter(LTE_NO_SIM);
      } else if (modem.init()) {
        log_i("LTE: modem %s", modem.getModemInfo().c_str());
        enter(LTE_WAIT_NET);
      } else {
        log_w("LTE: init failed");
        enter(LTE_BACKOFF);
      }
      break;

    case LTE_NO_SIM:
      // calm 30s poll; cheap AT+CPIN? only — no power-cycling, so a
      // hot-inserted SIM is picked up
      if (now - _lteStateMs >= 30000) {
        if (modem.getSimStatus(2000) == 1) {
          log_i("LTE: SIM detected");
          enter(LTE_INIT);
        } else {
          _lteStateMs = now;
        }
      }
      break;

    case LTE_WAIT_NET:
      if (now - _lteLastPollMs < 2000) break;
      _lteLastPollMs = now;
      if (modem.isNetworkConnected()) {
        log_i("LTE: registered");
        enter(LTE_ATTACH);
      } else if (now - _lteStateMs > 120000) {
        log_w("LTE: registration timeout");
        enter(LTE_BACKOFF);
      }
      break;

    case LTE_ATTACH:
      if (modem.gprsConnect(Cfg.apn, Cfg.apn_user, Cfg.apn_pass)) {
        _lteOperator = modem.getOperator();
        log_i("LTE: data up, op=%s", _lteOperator.c_str());
        Store.logEvent("LTE up op=%s", _lteOperator.c_str());
        enter(LTE_READY);
      } else {
        log_w("LTE: PDP attach failed");
        enter(LTE_BACKOFF);
      }
      break;

    case LTE_READY:
      if (now - _lteLastPollMs < 10000) break;
      _lteLastPollMs = now;
      _lteRssi = modem.getSignalQuality();
      if (!modem.isNetworkConnected() || !modem.isGprsConnected()) {
        log_w("LTE: connection lost");
        Store.logEvent("LTE down");
        enter(LTE_BACKOFF);
      }
      break;

    case LTE_BACKOFF:
      if (now - _lteStateMs >= _lteBackoffMs) {
        _lteBackoffMs = min<uint32_t>(_lteBackoffMs * 2, 300000);
        // hard reset; the module restarts by itself afterwards, so wait for
        // AT instead of going through the PWRKEY power-on path
        digitalWrite(PIN_MODEM_RESET, HIGH);
        delay(100);
        digitalWrite(PIN_MODEM_RESET, LOW);
        enter(LTE_WAIT_AT);
      }
      break;
  }
  if (_lteState == LTE_READY) _lteBackoffMs = 15000;
  ledWrite(PIN_LED_4G, _lteState == LTE_READY);
}

// Works over any Arduino UDP implementation: EthernetUDP (W5500 hardware
// stack) or WiFiUDP (lwIP).
static bool ntpQuery(UDP &udp) {
  static const char *NTP_HOST = "pool.ntp.org";
  udp.begin(2390);
  uint8_t pkt[48] = {0};
  pkt[0] = 0b11100011;  // LI=3, VN=4, mode=3 (client)
  if (udp.beginPacket(NTP_HOST, 123) != 1) { udp.stop(); return false; }
  udp.write(pkt, 48);
  udp.endPacket();
  uint32_t start = millis();
  while (millis() - start < 3000) {
    if (udp.parsePacket() >= 48) {
      udp.read(pkt, 48);
      udp.stop();
      uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                      ((uint32_t)pkt[42] << 8) | pkt[43];
      if (secs < 2208988800UL) return false;
      time_t epoch = secs - 2208988800UL;  // NTP -> Unix
      struct timeval tv;
      tv.tv_sec = epoch;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      return true;
    }
    delay(20);
  }
  udp.stop();
  return false;
}

bool StationNetwork::syncNtpEth() {
  EthernetUDP udp;
  return ntpQuery(udp);
}

// Fallback time source for networks that block NTP/UDP: the Date header of
// an HTTP response (TCP 80 is almost never filtered).
static bool httpDateQuery(Client &c) {
  if (!c.connect("google.com", 80)) return false;
  c.print("HEAD / HTTP/1.1\r\nHost: google.com\r\nConnection: close\r\n\r\n");
  String line = "", dateLine = "";
  uint32_t start = millis();
  while (millis() - start < 8000) {
    if (!c.available()) {
      if (!c.connected()) break;
      delay(5);
      continue;
    }
    char ch = (char)c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { line += ch; continue; }
    if (line.startsWith("Date:") || line.startsWith("date:")) {
      dateLine = line;
      break;
    }
    if (line.length() == 0) break;  // end of headers
    line = "";
  }
  c.stop();
  if (dateLine.length() == 0) return false;
  // "Date: Sat, 11 Jul 2026 17:03:22 GMT"
  struct tm tmv = {};
  if (!strptime(dateLine.c_str() + 5, " %a, %d %b %Y %H:%M:%S", &tmv))
    return false;
  if (tmv.tm_year + 1900 < 2024) return false;
  // fields are GMT but mktime assumes our +07 zone: add the offset back
  time_t epoch = mktime(&tmv) + 7 * 3600;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

bool StationNetwork::syncTimeLte() {
  int year, month, day, hour, minute, second;
  float tz = 0;
  if (!modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &tz))
    return false;
  if (year < 2024) return false;
  struct tm tmv = {};
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_sec = second;
  // modem reports local time at offset tz (hours); convert to UTC epoch
  time_t local = mktime(&tmv);            // interpreted in our TZ (+7)
  time_t epoch = local;                   // if tz==7 this is already right
  epoch += (time_t)((7.0f - tz) * 3600);  // correct if network tz differs
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

void StationNetwork::timeTick() {
  uint32_t now = millis();
  bool due = !_timeSynced || (now - _lastTimeSyncMs > 24UL * 3600UL * 1000UL);
  if (!due || now - _lastTimeTryMs < 20000) return;
  _lastTimeTryMs = now;
  bool ok = false;
  if (_ethUp) {
    ok = syncNtpEth();
    if (!ok) {
      log_w("time: NTP over Ethernet failed, trying HTTP date");
      ok = httpDateQuery(ethCtrl);
    }
  } else if (wifiUp()) {
    WiFiUDP udp;
    ok = ntpQuery(udp);
    if (!ok) {
      log_w("time: NTP over WiFi failed, trying HTTP date");
      ok = httpDateQuery(wifiCtrl);
    }
  } else if (_lteState == LTE_READY) {
    ok = syncTimeLte();
  }
  if (ok) {
    _timeSynced = true;
    _lastTimeSyncMs = now;
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    log_i("time synced: %04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900,
          tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
}

void StationNetwork::tick() {
  ethTick();
  wifiTick();
  lteTick();
  timeTick();
}
