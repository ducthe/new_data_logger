#pragma once
#include <Arduino.h>

// Firmware OTA:
//  - SD card:  drop firmware.bin in the card root, reboot (or console 'U').
//              Renamed to firmware_ok.bin / firmware_bad.bin afterwards.
//  - Remote:   command-driven over the active link (ETH/WiFi/LTE): the
//              operator/platform sends the firmware URL directly via MQTT
//              {"cmd":"ota","url":"http://host/fw.bin","md5":"..."} or
//              console `O <url> [md5]`. MD5 (optional) is verified before
//              the new slot is activated.
class OtaManager {
 public:
  void checkSdAtBoot();          // call once in setup after SD is mounted
  bool updateFromSD();           // returns only on failure (reboots on success)
  // direct download + flash; returns only on failure (reboots on success)
  bool updateFromUrl(const char *url, const char *md5, String &msg);

 private:
  bool applyFromStream(Stream &in, size_t len, const char *md5);
};

extern OtaManager Ota;
