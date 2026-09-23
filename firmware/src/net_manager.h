#pragma once
#include <Arduino.h>
#include <Client.h>

enum class LinkType : uint8_t { NONE, ETH, WIFI, LTE };

// Manages W5500 Ethernet (preferred) and the A7682S LTE modem (fallback),
// plus wall-clock sync (NTP over Ethernet, +CCLK over LTE).
class StationNetwork {
 public:
  void begin();
  void tick();

  LinkType active() const;
  bool timeSynced() const { return _timeSynced; }

  // Clients bound to the currently active link (nullptr when offline)
  Client *ctrlClient();
  Client *dataClient();
  Client *mqttClient();   // dedicated socket: MQTT holds it persistently

  // UI/status
  bool ethUp() const { return _ethUp; }
  bool wifiUp() const;
  bool lteUp() const { return _lteState == LTE_READY; }
  String ethIp() const;
  String wifiStatus() const;   // "disabled" | "connecting" | "<ip> (<dBm>)"
  String lteOperator() const { return _lteOperator; }
  int lteRssi() const { return _lteRssi; }      // CSQ 0-31, 99 unknown
  const char *lteStateName() const;

 private:
  // Ethernet
  void ethTick();
  // Wi-Fi
  void wifiTick();
  bool _wifiWasUp = false;
  uint32_t _wifiLastCheckMs = 0;
  bool _ethUp = false;
  bool _ethStarted = false;
  uint32_t _ethLastTryMs = 0;
  uint32_t _ethLastCheckMs = 0;

  // LTE state machine
  enum LteState : uint8_t {
    LTE_OFF, LTE_PWR_PULSE, LTE_WAIT_AT, LTE_INIT, LTE_WAIT_NET,
    LTE_ATTACH, LTE_READY, LTE_BACKOFF, LTE_NO_SIM
  };
  void lteTick();
  LteState _lteState = LTE_OFF;
  uint32_t _lteStateMs = 0;
  uint32_t _lteBackoffMs = 15000;
  uint32_t _lteLastPollMs = 0;
  String _lteOperator = "";
  int _lteRssi = 99;

  // time
  void timeTick();
  bool syncNtpEth();
  bool syncTimeLte();
  bool _timeSynced = false;
  uint32_t _lastTimeSyncMs = 0;
  uint32_t _lastTimeTryMs = 0;
};

extern StationNetwork Net;
