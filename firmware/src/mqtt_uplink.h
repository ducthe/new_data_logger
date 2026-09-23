#pragma once
#include <Arduino.h>

// Realtime MQTT uplink (optional, alongside the regulatory FTP path).
// Publishes the same cycle-averaged values as the MONRE file as JSON to
//   <topic>/data      every transmit cycle
//   <topic>/status    "online"/"offline" (retained, offline = last will)
// Topic base defaults to "wynd/<station>" when config leaves it empty.
// Works over Ethernet/WiFi/LTE on a dedicated socket. QoS 0 by design: the
// FTP + SD-buffer path is the guaranteed-delivery channel; MQTT is live view.
class MqttUplink {
 public:
  void tick();                        // connection upkeep + keepalive
  void publishCycle(const char *ts);  // call before averaging windows reset
  bool connected();
  String status();                    // for UI/console
  String baseTopic();
  uint32_t pubOk = 0, pubFail = 0;

 private:
  uint32_t _lastTryMs = 0;
};

extern MqttUplink Mqtt;
