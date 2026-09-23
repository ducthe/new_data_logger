#pragma once
#include <Arduino.h>

// 3.2" 320x240 SPI TFT (ILI9341-class) + XPT2046 resistive touch, both on the
// shared SPI bus. Five-tab local interface for station operators/inspectors.
class UIManager {
 public:
  void begin();
  void tick();
  void setScreen(uint8_t idx);   // serial-console fallback navigation
  void touchTest(uint32_t ms);   // interactive diagnostic (console 't')
  void touchCalibrate();         // 4-point calibration, saved to NVS ('T')
  bool displayOk = false;

 private:
  enum Screen : uint8_t { SCR_HOME, SCR_SENSORS, SCR_NETWORK, SCR_CONTROL, SCR_SYSTEM };
  void drawChrome();          // header + tab bar
  void drawHeaderDynamic();
  void drawScreen(bool full);
  void drawHome(bool full);
  void drawSensors(bool full);
  void drawNetwork(bool full);
  void drawControl(bool full);
  void drawSystem(bool full);
  void handleTouch();
  void onButton(uint8_t id);
  void addButton(int16_t x, int16_t y, int16_t w, int16_t h,
                 const char *label, uint8_t id, uint16_t color);
  void clearButtons();

  Screen _screen = SCR_HOME;
  uint8_t _sensTop = 0;       // first visible row on the Sensors screen
  bool _needFull = true;
  uint32_t _lastDynMs = 0;
  uint32_t _lastTouchMs = 0;
  bool _wasPressed = false;
};

extern UIManager UI;
