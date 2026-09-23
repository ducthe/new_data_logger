#pragma once
#include <Arduino.h>
#include <Client.h>

// Minimal passive-mode FTP upload over any Arduino Client (EthernetClient or
// TinyGsmClient), so the same code path serves Ethernet and LTE.
class FtpUploader {
 public:
  // Uploads `content` as `filename` into `dir` on the server.
  // On failure returns false and fills `err`.
  static bool put(Client &ctrl, Client &data,
                  const char *host, uint16_t port,
                  const char *user, const char *pass,
                  const char *dir, const char *filename,
                  const String &content, String &err);

 private:
  static int readReply(Client &c, String &line, uint32_t timeoutMs = 8000);
  static bool cmd(Client &c, const String &command, int expectClass,
                  String &reply, String &err);
};
