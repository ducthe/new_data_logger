#include "ota.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "app_config.h"
#include "net_manager.h"
#include "pins.h"
#include "storage.h"

OtaManager Ota;

// ------------------------------------------------------------- http (GET) --
static bool parseUrl(const String &url, String &host, uint16_t &port,
                     String &path) {
  if (!url.startsWith("http://")) return false;  // TLS not supported here
  String rest = url.substring(7);
  int slash = rest.indexOf('/');
  path = slash < 0 ? "/" : rest.substring(slash);
  String hp = slash < 0 ? rest : rest.substring(0, slash);
  int colon = hp.indexOf(':');
  port = 80;
  host = hp;
  if (colon >= 0) {
    host = hp.substring(0, colon);
    port = (uint16_t)hp.substring(colon + 1).toInt();
  }
  return host.length() > 0;
}

static bool readLine(Client &c, String &line, uint32_t timeoutMs = 8000) {
  line = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!c.available()) {
      if (!c.connected()) return false;
      delay(2);
      continue;
    }
    char ch = (char)c.read();
    if (ch == '\r') continue;
    if (ch == '\n') return true;
    if (line.length() < 512) line += ch;
  }
  return false;
}

// Opens a GET request; on 200 returns true with the body ready to stream.
static bool httpOpen(Client &c, const String &url, int32_t &contentLen,
                     String &err) {
  String host, path;
  uint16_t port;
  if (!parseUrl(url, host, port, path)) { err = "bad url"; return false; }
  if (!c.connect(host.c_str(), port)) { err = "connect failed"; return false; }
  c.print("GET " + path + " HTTP/1.1\r\nHost: " + host +
          "\r\nConnection: close\r\n\r\n");
  String line;
  if (!readLine(c, line) || line.indexOf(" 200") < 0) {
    err = "http status: " + line;
    c.stop();
    return false;
  }
  contentLen = -1;
  while (readLine(c, line)) {
    if (line.length() == 0) return true;  // end of headers
    String low = line;
    low.toLowerCase();
    if (low.startsWith("content-length:"))
      contentLen = line.substring(15).toInt();
  }
  err = "header read failed";
  c.stop();
  return false;
}

// ------------------------------------------------------------ flash write --
bool OtaManager::applyFromStream(Stream &in, size_t len, const char *md5) {
  if (!Update.begin(len)) {
    log_e("OTA: not enough space (%u bytes)", (unsigned)len);
    return false;
  }
  if (md5 && strlen(md5) == 32) Update.setMD5(md5);
  uint8_t buf[1024];
  size_t done = 0;
  uint32_t lastData = millis();
  while (done < len) {
    esp_task_wdt_reset();  // slow LTE downloads may exceed the WDT window
    size_t avail = in.available();
    if (!avail) {
      if (millis() - lastData > 20000) break;  // stalled
      delay(2);
      continue;
    }
    size_t r = in.readBytes(buf, min(avail, sizeof(buf)));
    if (Update.write(buf, r) != r) break;
    done += r;
    lastData = millis();
    if (done % 65536 < 1024)
      Serial.printf("[ota] %u/%u kB\n", (unsigned)(done / 1024),
                    (unsigned)(len / 1024));
  }
  if (done != len || !Update.end()) {
    log_e("OTA: failed at %u/%u: %s", (unsigned)done, (unsigned)len,
          Update.errorString());
    Store.logEvent("OTA FAILED at %u/%u: %.60s", (unsigned)done, (unsigned)len,
                   Update.errorString());
    Update.abort();
    return false;
  }
  Serial.println("[ota] image written and verified, rebooting");
  Store.logEvent("OTA image applied (was fw=%s)", FW_VERSION);
  return true;
}

// -------------------------------------------------------------- SD update --
bool OtaManager::updateFromSD() {
  if (!Store.ready()) return false;
  // probe first: opening a missing file logs a VFS error on every boot
  if (!SD.exists("/firmware.bin")) return false;
  File f = SD.open("/firmware.bin", FILE_READ);
  if (!f || f.isDirectory()) return false;
  size_t sz = f.size();
  Serial.printf("[ota] firmware.bin found on SD (%u bytes), flashing...\n",
                (unsigned)sz);
  bool ok = sz > 0 && applyFromStream(f, sz, nullptr);
  f.close();
  SD.remove(ok ? "/firmware_ok.bin" : "/firmware_bad.bin");
  SD.rename("/firmware.bin", ok ? "/firmware_ok.bin" : "/firmware_bad.bin");
  if (ok) {
    delay(300);
    ESP.restart();
  }
  Serial.println("[ota] SD update FAILED (file renamed to firmware_bad.bin)");
  return false;
}

void OtaManager::checkSdAtBoot() { updateFromSD(); }

// ---------------------------------------------------------- remote update --
// Command-driven: the operator/platform supplies the firmware URL directly
// (MQTT {"cmd":"ota","url":...,"md5":...} or console `O <url> [md5]`).
// No version negotiation - an explicit command always flashes.
bool OtaManager::updateFromUrl(const char *url, const char *md5, String &msg) {
  if (!url || strncmp(url, "http://", 7) != 0) {
    msg = "bad url (http:// only)";
    return false;
  }
  Client *c = Net.ctrlClient();
  if (!c) { msg = "no network link"; return false; }

  Serial.printf("[ota] downloading %s (running %s)\n", url, FW_VERSION);
  Store.logEvent("OTA start: %.100s", url);
  int32_t len = -1;
  String err;
  if (!httpOpen(*c, String(url), len, err)) {
    msg = "download: " + err;
    return false;
  }
  if (len <= 0) { c->stop(); msg = "download: no content-length"; return false; }
  bool ok = applyFromStream(*c, (size_t)len,
                            (md5 && strlen(md5) == 32) ? md5 : nullptr);
  c->stop();
  if (!ok) { msg = "flash failed"; return false; }
  msg = "update applied, rebooting";
  Serial.println(msg);
  delay(300);
  ESP.restart();
  return true;  // not reached
}
