#pragma once
// =====================================================================
//  Finder.h - find your own Bluetooth thing (earbuds, a tracker, a
//  watch) by its signal strength: hotter = closer.
// =====================================================================
//  Bluetooth devices "advertise": a few times a second they broadcast a
//  short packet (see NovaKeyboard.cpp). Listening for those is SCANNING,
//  and every packet arrives with its RSSI, the received signal strength
//  in dBm: about -40 right next to you, -70 across a room, -90 barely
//  there. Walls and your body soak it up, so it jumps around: we smooth
//  it, and you walk around watching which way it gets warmer.
//
//  Pocket Nova keeps being a keyboard while it listens: it scans in short
//  windows (30 ms of every 100 ms) so the PC connection still gets air.
//
//  MOVING ADDRESSES: many phones and earbuds change their address every
//  few minutes for privacy. If the one you track vanishes, a device
//  advertising the same NAME is picked up instead.
// =====================================================================

#include <algorithm>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

struct Seen { uint8_t addr[6]; int8_t rssi; float smooth; char name[20]; uint32_t at; };
const int  SEEN_MAX = 24;
Seen       seen[SEEN_MAX];
portMUX_TYPE seenMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t  findAddr[6];                 // the target (saved in NVS "find")
char     findName[20] = "";
bool     findHave = false;
float    findRssi = -100;             // smoothed
uint32_t findSeenAt = 0;
float    findTrend = 0;               // + = getting warmer
bool     scanOn = false;
uint32_t scanUntil = 0;               // the panel's list scan stops itself here (0 = while the app is open)
bool     finderOpen = false;

class FinderCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {      // runs in the Bluetooth task
    BLEAddress ad = d.getAddress();
    const uint8_t* a = *ad.getNative();
    int8_t r = d.getRSSI();
    uint32_t now = millis();
    portENTER_CRITICAL(&seenMux);
    Seen* s = nullptr;
    for (auto& x : seen) if (x.at && !memcmp(x.addr, a, 6)) { s = &x; break; }
    if (!s) {                                          // new: take an empty or the stalest slot
      s = &seen[0];
      for (auto& x : seen) if (x.at < s->at) s = &x;
      memcpy(s->addr, a, 6);
      s->name[0] = 0;
      s->smooth = r;
    }
    s->rssi = r;
    s->smooth = s->smooth * 0.7f + r * 0.3f;
    s->at = now | 1;
    portEXIT_CRITICAL(&seenMux);
    if (d.haveName()) {                                // names come in the "scan response", now and then
      std::string n = d.getName();
      portENTER_CRITICAL(&seenMux);
      strncpy(s->name, n.c_str(), sizeof(s->name) - 1);
      s->name[sizeof(s->name) - 1] = 0;
      portEXIT_CRITICAL(&seenMux);
    }
  }
};
FinderCallbacks finderCb;

void finderLoad() {
  prefs.begin("find", true);
  findHave = prefs.getBytes("addr", findAddr, 6) == 6;
  if (prefs.isKey("name")) prefs.getString("name", findName, sizeof(findName));
  prefs.end();
}
void finderSave() {
  prefs.begin("find", false);
  if (findHave) { prefs.putBytes("addr", findAddr, 6); prefs.putString("name", findName); }
  else { prefs.remove("addr"); prefs.remove("name"); }
  prefs.end();
}

void scanStart(uint32_t forMs) {
  if (forMs) scanUntil = millis() + forMs;
  if (scanOn) return;
  BLEScan* s = BLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&finderCb, true);   // true = every packet, not just the first
  s->setActiveScan(true);                            // ask for the scan response (it has the name)
  s->setInterval(100);
  s->setWindow(30);
  memset(seen, 0, sizeof(seen));
  scanOn = s->start(0, nullptr, false);               // 0 = until stopped
  Serial.printf("[FIND] Scanning %s\n", scanOn ? "started" : "failed");
}
void scanStop() {
  if (!scanOn) return;
  BLEDevice::getScan()->stop();
  scanOn = false;
  scanUntil = 0;
  Serial.println("[FIND] Scanning stopped");
}

void finderSetTarget(const uint8_t* a, const char* name) {
  memcpy(findAddr, a, 6);
  strncpy(findName, name ? name : "", sizeof(findName) - 1);
  findName[sizeof(findName) - 1] = 0;
  findHave = true;
  findRssi = -100;
  findSeenAt = 0;
  finderSave();
  Serial.printf("[FIND] Tracking %s %s\n", macToString(findAddr).c_str(), findName);
}
void finderClear() { findHave = false; finderSave(); }

// Every frame: follow the target, and stop the panel's scan when it's done.
void finderUpdate() {
  uint32_t now = millis();
  if (scanOn && scanUntil && (int32_t)(now - scanUntil) > 0 && !finderOpen) scanStop();
  if (!scanOn || !findHave) return;
  portENTER_CRITICAL(&seenMux);
  Seen* hit = nullptr;
  for (auto& x : seen) if (x.at && !memcmp(x.addr, findAddr, 6)) { hit = &x; break; }
  // Gone for 8 s? Look for the same name under a new address.
  if ((!hit || now - hit->at > 8000) && findName[0])
    for (auto& x : seen)
      if (x.at && now - x.at < 3000 && !strcmp(x.name, findName)) { hit = &x; memcpy(findAddr, x.addr, 6); break; }
  if (hit && now - hit->at < 3000) {
    float before = findRssi;
    findRssi = findRssi < -99 ? hit->smooth : findRssi * 0.85f + hit->smooth * 0.15f;
    findTrend = findTrend * 0.9f + (findRssi - before) * 0.1f * 10;
    findSeenAt = hit->at;
  }
  portEXIT_CRITICAL(&seenMux);
}

// The strongest device heard in the last 3 s (for "tap to lock on").
bool finderStrongest(uint8_t* addr, char* name, int8_t* rssi) {
  uint32_t now = millis();
  bool got = false;
  portENTER_CRITICAL(&seenMux);
  float best = -200;
  for (auto& x : seen)
    if (x.at && now - x.at < 3000 && x.smooth > best) {
      best = x.smooth;
      memcpy(addr, x.addr, 6);
      strcpy(name, x.name);
      *rssi = (int8_t)x.smooth;
      got = true;
    }
  portEXIT_CRITICAL(&seenMux);
  return got;
}

void finderList(JsonArray a) {
  uint32_t now = millis();
  Seen copy[SEEN_MAX];
  portENTER_CRITICAL(&seenMux);
  memcpy(copy, seen, sizeof(seen));
  portEXIT_CRITICAL(&seenMux);
  std::sort(copy, copy + SEEN_MAX, [](const Seen& x, const Seen& y) { return x.smooth > y.smooth; });
  for (auto& x : copy) {
    if (!x.at || now - x.at > 10000) continue;
    JsonObject o = a.add<JsonObject>();
    o["addr"] = macToString(x.addr);
    o["rssi"] = (int)x.smooth;
    if (x.name[0]) o["name"] = x.name;
    o["ago"] = now - x.at;
  }
}

// ---------------------------------------------------------------------
//  FINDER app.
//    no target yet: hold Pocket Nova right next to your thing and tap; it
//                   locks onto the strongest signal it hears
//    tracking:      rings fill in and turn from blue (cold) to red (hot),
//                   and the pulse speeds up as you get closer.
//                   Tap = read out the signal. Tilt left + tap = pick again.
// ---------------------------------------------------------------------
Scroller findScroll;

void finderEnter() { finderOpen = true; findScroll.stop(); scanStart(0); findRssi = -100; findSeenAt = 0; }
void finderLeave() { finderOpen = false; scanStop(); }

bool finderFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  clearFb();
  if (!findHave) {
    if (e == EV_TAP) {
      uint8_t a[6]; char n[20]; int8_t r;
      if (finderStrongest(a, n, &r) && r > -75) {
        finderSetTarget(a, n);
        fxRipple(CRGB(0, 255, 80));
        findScroll.start(n[0] ? n : "LOCKED ON", CRGB(0, 255, 80));
      } else {
        fxError();
        findScroll.start("NOTHING CLOSE - HOLD IT NEXT TO YOUR DEVICE", CRGB(255, 200, 0));
      }
    }
    if (findScroll.draw()) return true;
    // "Tap to lock on": a dot with a ring breathing around it.
    uint8_t v = beatsin8(40, 30, 200);
    drawSpriteTint(SPR_RING, 0, 0, CRGB(0, v / 3, v));
    px(2, 2, CRGB::White);
    return true;
  }
  if (e == EV_TAP && tiltDir == T_LEFT) {
    finderClear();
    findScroll.start("PICK AGAIN", CRGB(255, 200, 0));
    return true;
  }
  bool fresh = findSeenAt && now - findSeenAt < 5000;
  if (e == EV_TAP) {
    char buf[48];
    if (!fresh) snprintf(buf, sizeof(buf), "NOT HEARD");
    else snprintf(buf, sizeof(buf), "%d DB %s", (int)findRssi, findTrend > 0.3f ? "WARMER" : findTrend < -0.3f ? "COLDER" : "");
    findScroll.start(buf, CRGB::White);
  }
  if (findScroll.draw()) return true;
  if (!fresh) {                                         // lost: a slow grey question mark
    if ((now / 700) % 2) drawGlyph('?', 1, 0, CRGB(60, 60, 80));
    return true;
  }
  // -90 dBm = 0 (cold), -40 dBm = 1 (on top of it).
  float heat = constrain((findRssi + 90) / 50.0f, 0.0f, 1.0f);
  uint8_t hue = 160 - (uint8_t)(heat * 160);           // blue -> green -> yellow -> red
  uint16_t period = 1200 - heat * 1000;                 // the pulse quickens as you get close
  uint8_t pulse = 255 - (uint8_t)(((now % period) * 200) / period);
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 5; x++) {
      int ring = max(abs(x - 2), abs(y - 2));           // 0 = centre, 1, 2 = edge
      float need = ring == 0 ? 0 : ring == 1 ? 0.35f : 0.7f;
      if (heat >= need) px(x, y, CRGB(CHSV(hue, 255, ring == 0 ? 255 : pulse)));
      else if (ring == 2) px(x, y, CRGB(6, 6, 12));
    }
  return true;
}
