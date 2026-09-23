#pragma once
#include <Arduino.h>

// Builds and transmits monitoring data files in the format used by the
// Vietnamese DONRE/MONRE continuous-monitoring data standard
// (TT 24/2017/TT-BTNMT, carried into TT 10/2021/TT-BTNMT):
//   one line per parameter:
//     <Name><sep><Value><sep><Unit><sep><YYYYMMDDhhmmss><sep><Status>\r\n
//   status: 00 normal, 01 calibrating, 02 device error
//   filename: <STATION>_<YYYYMMDDhhmmss>.txt, uploaded via FTP.
// Files that cannot be sent are buffered on SD and retried in order.
class MonreUplink {
 public:
  void tick();
  bool sendNow();                 // manual trigger from the UI
  // status for the UI
  String lastResult = "-";
  uint32_t lastAttemptMs = 0;
  uint32_t okCount = 0, failCount = 0;
  bool sending = false;
  uint32_t nextSendInMs() const;

 private:
  String buildFile(String &filename);   // averages the current window
  bool transmit(const String &filename, const String &content, String &err);
  void retryBuffered();
  uint32_t _lastSendMs = 0;
  uint32_t _lastCsvMs = 0;
};

extern MonreUplink Uplink;
