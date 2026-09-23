#include "ftp_client.h"
#include <esp_task_wdt.h>

// Reads one full FTP reply (handles multi-line "xxx-" replies).
// Returns the 3-digit code, or -1 on timeout.
int FtpUploader::readReply(Client &c, String &line, uint32_t timeoutMs) {
  line = "";
  String cur = "";
  int code = -1;
  bool multiline = false;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    esp_task_wdt_reset();  // several replies may chain in one loop() pass
    if (!c.available()) {
      if (!c.connected()) break;
      delay(5);
      continue;
    }
    char ch = (char)c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { cur += ch; continue; }
    // full line received
    if (line.length() < 256) line += cur + "\n";
    if (cur.length() >= 3 && isDigit(cur[0]) && isDigit(cur[1]) && isDigit(cur[2])) {
      int c3 = cur.substring(0, 3).toInt();
      if (cur.length() > 3 && cur[3] == '-') { multiline = true; code = c3; }
      else if (!multiline || c3 == code) return c3;
    }
    cur = "";
  }
  return -1;
}

bool FtpUploader::cmd(Client &c, const String &command, int expectClass,
                      String &reply, String &err) {
  c.print(command + "\r\n");
  int code = readReply(c, reply);
  if (code / 100 != expectClass) {
    err = command.startsWith("PASS") ? "PASS" : command;
    err += " -> " + String(code) + " " + reply;
    return false;
  }
  return true;
}

bool FtpUploader::put(Client &ctrl, Client &data,
                      const char *host, uint16_t port,
                      const char *user, const char *pass,
                      const char *dir, const char *filename,
                      const String &content, String &err) {
  String reply;
  err = "";
  if (!ctrl.connect(host, port)) { err = "ctrl connect failed"; return false; }
  bool ok = false;
  do {
    if (readReply(ctrl, reply) / 100 != 2) { err = "no banner: " + reply; break; }
    ctrl.print(String("USER ") + user + "\r\n");
    int ucode = readReply(ctrl, reply);
    if (ucode / 100 == 3) {  // password required (331)
      if (!cmd(ctrl, String("PASS ") + pass, 2, reply, err)) break;
    } else if (ucode / 100 != 2) {  // 230 = logged in without password
      err = "USER -> " + String(ucode);
      break;
    }
    if (!cmd(ctrl, "TYPE I", 2, reply, err)) break;
    if (dir && strlen(dir) > 0 && strcmp(dir, "/") != 0) {
      if (!cmd(ctrl, String("CWD ") + dir, 2, reply, err)) break;
    }
    if (!cmd(ctrl, "PASV", 2, reply, err)) break;
    // parse "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
    int lp = reply.indexOf('(');
    int rp = reply.indexOf(')', lp);
    if (lp < 0 || rp < 0) { err = "bad PASV: " + reply; break; }
    int v[6] = {0};
    {
      String nums = reply.substring(lp + 1, rp);
      int idx = 0;
      char buf[64];
      nums.toCharArray(buf, sizeof(buf));
      char *tok = strtok(buf, ",");
      while (tok && idx < 6) { v[idx++] = atoi(tok); tok = strtok(nullptr, ","); }
      if (idx != 6) { err = "bad PASV nums"; break; }
    }
    uint16_t dport = v[4] * 256 + v[5];
    IPAddress pasvIp(v[0], v[1], v[2], v[3]);
    log_i("FTP: PASV %s -> data %s:%u (host %s)", reply.c_str(),
          pasvIp.toString().c_str(), dport, host);
    // Prefer the control hostname (correct when the server sits behind NAT
    // and advertises a private address); fall back to the advertised IP,
    // which avoids a second DNS lookup on links where that is the problem.
    uint32_t t0 = millis();
    bool dok = data.connect(host, dport);
    log_i("FTP: data connect by host %s in %lums", dok ? "OK" : "FAILED",
          (unsigned long)(millis() - t0));
    if (!dok) {
      data.stop();
      t0 = millis();
      dok = data.connect(pasvIp, dport);
      log_i("FTP: data connect by IP %s in %lums", dok ? "OK" : "FAILED",
            (unsigned long)(millis() - t0));
    }
    if (!dok) { err = "data connect failed"; break; }
    ctrl.print(String("STOR ") + filename + "\r\n");
    int code = readReply(ctrl, reply);
    if (code / 100 != 1) { err = "STOR -> " + String(code); data.stop(); break; }
    t0 = millis();
    size_t written = data.write((const uint8_t *)content.c_str(), content.length());
    data.flush();
    log_i("FTP: wrote %u/%u bytes in %lums", (unsigned)written,
          (unsigned)content.length(), (unsigned long)(millis() - t0));
    // give the modem a moment to push the last segment: closing the socket
    // immediately after CIPSEND can drop data still in its TX queue
    delay(300);
    data.stop();
    if (written != content.length()) { err = "short write"; break; }
    t0 = millis();
    code = readReply(ctrl, reply, 20000);
    if (code / 100 != 2) {
      log_w("FTP: no transfer-complete after %lums; ctrl connected=%d "
            "available=%d partial='%s'", (unsigned long)(millis() - t0),
            (int)ctrl.connected(), ctrl.available(), reply.c_str());
      err = "xfer close -> " + String(code);
      break;
    }
    ctrl.print("QUIT\r\n");
    readReply(ctrl, reply, 2000);
    ok = true;
  } while (false);
  ctrl.stop();
  data.stop();
  // A cellular modem does not release a socket the instant CIPCLOSE returns.
  // Reopening the same mux too soon gives a connect() that reports success
  // but never delivers data, which showed up as the next session failing at
  // a random step (PASV/PASS/transfer-complete all timing out).
  delay(1500);
  return ok;
}
