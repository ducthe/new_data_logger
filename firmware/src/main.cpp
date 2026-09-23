// Wynd Datalogger — Automatic Wastewater / Water Supply Monitoring Station
//
// RS485 Modbus RTU + 4x 4-20mA sensor acquisition, Ethernet-first / LTE-
// fallback FTP transmission in the MONRE continuous-monitoring file format,
// SD backlog buffering, relay control (sampling pump / discharge valve) and
// a local touchscreen UI.
//
// Everything that touches the shared SPI bus (TFT, touch, SD, W5500) runs
// from this single loop on purpose: no cross-task bus contention.
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include "pins.h"
#include "app_config.h"
#include "sensors.h"
#include "storage.h"
#include "net_manager.h"
#include "monre.h"
#include "mqtt_uplink.h"
#include "control.h"
#include "ota.h"
#include "ui.h"

static void safeGpioInit() {
  // relays de-energized before anything else can glitch them
  pinMode(PIN_RELAY1, OUTPUT);
  digitalWrite(PIN_RELAY1, LOW);
  pinMode(PIN_RELAY2, OUTPUT);
  digitalWrite(PIN_RELAY2, LOW);
  // all SPI chip-selects idle high before the first transfer
  const int cs[] = {PIN_CS_ETH, PIN_CS_SD, PIN_CS_TFT, PIN_CS_TOUCH};
  for (int p : cs) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);
  const int leds[] = {PIN_LED_WIFI, PIN_LED_4G, PIN_LED_ETH};
  for (int p : leds) {
    pinMode(p, OUTPUT);
    ledWrite(p, false);  // LEDs are active-low: off = HIGH
  }
}

// Diagnostic: read XPT2046 channels directly, bypassing LovyanGFX.
// X/Y are position plates, Z1/Z2 are the pressure dividers.
#define XPT_DIAG 0
#if XPT_DIAG
static uint16_t xptRead(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS_TOUCH, LOW);
  SPI.transfer(cmd);
  SPI.transfer16(0);                    // discard first conversion (settling)
  SPI.transfer(cmd);
  uint16_t v = SPI.transfer16(0) >> 3;  // 12-bit result
  digitalWrite(PIN_CS_TOUCH, HIGH);
  SPI.endTransaction();
  return v;
}

static void xptDiag(uint32_t ms) {
  Serial.println("[diag] XPT2046 direct read - PRESS AND HOLD SCREEN CENTER");
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    uint16_t x = xptRead(0xD0), y = xptRead(0x90);
    uint16_t z1 = xptRead(0xB0), z2 = xptRead(0xC0);
    Serial.printf("[diag] x=%4u y=%4u z1=%4u z2=%4u\n", x, y, z1, z2);
    delay(400);
  }
}
#endif

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-wdt";
    case ESP_RST_TASK_WDT: return "TASK-WDT (main loop hung)";
    case ESP_RST_WDT: return "other-wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

void setup() {
  safeGpioInit();
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n=== Wynd Station v%s ===\n", FW_VERSION);
  Serial.printf("[init] reset reason: %s\n", resetReasonName(esp_reset_reason()));

  // Task watchdog on the main loop: a hard hang (SPI deadlock, modem UART
  // flood, driver bug) reboots the station instead of freezing it forever.
  // 120s clears every legitimate long block (LTE attach <=75s, FTP steps
  // <=15s each and fed inside their wait loops).
  {
    esp_task_wdt_config_t wdt_cfg = {};
    wdt_cfg.timeout_ms = 120000;
    wdt_cfg.idle_core_mask = 0;
    wdt_cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK)
      esp_task_wdt_init(&wdt_cfg);
    esp_task_wdt_add(NULL);  // subscribe loopTask
    Serial.println("[init] task watchdog: 120s on main loop");
  }

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

#if XPT_DIAG
  xptDiag(20000);
#endif

  Cfg.setDefaults();

  UI.begin();  // init display first so the bus is configured by LGFX
  Serial.printf("[init] display: %s\n", UI.displayOk ? "OK" : "FAIL");

  bool sd = Store.begin();
  Serial.printf("[init] sd card: %s\n", sd ? "OK" : "NOT FOUND");
  if (sd) {
    Ota.checkSdAtBoot();  // flashes + reboots if /firmware.bin is present
    bool cfgLoaded = Cfg.loadFromSD();
    Serial.printf("[init] config.json: %s\n",
                  cfgLoaded ? "loaded" : "defaults written");
    Store.logEvent("boot fw=%s reset=%s", FW_VERSION,
                   resetReasonName(esp_reset_reason()));
  }

  Sensors.begin();
  Serial.printf("[init] rs485: %lu baud, %u sensors\n",
                (unsigned long)Cfg.rs485_baud, Cfg.sensor_count);

  Control.begin();
  Net.begin();
  Serial.printf("[init] station '%s' -> ftp://%s:%u%s every %us\n",
                Cfg.station, Cfg.ftp_host, Cfg.ftp_port, Cfg.ftp_dir,
                Cfg.send_interval_s);
  Serial.println("[init] done");
}

// Serial console: bench/fallback control when the touchscreen is unavailable.
static void consoleTick() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case '1': case '2': case '3': case '4': case '5':
        UI.setScreen(c - '1');
        Serial.printf("> screen %c\n", c);
        break;
      case 's':
        Serial.println("> manual send");
        Uplink.sendNow();
        Serial.printf("> result: %s\n", Uplink.lastResult.c_str());
        break;
      case 'P': Control.setRelay(0, true);  Serial.println("> pump ON (manual)"); break;
      case 'p': Control.setRelay(0, false); Serial.println("> pump OFF (manual)"); break;
      case 'V': Control.setRelay(1, true);  Serial.println("> valve ON (manual)"); break;
      case 'v': Control.setRelay(1, false); Serial.println("> valve OFF (manual)"); break;
      case 'c':
        Sensors.calibrationMode = !Sensors.calibrationMode;
        Serial.printf("> calibration mode %s\n", Sensors.calibrationMode ? "ON" : "off");
        Store.logEvent("calibration mode %s (console)",
                       Sensors.calibrationMode ? "ON" : "off");
        break;
      case 'e':
        Serial.println("> event log tail:");
        Serial.println(Store.eventTail());
        break;
      case 'i': {
        Serial.print("station="); Serial.flush();
        Serial.print(Cfg.station); Serial.flush();
        Serial.printf(" eth=%s", Net.ethUp() ? Net.ethIp().c_str() : "down");
        Serial.printf(" wifi=%s", Net.wifiStatus().c_str());
        Serial.flush();
        Serial.printf(" lte=%s time=%s", Net.lteStateName(),
                      Net.timeSynced() ? "synced" : "no");
        Serial.flush();
        Serial.printf(" mqtt=%s", Mqtt.status().c_str()); Serial.flush();
        Serial.printf(" buffered=%u", Store.bufferedCount()); Serial.flush();
        Serial.printf(" ok=%lu fail=%lu\n", (unsigned long)Uplink.okCount,
                      (unsigned long)Uplink.failCount);
        Serial.printf("sd: card=%lluMB fs=%lluMB used=%lluMB\n",
                      (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)),
                      (unsigned long long)Store.totalMB(),
                      (unsigned long long)Store.usedMB());
        for (uint8_t k = 0; k < Cfg.sensor_count; k++) {
          if (!Cfg.sensors[k].enabled) continue;
          const SensorValue &v = Sensors.value(k);
          Serial.printf("  %-6s = %s%.3f (%s)", Cfg.sensors[k].name,
                        v.ok ? "" : "ERR ", v.value, Sensors.paramStatus(k));
          if (!isnan(v.rawMa)) Serial.printf("  [%.2f mA]", v.rawMa);
          Serial.println();
        }
        break;
      }
      case 'w':
        Cfg.setDefaults();
        Serial.printf("> config.json reset to defaults: %s\n",
                      Cfg.saveToSD() ? "OK" : "SD WRITE FAILED");
        break;
      case 'F': {  // set FTP target: F <host> <port> <user> <pass> [dir]
        Serial.setTimeout(5000);
        String line = Serial.readStringUntil('\n');
        Serial.setTimeout(1000);
        line.trim();
        char host[64], user[32], pass[32], dir[64] = "/";
        int port = 21;
        int n = sscanf(line.c_str(), "%63s %d %31s %31s %63s", host, &port,
                       user, pass, dir);
        if (n < 4) {
          Serial.println("> usage: F <host> <port> <user> <pass> [dir]");
          break;
        }
        strlcpy(Cfg.ftp_host, host, sizeof(Cfg.ftp_host));
        Cfg.ftp_port = (uint16_t)port;
        strlcpy(Cfg.ftp_user, user, sizeof(Cfg.ftp_user));
        strlcpy(Cfg.ftp_pass, pass, sizeof(Cfg.ftp_pass));
        strlcpy(Cfg.ftp_dir, dir, sizeof(Cfg.ftp_dir));
        Serial.printf("> ftp -> %s:%u user=%s dir=%s  saved:%s\n", Cfg.ftp_host,
                      Cfg.ftp_port, Cfg.ftp_user, Cfg.ftp_dir,
                      Cfg.saveToSD() ? "OK" : "SD-FAIL");
        break;
      }
      case 't':
        UI.touchTest(60000);
        break;
      case 'X': {  // watchdog test: requires "X!" so stray chars can't trigger
        Serial.setTimeout(1000);
        String confirm = Serial.readStringUntil('\n');
        Serial.setTimeout(1000);
        if (confirm != "!") {
          Serial.println("> watchdog test needs 'X!' to confirm");
          break;
        }
        Serial.println("> simulating main-loop hang; watchdog reboot in ~120s");
        while (true) delay(100);
        break;
      }
      case 'T':
        UI.touchCalibrate();
        break;
      case 'U':
        Serial.println("> OTA from SD (/firmware.bin)...");
        if (!Ota.updateFromSD()) Serial.println("> no update applied");
        break;
      case 'M': {  // M <host> <port> [user pass [topic]] | M off
        Serial.setTimeout(5000);
        String line = Serial.readStringUntil('\n');
        Serial.setTimeout(1000);
        line.trim();
        if (line == "off") {
          Cfg.mqtt_enabled = false;
        } else {
          char host[64], user[32] = "", pass[32] = "", topic[48] = "";
          int port = 1883;
          int n = sscanf(line.c_str(), "%63s %d %31s %31s %47s", host, &port,
                         user, pass, topic);
          // Validate: a mistyped or half-received line used to be accepted
          // silently and could land junk in mqtt_topic, which then published
          // to a topic nobody is subscribed to.
          if (n < 2 || port < 1 || port > 65535 || strlen(host) < 4 ||
              strspn(host, "0123456789") == strlen(host)) {
            Serial.println("> usage: M <host> <port> [user pass [topic]] | M off");
            Serial.printf("> (parsed %d field(s): host='%s' port=%d)\n", n,
                          host, port);
            break;
          }
          if (n >= 5 && strchr(topic, '/') == nullptr) {
            Serial.printf("> topic '%s' has no '/', ignoring it and using "
                          "the default wynd/<station>\n", topic);
            topic[0] = '\0';
          }
          strlcpy(Cfg.mqtt_host, host, sizeof(Cfg.mqtt_host));
          Cfg.mqtt_port = (uint16_t)port;
          strlcpy(Cfg.mqtt_user, user, sizeof(Cfg.mqtt_user));
          strlcpy(Cfg.mqtt_pass, pass, sizeof(Cfg.mqtt_pass));
          strlcpy(Cfg.mqtt_topic, topic, sizeof(Cfg.mqtt_topic));
          Cfg.mqtt_enabled = true;
          Cfg.mqtt_tls = (port == 8883);  // 8883 = TLS, any other port = plain
        }
        Serial.printf("> mqtt %s %s:%u tls:%s saved:%s\n",
                      Cfg.mqtt_enabled ? "ON" : "OFF", Cfg.mqtt_host,
                      Cfg.mqtt_port, Cfg.mqtt_tls ? "yes" : "no",
                      Cfg.saveToSD() ? "OK" : "SD-FAIL");
        break;
      }
      case 'O': {  // O <firmware-url> [md5] — direct download + flash
        Serial.setTimeout(5000);
        String line = Serial.readStringUntil('\n');
        Serial.setTimeout(1000);
        line.trim();
        char url[128], md5[40] = "";
        int n = sscanf(line.c_str(), "%127s %39s", url, md5);
        if (n < 1) {
          Serial.println("> usage: O <http://host/firmware.bin> [md5]");
          break;
        }
        Serial.printf("> OTA from %s ...\n", url);
        String msg;
        Ota.updateFromUrl(url, md5, msg);  // reboots on success
        Serial.printf("> %s\n", msg.c_str());
        break;
      }
      case 'h': case '?':
        Serial.println("keys: 1-5 screen | s send | P/p pump | V/v valve | c calib | e events | i info | t touch-test | T touch-cal | w reset-config | F ftp-cfg | M mqtt-cfg | U ota-sd | O ota-remote");
        break;
      default: break;
    }
  }
}

void loop() {
  esp_task_wdt_reset();
  Sensors.tick();
  Net.tick();
  Uplink.tick();
  Control.tick();
  UI.tick();
  Mqtt.tick();
  Store.maintain();
  consoleTick();
  delay(2);
}
