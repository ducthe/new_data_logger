#include "storage.h"
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include "app_config.h"
#include "pins.h"

StorageManager Store;

bool StorageManager::begin() {
  // 10 MHz: conservative through the 74LVC125 buffer
  _ok = SD.begin(PIN_CS_SD, SPI, 10000000);
  if (_ok) {
    SD.mkdir("/buffer");
    SD.mkdir("/data");
    SD.mkdir("/events");
  }
  return _ok;
}

void StorageManager::logEvent(const char *fmt, ...) {
  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[event] %s\n", msg);
  if (!_ok) return;

  time_t now = time(nullptr);
  char path[24], stamp[32];
  if (now > 1600000000) {  // wall clock is valid
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(path, sizeof(path), "/events/%04d%02d.log", tmv.tm_year + 1900,
             tmv.tm_mon + 1);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min, tmv.tm_sec);
  } else {  // not synced yet: log against uptime into a catch-all file
    strcpy(path, "/events/nosync.log");
    snprintf(stamp, sizeof(stamp), "up+%lus", (unsigned long)(millis() / 1000));
  }
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  f.printf("%s  %s\n", stamp, msg);
  f.close();
}

String StorageManager::eventTail(size_t maxBytes) {
  if (!_ok) return "(no SD)";
  time_t now = time(nullptr);
  char path[24];
  if (now > 1600000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(path, sizeof(path), "/events/%04d%02d.log", tmv.tm_year + 1900,
             tmv.tm_mon + 1);
  } else {
    strcpy(path, "/events/nosync.log");
  }
  File f = SD.open(path, FILE_READ);
  if (!f) return "(no events yet)";
  if (f.size() > maxBytes) f.seek(f.size() - maxBytes);
  String out = f.readString();
  f.close();
  return out;
}

// Deletes files in `dir` whose basename sorts before `cutoffName`
// (date-encoded names make lexicographic == chronological).
void StorageManager::pruneDir(const char *dir, const String &cutoffName,
                              const char *what) {
  File d = SD.open(dir);
  if (!d) return;
  uint16_t removed = 0;
  File f;
  String victims[16];
  uint8_t nv = 0;
  // collect in small batches: deleting while iterating breaks openNextFile
  do {
    nv = 0;
    d.rewindDirectory();
    while ((f = d.openNextFile()) && nv < 16) {
      if (!f.isDirectory()) {
        String n = f.name();
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        if (n < cutoffName) victims[nv++] = n;
      }
      f.close();
    }
    if (f) f.close();
    for (uint8_t k = 0; k < nv; k++) {
      SD.remove(String(dir) + "/" + victims[k]);
      removed++;
    }
  } while (nv == 16 && removed < 500);
  d.close();
  if (removed) logEvent("retention: removed %u old %s files", removed, what);
}

void StorageManager::maintain() {
  if (!_ok) return;
  uint32_t nowMs = millis();
  if (_lastMaintainMs != 0 && nowMs - _lastMaintainMs < 3600000UL) return;
  _lastMaintainMs = nowMs;

  // low-space warning (once until reboot), at 90% used
  uint64_t total = totalMB(), used = usedMB();
  if (total > 0 && used * 10 >= total * 9 && !_spaceWarned) {
    _spaceWarned = true;
    logEvent("WARNING: SD card %llu/%llu MB used", (unsigned long long)used,
             (unsigned long long)total);
  }

  // cap the send backlog regardless of clock state
  uint16_t excess = 0;
  _cachedCount = -1;
  uint16_t cnt = bufferedCount();
  if (cnt > Cfg.buffer_max) excess = cnt - Cfg.buffer_max;
  for (uint16_t k = 0; k < excess && k < 50; k++) {  // max 50 per pass
    String name, content;
    if (!oldestBuffered(name, content)) break;
    removeBuffered(name);
  }
  if (excess) logEvent("retention: dropped %u oldest buffered files (cap %u)",
                       min<uint16_t>(excess, 50), Cfg.buffer_max);

  // date-based pruning requires a valid clock
  time_t now = time(nullptr);
  if (now < 1600000000) return;
  struct tm tmv;
  time_t cutoff = now - (time_t)Cfg.retention_days * 86400;
  localtime_r(&cutoff, &tmv);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
           tmv.tm_mday);
  pruneDir("/data", String(buf) + ".csv", "csv");
  // event logs: keep 13 months
  time_t evCut = now - 13L * 31 * 86400;
  localtime_r(&evCut, &tmv);
  snprintf(buf, sizeof(buf), "%04d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
  pruneDir("/events", String(buf) + ".log", "event");
}

uint64_t StorageManager::totalMB() { return _ok ? SD.totalBytes() / (1024ULL * 1024ULL) : 0; }
uint64_t StorageManager::usedMB() { return _ok ? SD.usedBytes() / (1024ULL * 1024ULL) : 0; }

uint16_t StorageManager::bufferedCount() {
  if (!_ok) return 0;
  if (_cachedCount >= 0) return _cachedCount;
  uint16_t n = 0;
  File dir = SD.open("/buffer");
  if (!dir) return 0;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) n++;
    f.close();
    if (n >= 9999) break;
  }
  dir.close();
  _cachedCount = n;
  return n;
}

bool StorageManager::bufferFile(const String &name, const String &content) {
  if (!_ok) return false;
  File f = SD.open("/buffer/" + name, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();
  _cachedCount = -1;
  return true;
}

bool StorageManager::oldestBuffered(String &name, String &content) {
  if (!_ok) return false;
  File dir = SD.open("/buffer");
  if (!dir) return false;
  String best;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      String n = f.name();
      int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      // filenames embed the timestamp, so lexicographic min == oldest
      if (best.length() == 0 || n < best) best = n;
    }
    f.close();
  }
  dir.close();
  if (best.length() == 0) return false;
  File in = SD.open("/buffer/" + best, FILE_READ);
  if (!in) return false;
  content = in.readString();
  in.close();
  name = best;
  return true;
}

void StorageManager::removeBuffered(const String &name) {
  if (!_ok) return;
  SD.remove("/buffer/" + name);
  _cachedCount = -1;
}

void StorageManager::appendCsv(const String &line) {
  if (!_ok) return;
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char path[32];
  snprintf(path, sizeof(path), "/data/%04d%02d%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  f.println(line);
  f.close();
}
