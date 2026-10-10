#pragma once
// =====================================================================
//  NetLink.h - the PC panel over your Wi-Fi, and keys/mouse through it
// =====================================================================
//  While Pocket Nova is on your Wi-Fi it listens on TCP port 3233. The
//  PC panel connects and they exchange the same one-line JSON commands
//  as over the USB cable. Two things ride on it:
//    1. the panel itself (settings, live screen) with no cable
//    2. KEYS, MEDIA, SLIDES and the AIR MOUSE: instead of a Bluetooth
//       report, Pocket Nova sends  @{"t":"hid","r":3,"d":"00fe0100"}
//       and the panel presses those keys / moves the mouse on Windows.
//       Same report bytes as Bluetooth, just another way to get there.
//
//  PROVING WHO'S WHO (challenge-response): both sides already know the
//  secret key (Ota.h; the panel learns it once over USB). The key itself
//  never crosses the network:
//    Pocket Nova -> panel   a random number N1 ("the challenge")
//    panel -> Pocket Nova   HMAC(key, "dev:" + N1)  and its own random N2
//    Pocket Nova -> panel   HMAC(key, "pc:" + N2)
//  HMAC-SHA256 mixes the key into a fingerprint that can't be turned back
//  into the key, and a fresh random number each time means an old answer
//  can't be replayed. So neither side can be faked by something else on
//  your network. (The traffic isn't encrypted: someone already on your
//  Wi-Fi could watch which keys go by. Fine at home.)
//
//  WHICH WAY KEYS GO (cfg.inputVia, set in the panel):
//    0 auto       Bluetooth while it's connected, Wi-Fi when it drops
//    1 Bluetooth  never over Wi-Fi
//    2 Wi-Fi      Wi-Fi whenever the panel is linked, Bluetooth otherwise
// =====================================================================

#include <mbedtls/md.h>
#include <lwip/sockets.h>

const uint16_t LINK_PORT = 3233;

WiFiServer* linkSrv = nullptr;
WiFiClient  linkCli;
bool     linkAuthed = false;          // the panel proved it knows the key
bool     linkInput = false;           // the panel accepts keys/mouse from us
bool     replyNet = false;            // sendJson() answers over the link
bool     netMirror = false;           // stream the screen over the link
char     linkNonce[33];
char     linkBuf[400];
size_t   linkLen = 0;
uint32_t linkSince = 0, linkSeen = 0, linkLockUntil = 0;
uint8_t  linkFails = 0;
uint32_t linkHid = 0, linkSkipped = 0;

void hmacHex(const char* msg, char out[65]) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)otaKey, strlen(otaKey),
                  (const uint8_t*)msg, strlen(msg), mac);
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
}

bool linkUp() { return linkAuthed && linkCli.connected(); }

void linkDrop(const char* why) {
  if (linkCli) {
    if (linkAuthed) Serial.printf("[LINK] Panel link closed: %s\n", why);
    linkCli.stop();
  }
  linkAuthed = linkInput = netMirror = false;
  linkLen = 0;
}

// Sends one line WITHOUT waiting: if the Wi-Fi is too slow to take it right
// now, the line is skipped rather than freezing Pocket Nova (a normal send
// can block for seconds on a weak signal). Key and mouse reports carry the
// whole state each time, so the next one puts things right.
bool netSendLine(const String& s) {
  int fd = linkCli.fd();
  if (fd < 0) return false;
  String out = s + '\n';
  int n = send(fd, out.c_str(), out.length(), MSG_DONTWAIT);
  if (n == (int)out.length()) return true;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { linkSkipped++; return false; }
  linkDrop(n < 0 ? "send failed" : "send cut short");
  return false;
}

// Keys and mouse can go this way right now?
bool netInputReady() { return cfg.inputVia != 1 && linkUp() && linkInput; }

// NovaKeyboard calls this before every Bluetooth report (see setReportSink).
bool netHidSink(uint8_t id, const uint8_t* d, size_t n) {
  if (!netInputReady()) return false;
  if (cfg.inputVia == 0 && bleKeyboard.isConnected()) return false;   // auto: Bluetooth while it's there
  char line[64];
  int k = snprintf(line, sizeof(line), "@{\"t\":\"hid\",\"r\":%u,\"d\":\"", id);
  for (size_t i = 0; i < n && k < (int)sizeof(line) - 4; i++) k += snprintf(line + k, sizeof(line) - k, "%02x", d[i]);
  strcpy(line + k, "\"}");
  if (netSendLine(line)) linkHid++;
  return true;
}

void linkLine(const char* line) {
  linkSeen = millis();
  JsonDocument in;
  if (deserializeJson(in, line)) return;
  const char* cmd = in["cmd"] | "";
  if (!linkAuthed) {
    if (strcmp(cmd, "auth")) { linkDrop("spoke before proving the key"); return; }
    char msg[48], want[65];
    snprintf(msg, sizeof(msg), "dev:%s", linkNonce);
    hmacHex(msg, want);
    const char* got = in["mac"] | "";
    uint8_t diff = strlen(got) != 64;
    for (int i = 0; i < 64 && got[i]; i++) diff |= got[i] ^ want[i];   // every character, every time
    const char* pn = in["n"] | "";
    if (diff || strlen(pn) != 32) {
      Serial.printf("[LINK] Refused %s: wrong key\n", linkCli.remoteIP().toString().c_str());
      if (++linkFails >= 3) { linkFails = 0; linkLockUntil = millis() + 60000; }   // 3 wrong: nobody for a minute
      linkDrop("wrong key");
      return;
    }
    linkFails = 0;
    linkAuthed = true;
    linkInput = in["input"] | false;
    snprintf(msg, sizeof(msg), "pc:%s", pn);
    char proof[65];
    hmacHex(msg, proof);
    JsonDocument d;
    d["t"] = "auth";
    d["ok"] = true;
    d["mac"] = proof;
    netSendLine(atJson(d));
    Serial.printf("[LINK] Panel linked over Wi-Fi from %s%s\n", linkCli.remoteIP().toString().c_str(),
                  linkInput ? " (keys and mouse can go this way)" : "");
    return;
  }
  if (!strcmp(cmd, "ping"))   { netSendLine("@{\"t\":\"pong\"}"); return; }
  if (!strcmp(cmd, "mirror")) { netMirror = in["on"] | false; netSendLine("@{\"t\":\"ok\",\"cmd\":\"mirror\"}"); return; }
  if (!strcmp(cmd, "linkcfg")) {
    if (in["input"].is<bool>()) linkInput = in["input"];
    netSendLine("@{\"t\":\"ok\",\"cmd\":\"linkcfg\"}");
    return;
  }
  replyNet = true;
  handleRemoteLine(line);
  replyNet = false;
}

// Every frame.
void linkUpdate() {
  uint32_t now = millis();
  if (wifiState != WF_ONLINE) {
    if (linkSrv) { linkDrop("Wi-Fi gone"); linkSrv->end(); delete linkSrv; linkSrv = nullptr; }
    return;
  }
  if (!linkSrv) {
    linkSrv = new WiFiServer(LINK_PORT);
    linkSrv->begin();
    linkSrv->setNoDelay(true);
    Serial.printf("[LINK] Panel link on %s:%u\n", WiFi.localIP().toString().c_str(), LINK_PORT);
  }
  if (linkSrv->hasClient()) {
    WiFiClient c = linkSrv->available();
    if (linkUp() && now - linkSeen < 15000) c.stop();          // busy with a live panel
    else if ((int32_t)(now - linkLockUntil) < 0) c.stop();     // too many wrong keys lately
    else {
      linkDrop("replaced");
      linkCli = c;
      linkCli.setNoDelay(true);                                 // send small lines at once (no batching)
      linkSince = linkSeen = now;
      for (int i = 0; i < 16; i++) sprintf(linkNonce + i * 2, "%02x", (unsigned)(esp_random() & 0xFF));
      JsonDocument d;
      d["t"] = "challenge";
      d["n"] = linkNonce;
      d["device"] = "PocketNova";
      d["fw"] = FW_VERSION;
      d["name"] = cfg.name;
      netSendLine(atJson(d));
    }
  }
  if (!linkCli) return;
  if (!linkCli.connected()) { linkDrop("the panel left"); return; }
  if (!linkAuthed && now - linkSince > 5000) { linkDrop("no answer to the challenge"); return; }
  if (linkAuthed && now - linkSeen > 20000) { linkDrop("silent for 20 s"); return; }
  for (int budget = 8; budget && linkCli && linkCli.available(); ) {   // a few lines per frame, at most
    char ch = linkCli.read();
    if (ch == '\n' || ch == '\r') {
      if (!linkLen) continue;
      linkBuf[linkLen] = 0;
      linkLen = 0;
      budget--;
      linkLine(linkBuf);
    } else if (linkLen < sizeof(linkBuf) - 1) {
      linkBuf[linkLen++] = ch;
    } else {
      linkLen = 0;                                              // too long: drop it
    }
  }
  static uint32_t lastFrame = 0;
  if (netMirror && linkUp() && now - lastFrame >= 150) {       // the live screen, ~7 times a second
    lastFrame = now;
    static const char HEX_[] = "0123456789abcdef";
    String s = "@{\"t\":\"fb\",\"d\":\"";
    for (int i = 0; i < 25; i++) {
      const uint8_t c[3] = {fb[i].r, fb[i].g, fb[i].b};
      for (int k = 0; k < 3; k++) { s += HEX_[c[k] >> 4]; s += HEX_[c[k] & 15]; }
    }
    s += "\"}";
    netSendLine(s);
  }
}
