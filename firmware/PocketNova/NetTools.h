#pragma once
// =====================================================================
//  NetTools.h - Wi-Fi channel analyzer and network health monitor
// =====================================================================
//  CHANNELS: 2.4 GHz Wi-Fi is cut into 13 channels only 5 MHz apart, but
//  each network is 20 MHz wide, so it spills over ~2 channels on each
//  side. That's why 1, 6 and 11 are the classic picks: they don't touch.
//  A scan lists every network nearby with its channel and signal; adding
//  up how loud each channel is (spill-over included) shows where your
//  router would have the least company.
//
//  HEALTH: a "ping" (ICMP echo) is a tiny packet that says "answer me";
//  the time to the answer is the latency. Pinging two places tells you
//  WHERE a problem is:
//    the router (your own Wi-Fi)       slow or lost = Wi-Fi trouble
//    the internet (1.1.1.1 by default) slow or lost but router fine =
//                                      your internet provider
// =====================================================================

#include <ping/ping_sock.h>
#include <lwip/inet.h>
#include <ESPmDNS.h>

void alertShow(const char* kind, const char* msg);   // main sketch

// ---------------------------------------------------------------------
//  LOOKING NAMES UP without freezing. Turning "google.com" into an
//  address (DNS) can take up to 15 s when the internet is down: as long
//  as the watchdog allows the main loop. So lookups run in a little task
//  of their own and the loop just checks back. One at a time.
//  Names ending in ".local" (homeassistant.local) aren't in DNS at all:
//  devices answer for themselves on the local network (mDNS).
// ---------------------------------------------------------------------
struct Lookup {
  char      name[64] = "";
  IPAddress ip;
  volatile uint8_t state = 0;         // 0 = not started, 1 = looking, 2 = found, 3 = failed
};
volatile bool lookupBusy = false;
bool mdnsUp = false;

void lookupTask(void* arg) {
  Lookup* l = (Lookup*)arg;
  IPAddress r;
  bool ok;
  size_t n = strlen(l->name);
  if (n > 6 && !strcasecmp(l->name + n - 6, ".local")) {
    String h(l->name);
    r = MDNS.queryHost(h.substring(0, n - 6), 3000);
    ok = (uint32_t)r != 0;
  } else {
    ok = WiFi.hostByName(l->name, r) == 1;
  }
  l->ip = r;
  l->state = ok ? 2 : 3;
  lookupBusy = false;
  vTaskDelete(nullptr);
}

// Starts a lookup. Returns false if another one is running (try later).
bool lookupStart(Lookup& l, const char* name) {
  strncpy(l.name, name, sizeof(l.name) - 1);
  l.name[sizeof(l.name) - 1] = 0;
  if (l.ip.fromString(l.name)) { l.state = 2; return true; }   // already a number: nothing to look up
  if (lookupBusy) return false;
  lookupBusy = true;
  l.state = 1;
  if (xTaskCreate(lookupTask, "lookup", 4096, &l, 1, nullptr) != pdPASS) { lookupBusy = false; l.state = 3; }
  return true;
}

// Pocket Nova answers to "<router name>.local" too, once it's online.
void mdnsUpdate() {
  if (wifiState == WF_ONLINE && !mdnsUp) mdnsUp = MDNS.begin(wifiHostname);
  else if (wifiState != WF_ONLINE && mdnsUp) { MDNS.end(); mdnsUp = false; }
}

// ---------------------------------------------------------------------
//  Channel analyzer
// ---------------------------------------------------------------------
struct ChanLoad { uint8_t nets; float load; };
ChanLoad chans[14];               // [1..13]
bool     chScanning = false, chHave = false, chStaAdded = false;
uint8_t  chBest = 0, chBestAny = 0, chTotal = 0;
uint32_t chScanAt = 0;

// How much of a network on channel c spills onto channel c+d (20 MHz wide, 5 MHz steps).
float chanOverlap(int d) { d = abs(d); return d >= 4 ? 0 : (4 - d) / 4.0f; }

bool chanScanStart() {
  if (chScanning) return true;
  if (!(WiFi.getMode() & WIFI_MODE_STA)) { WiFi.enableSTA(true); chStaAdded = true; }   // scanning needs the client side
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true, true) == WIFI_SCAN_FAILED) return false;   // async, include hidden networks
  chScanning = true;
  chScanAt = millis();
  Serial.println("[CHAN] Scanning...");
  return true;
}

void chanScanUpdate() {
  if (!chScanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - chScanAt > 15000) { chScanning = false; WiFi.scanDelete(); }   // gave up
    return;
  }
  chScanning = false;
  if (n < 0) return;
  memset(chans, 0, sizeof(chans));
  chTotal = 0;
  for (int i = 0; i < n; i++) {
    int c = WiFi.channel(i);
    if (c < 1 || c > 13) continue;                    // 5 GHz networks don't share our band
    chans[c].nets++;
    chTotal++;
    // A loud neighbour (-45 dBm) counts 1, a faint one (-90 dBm) almost nothing.
    float w = constrain((WiFi.RSSI(i) + 95) / 50.0f, 0.05f, 1.0f);
    for (int d = -3; d <= 3; d++)
      if (c + d >= 1 && c + d <= 13) chans[c + d].load += w * chanOverlap(d);
  }
  chBest = 1;
  for (int c : {6, 11}) if (chans[c].load < chans[chBest].load) chBest = c;
  chBestAny = 1;
  for (int c = 2; c <= 13; c++) if (chans[c].load < chans[chBestAny].load) chBestAny = c;
  chHave = true;
  Serial.printf("[CHAN] %d networks, quietest of 1/6/11: channel %d\n", chTotal, chBest);
  // Turn the client side back off if only the scan needed it (and nothing else uses it now).
  if (chStaAdded && !(cfg.wifiOn && wifiHasNetwork())) {
    chStaAdded = false;
    WiFi.enableSTA(false);
  }
}

// Every network from the last scan, for the panel.
void chanList(JsonArray a) {
  int n = WiFi.scanComplete();
  for (int i = 0; i < n && a.size() < 24; i++) {
    int c = WiFi.channel(i);
    if (c < 1 || c > 13) continue;
    JsonObject o = a.add<JsonObject>();
    String s = WiFi.SSID(i);
    o["ssid"] = s.isEmpty() ? "(hidden)" : s;
    o["rssi"] = WiFi.RSSI(i);
    o["ch"] = c;
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
}

// ---------------------------------------------------------------------
//  Health monitor
// ---------------------------------------------------------------------
const uint8_t NET_HIST = 30;          // samples kept (one pair every 5 s = 2.5 minutes)
bool     nmOn = true;                 // settings (NVS "net")
char     nmTarget[64] = "1.1.1.1";
uint16_t nmSlowMs = 150;
bool     nmAlert = true;

int16_t  nmRouter[NET_HIST], nmNet[NET_HIST];   // ms, -1 = no answer, -2 = not measured
uint8_t  nmHead = 0, nmCount = 0;
bool     nmInternetUp = true, nmWasUp = false;
uint32_t nmNextAt = 0;
ip_addr_t nmNetIp;
bool     nmNetIpOk = false;
uint32_t nmResolveAt = 0;
Lookup   nmLookup;

esp_ping_handle_t nmPing = nullptr;
volatile bool  nmDone = false;
volatile int   nmMs = -1;
uint8_t  nmStep = 0;                  // 0 = idle, 1 = pinging router, 2 = pinging internet
int16_t  nmRouterNow = -2;

void nmLoad() {
  prefs.begin("net", true);
  nmOn = prefs.getBool("on", true);
  if (prefs.isKey("target")) prefs.getString("target", nmTarget, sizeof(nmTarget));
  nmSlowMs = prefs.getUShort("slow", 150);
  nmAlert = prefs.getBool("alert", true);
  prefs.end();
  for (auto& v : nmRouter) v = -2;
  for (auto& v : nmNet) v = -2;
}
void nmSave() {
  prefs.begin("net", false);
  prefs.putBool("on", nmOn);
  prefs.putString("target", nmTarget);
  prefs.putUShort("slow", nmSlowMs);
  prefs.putBool("alert", nmAlert);
  prefs.end();
  nmNetIpOk = false;                  // look the target up again
  nmResolveAt = 0;
}

// The ping runs in its own little task; these callbacks run there.
void nmOk(esp_ping_handle_t h, void*) {
  uint32_t t;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
  nmMs = t;
}
void nmTimeout(esp_ping_handle_t, void*) { nmMs = -1; }
void nmEnd(esp_ping_handle_t, void*) { nmDone = true; }

bool nmPingStart(const ip_addr_t& ip) {
  esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
  c.target_addr = ip;
  c.count = 1;
  c.timeout_ms = 1000;
  esp_ping_callbacks_t cb = {};
  cb.on_ping_success = nmOk;
  cb.on_ping_timeout = nmTimeout;
  cb.on_ping_end = nmEnd;
  nmMs = -1;
  nmDone = false;
  if (esp_ping_new_session(&c, &cb, &nmPing) != ESP_OK) { nmPing = nullptr; return false; }
  esp_ping_start(nmPing);
  return true;
}

void nmPingFinish() {
  if (nmPing) { esp_ping_delete_session(nmPing); nmPing = nullptr; }
}

// Status words the panel and the LEDs use.
int16_t nmLast(int16_t* h) { return nmCount ? h[(nmHead + NET_HIST - 1) % NET_HIST] : -2; }
uint8_t nmLost(int16_t* h, uint8_t last) {           // misses among the last few samples
  uint8_t n = 0;
  for (uint8_t i = 0; i < min(last, nmCount); i++) if (h[(nmHead + NET_HIST - 1 - i) % NET_HIST] == -1) n++;
  return n;
}
const char* nmState(int16_t* h) {
  if (!nmOn || wifiState != WF_ONLINE || !nmCount) return "off";
  if (nmLost(h, 3) >= 3) return "down";
  int16_t v = nmLast(h);
  return v < 0 || v > nmSlowMs || nmLost(h, 10) >= 2 ? "slow" : "good";
}

void nmRecord(int16_t routerMs, int16_t netMs) {
  nmRouter[nmHead] = routerMs;
  nmNet[nmHead] = netMs;
  nmHead = (nmHead + 1) % NET_HIST;
  if (nmCount < NET_HIST) nmCount++;
  bool up = strcmp(nmState(nmNet), "down") != 0;
  if (up != nmInternetUp) {
    nmInternetUp = up;
    Serial.printf("[NET] Internet %s\n", up ? "back" : "down");
    if (nmAlert && (nmWasUp || up)) alertShow(up ? "netup" : "netdown", up ? "INTERNET BACK" : "INTERNET DOWN");
  }
  if (up) nmWasUp = true;
}

void netMonitorUpdate() {
  uint32_t now = millis();
  if (!nmOn || wifiState != WF_ONLINE) {
    if (nmPing && nmDone) { nmPingFinish(); nmStep = 0; }
    return;
  }
  // The internet target can be a name (google.com): look it up in the
  // background (see Lookup above), again every 10 minutes in case it moves.
  if (nmLookup.state == 2 || nmLookup.state == 3) {
    if (nmLookup.state == 2) {
      ip4_addr_t b;
      b.addr = (uint32_t)nmLookup.ip;
      ip_addr_copy_from_ip4(nmNetIp, b);
      nmNetIpOk = true;
    }
    nmLookup.state = 0;
  }
  if (nmLookup.state == 0 && (int32_t)(now - nmResolveAt) >= 0) {
    if (lookupStart(nmLookup, nmTarget)) nmResolveAt = now + (nmNetIpOk ? 600000 : 30000);   // found: 10 min, not yet: 30 s
  }
  switch (nmStep) {
    case 0:
      if ((int32_t)(now - nmNextAt) < 0) return;
      nmNextAt = now + 5000;
      {
        ip4_addr_t g;
        g.addr = (uint32_t)WiFi.gatewayIP();
        ip_addr_t gw;
        ip_addr_copy_from_ip4(gw, g);
        if (nmPingStart(gw)) nmStep = 1;
      }
      return;
    case 1:
      if (!nmDone) return;
      nmRouterNow = nmMs;
      nmPingFinish();
      if (nmNetIpOk && nmPingStart(nmNetIp)) { nmStep = 2; return; }
      nmRecord(nmRouterNow, -1);             // can't even look the name up: count as no answer
      nmStep = 0;
      return;
    case 2:
      if (!nmDone) return;
      nmPingFinish();
      nmRecord(nmRouterNow, nmMs);
      nmStep = 0;
      return;
  }
}

void netHistory(JsonObject o) {
  JsonArray r = o["router"].to<JsonArray>(), n = o["net"].to<JsonArray>();
  for (uint8_t i = 0; i < nmCount; i++) {
    uint8_t k = (nmHead + NET_HIST - nmCount + i) % NET_HIST;
    r.add(nmRouter[k]);
    n.add(nmNet[k]);
  }
}

CRGB netColor(int16_t ms) {
  if (ms == -1) return CRGB(255, 0, 0);
  if (ms < 0) return CRGB(20, 20, 30);
  if (ms <= nmSlowMs / 2) return CRGB(0, 255, 80);
  if (ms <= nmSlowMs) return CRGB(255, 200, 0);
  return CRGB(255, 90, 0);
}

// ---------------------------------------------------------------------
//  NETWORK app. Top row = the router, a bar per sample; the 4 rows under
//  it = the internet: one column per sample (newest on the right), taller
//  and warmer = slower, a full red column = no answer.
//  Tap = read the numbers out.
// ---------------------------------------------------------------------
Scroller netScroll;

void netEnter() { netScroll.stop(); }

bool netFrame(Event e) {
  if (e == EV_HOLD) return false;
  clearFb();
  if (wifiState != WF_ONLINE || !nmOn) {
    if (!netScroll.draw())
      netScroll.start(!nmOn ? "MONITOR OFF - TURN ON IN PANEL" : "NOT ON WI-FI - JOIN IN PANEL", CRGB(255, 200, 0));
    return true;
  }
  if (e == EV_TAP) {
    char buf[64];
    int16_t r = nmLast(nmRouter), n = nmLast(nmNet);
    char rs[12], ns[12];
    if (r >= 0) snprintf(rs, sizeof(rs), "%dMS", r); else strcpy(rs, r == -1 ? "NO ANSWER" : "-");
    if (n >= 0) snprintf(ns, sizeof(ns), "%dMS", n); else strcpy(ns, n == -1 ? "DOWN" : "-");
    snprintf(buf, sizeof(buf), "ROUTER %s  INTERNET %s", rs, ns);
    netScroll.start(buf, CRGB::White);
  }
  if (netScroll.draw()) return true;
  for (int x = 0; x < 5; x++) {
    int k = (int)nmCount - 5 + x;                      // the last 5 samples
    if (k < 0) { px(x, 0, CRGB(10, 10, 16)); continue; }
    uint8_t i = (nmHead + NET_HIST - nmCount + k) % NET_HIST;
    px(x, 0, netColor(nmRouter[i]));
    int16_t ms = nmNet[i];
    int h = ms == -1 ? 4 : ms < 0 ? 0 : ms <= 30 ? 1 : ms <= nmSlowMs / 2 ? 2 : ms <= nmSlowMs ? 3 : 4;
    for (int y = 0; y < 4; y++) px(x, 4 - y, y < h ? netColor(ms) : CRGB(8, 8, 14));
  }
  if (nmStep && (millis() / 200) % 2) addPx(4, 4, CRGB(40, 40, 60));   // a ping is out right now
  return true;
}

// ---------------------------------------------------------------------
//  CHANNELS app. 5 channels at a time as bars (taller = busier), tilt to
//  see the others (1-5, 5-9, 9-13). The quietest of 1/6/11 blinks white
//  on top; your own router's channel has a cyan floor. Tap = scan again.
// ---------------------------------------------------------------------
Scroller chScroll;
uint8_t  chPage = 0;                  // first channel shown = 1 + page * 4

void chanEnter() { chPage = 0; chScroll.stop(); chanScanStart(); }

bool chanFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  static bool wasScanning = false;              // the main loop collects the scan; notice when it ends
  if (wasScanning && !chScanning && chHave) {
    char buf[32];
    snprintf(buf, sizeof(buf), "BEST CH %d", chBest);
    chScroll.start(buf, CRGB(0, 255, 80));
  }
  wasScanning = chScanning;
  if (e == EV_TAP) { chScroll.stop(); chanScanStart(); }
  if (e == EV_RIGHT && chPage < 2) chPage++;
  if (e == EV_LEFT && chPage > 0) chPage--;
  clearFb();
  if (chScanning) {                                   // a sweeping column while it listens
    int x = (now / 120) % 5;
    for (int y = 0; y < 5; y++) px(x, y, CRGB(0, 60 + y * 30, 120));
    return true;
  }
  if (chScroll.draw()) return true;
  if (!chHave) { drawSprite(SPR_WIFI); return true; }
  int mine = wifiState == WF_ONLINE ? WiFi.channel() : 0;
  float top = 0.5f;
  for (int c = 1; c <= 13; c++) top = max(top, chans[c].load);
  for (int x = 0; x < 5; x++) {
    int c = 1 + chPage * 4 + x;
    if (c > 13) break;
    int h = (int)ceilf(chans[c].load / top * 5);
    if (chans[c].load > 0 && h == 0) h = 1;
    for (int y = 0; y < 5; y++) {
      CRGB col = y < h ? (h <= 1 ? CRGB(0, 255, 80) : h <= 3 ? CRGB(255, 200, 0) : CRGB(255, 40, 0)) : CRGB::Black;
      if (y >= h && c == mine && y == 0) col = CRGB(0, 80, 90);
      px(x, 4 - y, col);
    }
    if (c == chBest && (now / 300) % 2) px(x, 4 - min(h, 4), CRGB::White);
  }
  return true;
}
