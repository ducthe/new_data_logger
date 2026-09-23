#pragma once
#include <Arduino.h>

// microSD on the shared SPI bus (CN1, behind a 74LVC125 buffer -> keep the
// clock modest). Layout:
//   /config.json            editable station configuration
//   /buffer/<file>.txt      MONRE files pending upload (link was down)
//   /data/YYYYMMDD.csv      local measurement history
//   /events/YYYYMM.log      operational event journal
class StorageManager {
 public:
  bool begin();
  bool ready() const { return _ok; }
  uint16_t bufferedCount();                       // pending files in /buffer
  bool bufferFile(const String &name, const String &content);
  // fetch oldest buffered file; returns false when buffer is empty
  bool oldestBuffered(String &name, String &content);
  void removeBuffered(const String &name);
  void appendCsv(const String &line);             // to /data/YYYYMMDD.csv
  uint64_t totalMB(), usedMB();

  // operational journal: timestamped one-liners for field forensics
  // (boot/reset, link changes, send failures, relay actions, commands...)
  void logEvent(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  String eventTail(size_t maxBytes = 2048);       // end of current month log

  // hourly housekeeping: prune old CSV/event files, cap the send buffer,
  // warn once when the card runs low on space
  void maintain();

 private:
  void pruneDir(const char *dir, const String &cutoffName, const char *what);
  bool _ok = false;
  int16_t _cachedCount = -1;
  uint32_t _lastMaintainMs = 0;
  bool _spaceWarned = false;
};

extern StorageManager Store;
