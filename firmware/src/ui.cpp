#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "ui.h"
#include "pins.h"
#include "app_config.h"
#include "sensors.h"
#include "storage.h"
#include "net_manager.h"
#include "monre.h"
#include "mqtt_uplink.h"
#include "control.h"

// ---------------------------------------------------------------- display --
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;   // 3.2" module; swap to Panel_ST7789 if needed
  lgfx::Bus_SPI _bus;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_SPI_SCK;
      cfg.pin_mosi = PIN_SPI_MOSI;
      cfg.pin_miso = PIN_SPI_MISO;
      cfg.pin_dc = PIN_TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_CS_TFT;
      cfg.pin_rst = PIN_TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.bus_shared = true;  // SD card + W5500 share this bus
      _panel.config(cfg);
    }
    {
      auto cfg = _touch.config();
      // raw X endpoints swapped: this panel's X gradient runs opposite to the
      // screen-vertical after rotation (verified on hardware 2026-07-11).
      // Real 4-point calibration ('T' console key / System tab) overrides.
      cfg.x_min = 3800;
      cfg.x_max = 300;
      cfg.y_min = 200;
      cfg.y_max = 3700;
      cfg.pin_int = -1;      // T_IRQ not wired: poll
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = PIN_SPI_SCK;
      cfg.pin_mosi = PIN_SPI_MOSI;
      cfg.pin_miso = PIN_SPI_MISO;
      cfg.pin_cs = PIN_CS_TOUCH;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
UIManager UI;

// layout
static const int16_t W = 320, H = 240;
static const int16_t HDR_H = 24;
static const int16_t TAB_H = 32;
static const int16_t CT_Y = HDR_H;            // content top
static const int16_t CT_H = H - HDR_H - TAB_H;

// palette
static const uint16_t C_BG = 0x0000;
static const uint16_t C_HDR = 0x1926;         // dark blue-gray
static const uint16_t C_TAB = 0x18E3;
static const uint16_t C_TAB_SEL = 0x0339;
static const uint16_t C_TILE = 0x10A2;
static const uint16_t C_TEXT = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_ACC = 0x07FF;         // cyan
static const uint16_t C_OK = 0x07E0;
static const uint16_t C_ERR = 0xF800;
static const uint16_t C_WARN = 0xFD20;

static const char *TAB_NAMES[5] = {"Home", "Sensors", "Network", "Control", "System"};

// buttons
struct Btn {
  int16_t x, y, w, h;
  char label[14];
  uint8_t id;
  uint16_t color;
};
static Btn s_btns[10];
static uint8_t s_btnCount = 0;

enum BtnId : uint8_t {
  BTN_SEND_NOW = 1,
  BTN_R1_ON, BTN_R1_OFF, BTN_R1_MODE,
  BTN_R2_ON, BTN_R2_OFF, BTN_R2_MODE,
  BTN_CAL_MODE, BTN_TOUCH_CAL, BTN_REBOOT,
  BTN_SENS_UP, BTN_SENS_DOWN,
};

void UIManager::clearButtons() { s_btnCount = 0; }

void UIManager::addButton(int16_t x, int16_t y, int16_t w, int16_t h,
                          const char *label, uint8_t id, uint16_t color) {
  if (s_btnCount >= 10) return;
  Btn &b = s_btns[s_btnCount++];
  b.x = x; b.y = y; b.w = w; b.h = h; b.id = id; b.color = color;
  strlcpy(b.label, label, sizeof(b.label));
  tft.fillRoundRect(x, y, w, h, 4, color);
  tft.drawRoundRect(x, y, w, h, 4, C_DIM);
  tft.setTextDatum(lgfx::middle_center);
  tft.setTextColor(C_TEXT, color);
  tft.setFont(&fonts::Font2);
  tft.setTextPadding(0);
  tft.drawString(label, x + w / 2, y + h / 2);
}

// ----------------------------------------------------------------- chrome --
void UIManager::drawChrome() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, HDR_H, C_HDR);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::middle_left);
  tft.setTextColor(C_ACC, C_HDR);
  tft.setTextPadding(0);
  tft.drawString(Cfg.station, 4, HDR_H / 2);

  // tab bar
  int16_t tw = W / 5;
  for (int i = 0; i < 5; i++) {
    bool sel = (i == (int)_screen);
    tft.fillRect(i * tw, H - TAB_H, tw, TAB_H, sel ? C_TAB_SEL : C_TAB);
    tft.drawRect(i * tw, H - TAB_H, tw, TAB_H, C_BG);
    tft.setTextDatum(lgfx::middle_center);
    tft.setTextColor(sel ? C_TEXT : C_DIM, sel ? C_TAB_SEL : C_TAB);
    tft.drawString(TAB_NAMES[i], i * tw + tw / 2, H - TAB_H / 2);
  }
}

void UIManager::drawHeaderDynamic() {
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::middle_right);

  // clock
  char buf[24];
  if (Net.timeSynced()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d:%02d", tmv.tm_mday,
             tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "--:--:--");
  }
  tft.setTextColor(C_TEXT, C_HDR);
  tft.setTextPadding(110);
  tft.drawString(buf, W - 4, HDR_H / 2);

  // link + SD badges
  tft.setTextDatum(lgfx::middle_center);
  tft.setTextPadding(0);
  tft.setTextColor(Net.ethUp() ? C_OK : C_DIM, C_HDR);
  tft.drawString("ETH", 112, HDR_H / 2);
  tft.setTextColor(Net.wifiUp() ? C_OK : (Cfg.wifi_enabled ? C_WARN : C_DIM),
                   C_HDR);
  tft.drawString("WiFi", 146, HDR_H / 2);
  tft.setTextColor(Net.lteUp() ? C_OK : C_DIM, C_HDR);
  tft.drawString("4G", 175, HDR_H / 2);
  tft.setTextColor(Store.ready() ? C_OK : C_ERR, C_HDR);
  tft.drawString("SD", 198, HDR_H / 2);
}

// ------------------------------------------------------------------ home --
void UIManager::drawHome(bool full) {
  const int cols = 4, rows = 2;
  const int16_t gw = W / cols, gh = CT_H / rows;
  uint8_t shown = 0;
  for (uint8_t i = 0; i < Cfg.sensor_count && shown < cols * rows; i++) {
    const SensorCfg &s = Cfg.sensors[i];
    if (!s.enabled) continue;
    int16_t x = (shown % cols) * gw, y = CT_Y + (shown / cols) * gh;
    shown++;
    const SensorValue &v = Sensors.value(i);
    uint16_t border = v.ok ? C_TILE : C_ERR;
    if (Sensors.calibrationMode) border = C_WARN;
    if (full) {
      tft.fillRoundRect(x + 2, y + 2, gw - 4, gh - 4, 5, C_TILE);
    }
    tft.drawRoundRect(x + 2, y + 2, gw - 4, gh - 4, 5, border);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(lgfx::top_center);
    tft.setTextColor(C_DIM, C_TILE);
    tft.setTextPadding(gw - 12);
    tft.drawString(s.name, x + gw / 2, y + 7);
    char num[16];
    if (v.ok && !isnan(v.value)) dtostrf(v.value, 0, s.decimals, num);
    else strcpy(num, "---");
    tft.setFont(&fonts::Font4);
    tft.setTextDatum(lgfx::middle_center);
    tft.setTextColor(v.ok ? C_TEXT : C_ERR, C_TILE);
    tft.setTextPadding(gw - 10);
    tft.drawString(num, x + gw / 2, y + gh / 2 + 4);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(lgfx::bottom_center);
    tft.setTextColor(C_DIM, C_TILE);
    tft.setTextPadding(gw - 12);
    tft.drawString(s.unit, x + gw / 2, y + gh - 6);
  }
}

// --------------------------------------------------------------- sensors --
void UIManager::drawSensors(bool full) {
  const uint8_t ROWS = 7;  // visible rows per page

  // enabled sensors only, in config order
  uint8_t idx[MAX_SENSORS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < Cfg.sensor_count; i++)
    if (Cfg.sensors[i].enabled) idx[n++] = i;
  uint8_t maxTop = n > ROWS ? n - ROWS : 0;
  if (_sensTop > maxTop) _sensTop = maxTop;

  tft.setFont(&fonts::Font2);
  int16_t y = CT_Y + 2;
  if (full) {
    tft.setTextDatum(lgfx::top_left);
    tft.setTextColor(C_ACC, C_BG);
    tft.setTextPadding(0);
    tft.drawString("Param", 6, y);
    tft.drawString("Source", 78, y);
    tft.drawString("Value", 180, y);
    tft.drawString("St", 260, y);
    tft.drawFastHLine(0, y + 16, W, C_DIM);
    if (n > ROWS) {
      addButton(W - 32, CT_Y + 20, 28, 62, "^", BTN_SENS_UP, C_TAB);
      addButton(W - 32, CT_Y + 88, 28, 62, "v", BTN_SENS_DOWN, C_TAB);
    }
  }
  y += 20;
  for (uint8_t r = 0; r < ROWS; r++) {
    uint8_t k = _sensTop + r;
    if (k >= n) break;
    uint8_t i = idx[k];
    const SensorCfg &s = Cfg.sensors[i];
    const SensorValue &v = Sensors.value(i);
    tft.setTextDatum(lgfx::top_left);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextPadding(68);
    tft.drawString(s.name, 6, y);
    char src[24];
    if (s.source == SRC_MODBUS)
      snprintf(src, sizeof(src), "MB a%u r%u", s.slave, s.reg);
    else if (s.source == SRC_VI1)
      snprintf(src, sizeof(src), "VI1");
    else if (!isnan(v.rawMa))
      snprintf(src, sizeof(src), "AI%u %.1fmA", s.source, v.rawMa);
    else
      snprintf(src, sizeof(src), "AI%u --", s.source);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextPadding(98);
    tft.drawString(src, 78, y);
    char num[20];
    if (v.ok && !isnan(v.value)) {
      dtostrf(v.value, 0, s.decimals, num);
      strlcat(num, " ", sizeof(num));
      strlcat(num, s.unit, sizeof(num));
    } else {
      strcpy(num, "---");
    }
    tft.setTextColor(v.ok ? C_TEXT : C_ERR, C_BG);
    tft.setTextPadding(76);
    tft.drawString(num, 180, y);
    tft.setTextColor(v.ok ? C_OK : C_ERR, C_BG);
    tft.setTextPadding(24);
    tft.drawString(v.ok ? "OK" : "ER", 260, y);
    y += 19;
  }
  // scroll position indicator
  if (n > ROWS) {
    char pos[16];
    snprintf(pos, sizeof(pos), "%u-%u/%u", _sensTop + 1,
             min<uint8_t>(_sensTop + ROWS, n), n);
    tft.setTextDatum(lgfx::top_center);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextPadding(52);
    tft.drawString(pos, W - 18, CT_Y + 156);
  }
  // bus stats + DI1
  char foot[64];
  snprintf(foot, sizeof(foot), "RS485 ok:%lu err:%lu   DI1:%s",
           (unsigned long)Sensors.bus.okCount,
           (unsigned long)Sensors.bus.errCount,
           Sensors.di1Active() ? "ACTIVE" : "idle");
  tft.setTextDatum(lgfx::bottom_left);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextPadding(W - 60);
  tft.drawString(foot, 6, H - TAB_H - 2);
}

// --------------------------------------------------------------- network --
void UIManager::drawNetwork(bool full) {
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::top_left);
  int16_t y = CT_Y + 3;
  const int16_t lh = 18;
  auto line = [&](const char *k, const String &val, uint16_t color) {
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextPadding(0);
    tft.drawString(k, 6, y);
    tft.setTextColor(color, C_BG);
    tft.setTextPadding(226);
    tft.drawString(val, 90, y);
    y += lh;
  };
  line("Ethernet", Net.ethUp() ? ("UP  " + Net.ethIp()) : "DOWN",
       Net.ethUp() ? C_OK : C_ERR);
  line("WiFi", Net.wifiStatus(),
       Net.wifiUp() ? C_OK : (Cfg.wifi_enabled ? C_WARN : C_DIM));
  char lte[48];
  snprintf(lte, sizeof(lte), "%s %s CSQ:%d", Net.lteStateName(),
           Net.lteOperator().c_str(), Net.lteRssi());
  line("LTE 4G", lte, Net.lteUp() ? C_OK : C_WARN);
  line("MQTT", Mqtt.status(),
       Mqtt.connected() ? C_OK : (Cfg.mqtt_enabled ? C_WARN : C_DIM));
  line("Time", Net.timeSynced() ? "synced" : "NOT SYNCED",
       Net.timeSynced() ? C_OK : C_WARN);
  char ftp[64];
  snprintf(ftp, sizeof(ftp), "%s:%u%s", Cfg.ftp_host, Cfg.ftp_port, Cfg.ftp_dir);
  line("FTP", ftp, C_TEXT);
  char stat[64];
  snprintf(stat, sizeof(stat), "%us ok:%lu fail:%lu buf:%u next:%lus",
           Cfg.send_interval_s, (unsigned long)Uplink.okCount,
           (unsigned long)Uplink.failCount, Store.bufferedCount(),
           (unsigned long)(Uplink.nextSendInMs() / 1000));
  line("Send", stat, C_TEXT);
  line("Last", Uplink.lastResult.substring(0, 30),
       Uplink.lastResult.startsWith("OK") ? C_OK : C_WARN);

  if (full) {
    addButton(W - 110, H - TAB_H - 36, 104, 30, "SEND NOW", BTN_SEND_NOW, C_TAB_SEL);
  }
}

// --------------------------------------------------------------- control --
void UIManager::drawControl(bool full) {
  const int16_t pw = W / 2 - 8;
  const char *titles[2] = {"PUMP (DO1)", "VALVE (DO2)"};
  for (int r = 0; r < 2; r++) {
    int16_t x = 4 + r * (pw + 8);
    int16_t y = CT_Y + 4;
    uint8_t mode = r == 0 ? Cfg.r1_mode : Cfg.r2_mode;
    bool on = Control.relayState(r);
    if (full) tft.fillRoundRect(x, y, pw, 118, 6, C_TILE);
    tft.drawRoundRect(x, y, pw, 118, 6, C_DIM);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(lgfx::top_center);
    tft.setTextColor(C_ACC, C_TILE);
    tft.setTextPadding(0);
    tft.drawString(titles[r], x + pw / 2, y + 6);
    tft.setFont(&fonts::Font4);
    tft.setTextColor(on ? C_OK : C_DIM, C_TILE);
    tft.setTextPadding(pw - 20);
    tft.drawString(on ? "ON" : "OFF", x + pw / 2, y + 28);
    tft.setFont(&fonts::Font2);
    tft.setTextColor(C_WARN, C_TILE);
    tft.setTextPadding(pw - 20);
    tft.drawString(mode == RELAY_AUTO ? "AUTO" : "MANUAL", x + pw / 2, y + 60);
    if (full) {
      uint8_t idOn = r == 0 ? BTN_R1_ON : BTN_R2_ON;
      uint8_t idOff = r == 0 ? BTN_R1_OFF : BTN_R2_OFF;
      uint8_t idMode = r == 0 ? BTN_R1_MODE : BTN_R2_MODE;
      addButton(x + 6, y + 82, (pw - 18) / 2, 28, "ON", idOn,
                mode == RELAY_MANUAL ? 0x0480 : C_TAB);
      addButton(x + 12 + (pw - 18) / 2, y + 82, (pw - 18) / 2, 28, "OFF", idOff,
                mode == RELAY_MANUAL ? 0x8800 : C_TAB);
      addButton(x + pw - 62, y + 4, 56, 20, "MODE", idMode, C_TAB_SEL);
    }
  }
  // alarm status line
  char buf[80];
  snprintf(buf, sizeof(buf), "Alarm %s: %.2f  hi:%.2f lo:%.2f  %s",
           Cfg.alarm_param, Control.alarmValue(), Cfg.alarm_high, Cfg.alarm_low,
           Control.alarmActive() ? "ACTIVE" : "normal");
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::top_left);
  tft.setTextColor(Control.alarmActive() ? C_ERR : C_DIM, C_BG);
  tft.setTextPadding(W - 8);
  tft.drawString(buf, 6, CT_Y + 130);
  char pump[64];
  snprintf(pump, sizeof(pump), "Pump AUTO: %us ON every %us", Cfg.pump_on_s,
           Cfg.pump_period_s);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextPadding(W - 8);
  tft.drawString(pump, 6, CT_Y + 150);
}

// ---------------------------------------------------------------- system --
void UIManager::drawSystem(bool full) {
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::top_left);
  int16_t y = CT_Y + 4;
  const int16_t lh = 19;
  auto line = [&](const char *k, const String &val) {
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextPadding(0);
    tft.drawString(k, 6, y);
    tft.setTextColor(C_TEXT, C_BG);
    tft.setTextPadding(220);
    tft.drawString(val, 96, y);
    y += lh;
  };
  line("Station", Cfg.station);
  line("Firmware", "Wynd Station v" FW_VERSION);
  char up[32];
  uint32_t s = millis() / 1000;
  snprintf(up, sizeof(up), "%lud %02lu:%02lu:%02lu", (unsigned long)(s / 86400),
           (unsigned long)(s / 3600 % 24), (unsigned long)(s / 60 % 60),
           (unsigned long)(s % 60));
  line("Uptime", up);
  char sd[48];
  if (Store.ready())
    snprintf(sd, sizeof(sd), "%llu / %llu MB, %u pending",
             (unsigned long long)Store.usedMB(),
             (unsigned long long)Store.totalMB(), Store.bufferedCount());
  else
    snprintf(sd, sizeof(sd), "NOT DETECTED");
  line("SD card", sd);
  line("Heap", String(ESP.getFreeHeap() / 1024) + " kB");
  line("Calib mode", Sensors.calibrationMode ? "ON (data=01)" : "off");

  if (full) {
    int16_t by = H - TAB_H - 36;
    addButton(6, by, 96, 30, Sensors.calibrationMode ? "CAL OFF" : "CAL ON",
              BTN_CAL_MODE, C_WARN);
    addButton(110, by, 96, 30, "TOUCH CAL", BTN_TOUCH_CAL, C_TAB_SEL);
    addButton(214, by, 96, 30, "REBOOT", BTN_REBOOT, 0x8800);
  }
}

// -------------------------------------------------------------- top-level --
void UIManager::drawScreen(bool full) {
  if (full) {
    clearButtons();
    drawChrome();
  }
  drawHeaderDynamic();
  switch (_screen) {
    case SCR_HOME: drawHome(full); break;
    case SCR_SENSORS: drawSensors(full); break;
    case SCR_NETWORK: drawNetwork(full); break;
    case SCR_CONTROL: drawControl(full); break;
    case SCR_SYSTEM: drawSystem(full); break;
  }
}

void UIManager::onButton(uint8_t id) {
  switch (id) {
    case BTN_SEND_NOW: Uplink.sendNow(); _needFull = true; break;
    case BTN_R1_ON: Control.setRelay(0, true); break;
    case BTN_R1_OFF: Control.setRelay(0, false); break;
    case BTN_R2_ON: Control.setRelay(1, true); break;
    case BTN_R2_OFF: Control.setRelay(1, false); break;
    case BTN_R1_MODE:
      Cfg.r1_mode = Cfg.r1_mode == RELAY_AUTO ? RELAY_MANUAL : RELAY_AUTO;
      Cfg.saveToSD();
      _needFull = true;
      break;
    case BTN_R2_MODE:
      Cfg.r2_mode = Cfg.r2_mode == RELAY_AUTO ? RELAY_MANUAL : RELAY_AUTO;
      Cfg.saveToSD();
      _needFull = true;
      break;
    case BTN_CAL_MODE:
      Sensors.calibrationMode = !Sensors.calibrationMode;
      Store.logEvent("calibration mode %s (touch)",
                     Sensors.calibrationMode ? "ON" : "off");
      _needFull = true;
      break;
    case BTN_TOUCH_CAL: touchCalibrate(); _needFull = true; break;
    case BTN_SENS_UP:
      if (_sensTop > 0) { _sensTop--; _needFull = true; }
      break;
    case BTN_SENS_DOWN:
      _sensTop++;  // clamped against the enabled count in drawSensors
      _needFull = true;
      break;
    case BTN_REBOOT:
      tft.fillScreen(C_BG);
      delay(200);
      ESP.restart();
      break;
  }
}

// Serial diagnostics for the resistive touch: raw ADC + mapped coordinates.
#define TOUCH_DEBUG 0

void UIManager::handleTouch() {
  int32_t x, y;
  bool pressed = tft.getTouch(&x, &y);
#if TOUCH_DEBUG
  {
    static uint32_t lastDbg = 0;
    if (millis() - lastDbg >= 500) {
      lastDbg = millis();
      int32_t rx = -1, ry = -1;
      uint_fast8_t rawN = tft.getTouchRaw(&rx, &ry);
      Serial.printf("[touch] raw n=%u (%ld,%ld)  mapped n=%d (%ld,%ld)\n",
                    (unsigned)rawN, (long)rx, (long)ry, (int)pressed, (long)x,
                    (long)y);
    }
  }
#endif
  if (pressed && !_wasPressed) {
    // tab bar
    if (y >= H - TAB_H) {
      Screen ns = (Screen)constrain(x / (W / 5), 0, 4);
      if (ns != _screen) {
        _screen = ns;
        _needFull = true;
      }
    } else {
      for (uint8_t i = 0; i < s_btnCount; i++) {
        const Btn &b = s_btns[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
          // visual feedback
          tft.drawRoundRect(b.x, b.y, b.w, b.h, 4, C_ACC);
          onButton(b.id);
          break;
        }
      }
    }
  }
  _wasPressed = pressed;
}

void UIManager::touchCalibrate() {
  uint16_t params[8];
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::middle_center);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextPadding(0);
  tft.drawString("CALIBRATION: press each arrow tip", W / 2, H / 2);
  Serial.println("[tcal] press the tip of each corner arrow (stylus is best)");
  // calibrateTouch blocks until the user presses all 4 arrows — unsubscribe
  // from the task watchdog so a slow operator doesn't trigger a reboot
  esp_task_wdt_delete(NULL);
  tft.calibrateTouch(params, C_TEXT, C_BG, 20);
  esp_task_wdt_add(NULL);
  Preferences prefs;
  prefs.begin("wynd", false);
  prefs.putBytes("tcal", params, sizeof(params));
  prefs.end();
  Serial.printf("[tcal] saved: %u,%u,%u,%u,%u,%u,%u,%u\n", params[0], params[1],
                params[2], params[3], params[4], params[5], params[6],
                params[7]);
  _needFull = true;
}

void UIManager::begin() {
  displayOk = tft.init();
  Serial.printf("[touch] driver %s\n", tft.touch() ? "attached" : "MISSING");
  tft.setRotation(1);  // landscape, 320x240
  tft.setBrightness(255);
  Preferences prefs;
  prefs.begin("wynd", true);
  uint16_t params[8];
  if (prefs.getBytes("tcal", params, sizeof(params)) == sizeof(params))
    tft.setTouchCalibrate(params);
  prefs.end();
  _needFull = true;
}

// Interactive touch diagnostic. Reads the XPT2046 two ways every cycle:
// through LovyanGFX (raw + calibrated/mapped) and directly over SPI
// (position + pressure channels), shows everything live on the panel and
// serial, and draws a crosshair at the mapped position.
static uint16_t xptDirect(uint8_t cmd) {
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

void UIManager::touchTest(uint32_t ms) {
  Serial.println("[ttest] touch test running - press anywhere, hold ~2s per spot");
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(lgfx::top_left);
  tft.setTextColor(C_ACC, C_BG);
  tft.setTextPadding(0);
  tft.drawString("TOUCH TEST - press corners & center", 8, 4);
  // reference targets
  tft.drawCircle(10, 10, 6, C_DIM);
  tft.drawCircle(W - 10, 10, 6, C_DIM);
  tft.drawCircle(10, H - 10, 6, C_DIM);
  tft.drawCircle(W - 10, H - 10, 6, C_DIM);
  tft.drawCircle(W / 2, H / 2, 6, C_DIM);

  uint32_t t0 = millis(), lastLog = 0;
  while (millis() - t0 < ms) {
    esp_task_wdt_reset();  // this diagnostic legitimately blocks the loop
    // direct channel reads (bus is free between LGFX operations)
    uint16_t dx = xptDirect(0xD0), dy = xptDirect(0x90);
    uint16_t z1 = xptDirect(0xB0), z2 = xptDirect(0xC0);
    // LGFX view
    int32_t rx = -1, ry = -1, mx = -1, my = -1;
    uint_fast8_t rawN = tft.getTouchRaw(&rx, &ry);
    bool mapped = tft.getTouch(&mx, &my);

    if (millis() - lastLog >= 250) {
      lastLog = millis();
      char l1[64], l2[64];
      snprintf(l1, sizeof(l1), "dir x=%4u y=%4u z1=%4u z2=%4u", dx, dy, z1, z2);
      snprintf(l2, sizeof(l2), "lgfx raw(%ld,%ld)%u map(%ld,%ld)%d",
               (long)rx, (long)ry, (unsigned)rawN, (long)mx, (long)my,
               (int)mapped);
      Serial.printf("[ttest] %s | %s\n", l1, l2);
      tft.setTextColor(C_TEXT, C_BG);
      tft.setTextPadding(W - 16);
      tft.drawString(l1, 8, 30);
      tft.drawString(l2, 8, 50);
      uint32_t left = (ms - (millis() - t0)) / 1000;
      char l3[24];
      snprintf(l3, sizeof(l3), "%lus left", (unsigned long)left);
      tft.setTextColor(C_DIM, C_BG);
      tft.setTextPadding(90);
      tft.drawString(l3, 8, H - 22);
    }
    if (mapped && mx >= 0 && mx < W && my >= 0 && my < H) {
      tft.fillCircle(mx, my, 3, C_OK);      // where LGFX thinks you pressed
    }
    delay(30);
  }
  Serial.println("[ttest] done");
  _needFull = true;
}

void UIManager::setScreen(uint8_t idx) {
  if (idx > 4) return;
  _screen = (Screen)idx;
  _needFull = true;
}

void UIManager::tick() {
  uint32_t now = millis();
  if (now - _lastTouchMs >= 30) {
    _lastTouchMs = now;
    handleTouch();
  }
  if (_needFull) {
    _needFull = false;
    _lastDynMs = now;
    drawScreen(true);
    return;
  }
  if (now - _lastDynMs >= 1000) {
    _lastDynMs = now;
    drawScreen(false);
  }
}
