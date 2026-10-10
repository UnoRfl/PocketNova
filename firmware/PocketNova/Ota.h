#pragma once
// =====================================================================
//  Ota.h - firmware updates over Wi-Fi ("over the air", OTA)
// =====================================================================
//  The flash holds TWO app slots (the "Minimal SPIFFS" partition scheme):
//  the firmware runs from one while a new one is written into the other.
//  Only when the new one is complete and checks out does the chip switch
//  slots and restart. A cut-off download leaves the old firmware running.
//
//  While Pocket Nova is on your Wi-Fi, it listens on port 3232:
//    POST /update   the firmware .bin as the request body
//                   header X-Key: the update key (below)
//                   header X-Size: the file size, for the progress bar
//    GET  /info     {"device":"PocketNova","fw":"...","name":"..."}
//
//  THE KEY: anyone on your network could send a firmware, so every update
//  must carry a 32-character random key made on the first start. Only the
//  PC panel learns it, over the USB cable. (It travels in plain HTTP, so
//  someone already listening on your Wi-Fi could copy it. Fine at home;
//  real products sign their firmware instead.)
// =====================================================================

#include <Update.h>

const uint16_t OTA_PORT = 3232;
char       otaKey[33] = "";
WebServer* otaWeb = nullptr;
bool       otaKeyOk = false, otaFailed = false, otaBusy = false;
size_t     otaBytes = 0, otaTotal = 0;
uint32_t   otaDrawAt = 0;

void otaLoadKey() {
  prefs.begin("ota", false);
  if (prefs.isKey("key")) prefs.getString("key", otaKey, sizeof(otaKey));
  if (strlen(otaKey) != 32) {
    for (int i = 0; i < 16; i++) sprintf(otaKey + i * 2, "%02x", (unsigned)(esp_random() & 0xFF));
    prefs.putString("key", otaKey);
  }
  prefs.end();
}

// Compares every character even after a mismatch, so the time it takes
// doesn't give away how much of a guess was right.
bool otaKeyMatches(const String& k) {
  if (k.length() != 32) return false;
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= k[i] ^ otaKey[i];
  return diff == 0;
}

// The upload runs inside one web request, so the main loop is paused:
// draw the progress here, and tell the watchdog we're still alive.
void otaDraw() {
  esp_task_wdt_reset();
  if (millis() - otaDrawAt < 100) return;
  otaDrawAt = millis();
  float f = otaTotal ? (float)otaBytes / otaTotal : 0;
  clearFb();
  for (int i = 0; i < 25; i++) fb[i] = i < (int)(f * 25) ? CRGB(0, 120, 255) : CRGB(0, 8, 20);
  present();
}

void otaRaw() {
  HTTPRaw& r = otaWeb->raw();
  switch (r.status) {
    case RAW_START:
      otaKeyOk = otaKeyMatches(otaWeb->header("X-Key"));
      otaFailed = false;
      otaBytes = 0;
      otaTotal = otaWeb->header("X-Size").toInt();
      if (!otaKeyOk) {
        Serial.printf("[OTA] Refused an update from %s: wrong key\n", otaWeb->client().remoteIP().toString().c_str());
        break;
      }
      Serial.printf("[OTA] Receiving firmware from %s (%u bytes)\n", otaWeb->client().remoteIP().toString().c_str(), (unsigned)otaTotal);
      otaBusy = true;
      if (!Update.begin(otaTotal ? otaTotal : UPDATE_SIZE_UNKNOWN)) otaFailed = true;
      break;
    case RAW_WRITE:
      if (otaKeyOk && !otaFailed) {
        if (Update.write(r.buf, r.currentSize) != r.currentSize) otaFailed = true;
        otaBytes += r.currentSize;
        otaDraw();
      }
      esp_task_wdt_reset();
      break;
    case RAW_END:
      if (otaKeyOk && !otaFailed && !Update.end(true)) otaFailed = true;   // true = size from the data
      break;
    case RAW_ABORTED:
      if (otaKeyOk) Update.abort();
      otaFailed = true;
      otaBusy = false;                  // the request ends here, otaDone() won't run
      break;
  }
}

void otaDone() {
  if (!otaKeyOk) { otaWeb->send(403, "text/plain", "wrong key"); return; }
  otaBusy = false;
  if (otaFailed) {
    Serial.printf("[OTA] Failed: %s\n", Update.errorString());
    otaWeb->send(500, "text/plain", Update.errorString());
    fxError();
    return;
  }
  Serial.println("[OTA] Installed, restarting");
  otaWeb->send(200, "text/plain", "ok");
  restartSoon("installed an update over Wi-Fi");
}

// Runs every frame: the update server is up exactly while we're online.
void otaUpdate() {
  if (wifiState == WF_ONLINE && !otaWeb) {
    if (!otaKey[0]) otaLoadKey();
    otaWeb = new WebServer(OTA_PORT);
    static const char* HEADERS[] = {"X-Key", "X-Size"};
    otaWeb->collectHeaders(HEADERS, 2);
    otaWeb->on("/update", HTTP_POST, otaDone, otaRaw);
    otaWeb->on("/info", HTTP_GET, [] {
      JsonDocument d;
      d["device"] = "PocketNova";
      d["fw"] = FW_VERSION;
      d["name"] = cfg.name;
      String out;
      serializeJson(d, out);
      otaWeb->send(200, "application/json", out);
    });
    otaWeb->begin();
    Serial.printf("[OTA] Wi-Fi updates on http://%s:%u/\n", WiFi.localIP().toString().c_str(), OTA_PORT);
  } else if (wifiState != WF_ONLINE && otaWeb && !otaBusy) {
    otaWeb->stop();
    delete otaWeb;
    otaWeb = nullptr;
  }
  if (otaWeb) otaWeb->handleClient();
}
