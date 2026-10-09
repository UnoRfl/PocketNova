#pragma once
// =====================================================================
//  Wifi.h - joining your Wi-Fi, and the setup page that gets it there
// =====================================================================
//  The ESP32's Wi-Fi radio can work in two roles, even at the same time:
//
//    STATION (STA)       a normal client, like your phone: it joins your
//                        home network and gets an address from the router.
//    ACCESS POINT (AP)   it IS a network: it broadcasts its own name
//                        (SSID), hands out addresses (DHCP) and others join.
//
//  SETUP: Settings -> WI-FI, double tap (or the PC panel) starts an access
//  point called "NOVA-XXXX" with a fresh 8-digit password. Join it with
//  your phone and the setup page opens by itself (a "captive portal").
//  Pick your home network, type its password, and Pocket Nova joins it.
//
//  HOW THE PAGE OPENS BY ITSELF: when a phone joins any Wi-Fi it fetches
//  a test address (Android: connectivitycheck.gstatic.com/generate_204,
//  iPhone: captive.apple.com, Windows: www.msftconnecttest.com). Our
//  little DNS server answers EVERY name with our own address, 192.168.4.1,
//  so the phone gets our page instead of the expected reply. It decides
//  "this network needs a sign-in" and shows the page. Hotels do the same.
//
//  ONLINE: once joined it fetches the time from the internet (NTP), so
//  Nova knows when it's night even on a phone charger.
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>

extern int32_t tzOffsetSec;   // Pet.h

enum WifiState : uint8_t { WF_OFF, WF_CONNECTING, WF_ONLINE, WF_FAILED };
const char* const WIFI_STATE_NAMES[] = {"off", "connecting", "online", "failed"};

const uint32_t WIFI_SETUP_MS   = 10UL * 60 * 1000;   // setup network closes after 10 min
const uint32_t WIFI_JOIN_MS    = 20000;              // give up joining after 20 s
const uint32_t WIFI_RETRY_MS   = 2UL * 60 * 1000;    // then try again every 2 min

char      wifiSsid[33] = "";      // your home network (max 32 characters, a Wi-Fi rule)
char      wifiPass[65] = "";      // its password (WPA2 allows 8..63, or 64 hex digits)
WifiState wifiState = WF_OFF;
uint8_t   wifiFailReason = 0;     // the router's last "no" (802.11 reason code, see wifiReason)
uint8_t   wifiRefusals = 0;       // how many times it said "no" this attempt
uint32_t  wifiJoinAt = 0, wifiFailedAt = 0;
bool      ntpStarted = false;

bool        setupOn = false;
uint32_t    setupUntil = 0;
char        apSsid[12] = "";       // NOVA-XXXX
char        apPass[9] = "";        // 8 random digits
WebServer*  web = nullptr;
DNSServer*  dns = nullptr;
bool        setupJoining = false;  // the page asked us to try a network
uint32_t    setupDoneAt = 0;       // close the setup network a little after success

// ---- saved network (its own NVS area, apart from the other settings) ----
// Note: NVS isn't encrypted on this board, so anyone with a USB cable and
// the right tools could read the password back. Fine for a home gadget;
// real products turn on "flash encryption".
void wifiLoad() {
  prefs.begin("wifi", true);
  prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  prefs.end();
}
void wifiSave() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}
bool wifiHasNetwork() { return wifiSsid[0] != 0; }

// ---------------------------------------------------------------------
//  WHY A JOIN FAILED. Each time a router turns a device away, the Wi-Fi
//  chip gets a numbered "reason code". 1..68 come from the Wi-Fi standard
//  (IEEE 802.11) and are sent by the router; 200+ are the ESP32's own.
//    15 / 204  the password handshake never finished: almost always a
//              wrong password (the router stays silent rather than say so)
//    201       no network with that name on 2.4 GHz
//    2 / 200   the router stopped answering: weak signal
// ---------------------------------------------------------------------
const char* wifiReason() {
  switch (wifiFailReason) {
    case 0:   return "no answer from the router";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_MIC_FAILURE:   return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:   return "network not found on 2.4 GHz";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL: return "weak signal, move closer to the router";
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_FAIL:    return "the router refused (device limit or blocked list?)";
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_BAD_CIPHER_OR_AKM:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
    case WIFI_REASON_GROUP_CIPHER_INVALID:
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "security type not supported (WPA3 only?)";
    default:  return "the router said no";
  }
}

// The Wi-Fi chip reports events (connected, got an address, disconnected
// with a reason...) from its own task. We just note the reason here.
void wifiOnDisconnect(WiFiEvent_t, WiFiEventInfo_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (wifiState != WF_CONNECTING) return;
  wifiFailReason = r;
  wifiRefusals++;
  Serial.printf("[WIFI] Router said no: reason %u (%s)\n", r, WiFi.disconnectReasonName((wifi_err_reason_t)r));
}

// What the last scan saw of the network we're about to join.
void wifiLogTarget() {
  int n = WiFi.scanComplete();
  const char* const AUTH[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-Enterprise", "WPA3", "WPA2/WPA3", "WAPI"};
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != wifiSsid) continue;
    int a = WiFi.encryptionType(i);
    Serial.printf("[WIFI] Seen: channel %d, signal %d dBm, security %s, router %s\n", WiFi.channel(i), WiFi.RSSI(i),
                  a < 9 ? AUTH[a] : "?", WiFi.BSSIDstr(i).c_str());
    found = true;
  }
  if (n >= 0 && !found) Serial.println("[WIFI] Not in the last scan (5 GHz only, hidden, or out of range?)");
  size_t len = strlen(wifiPass);
  Serial.printf("[WIFI] Password: %u characters%s\n", (unsigned)len,
                len && (wifiPass[0] == ' ' || wifiPass[len - 1] == ' ') ? ", starts or ends with a SPACE" : "");
}

String wifiIp() { return wifiState == WF_ONLINE ? WiFi.localIP().toString() : String(""); }

// ---- station: join the saved network ----
void wifiJoin() {
  if (!wifiHasNetwork()) return;
  static bool hooked = false;
  if (!hooked) { WiFi.onEvent(wifiOnDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED); hooked = true; }
  wifiLogTarget();
  wifiFailReason = 0;
  wifiRefusals = 0;
  WiFi.begin(wifiSsid, wifiPass);
  wifiState = WF_CONNECTING;
  wifiJoinAt = millis();
  Serial.printf("[WIFI] Joining \"%s\"\n", wifiSsid);
}

void wifiRadioFor(bool sta, bool ap) {
  WiFi.mode(sta && ap ? WIFI_AP_STA : ap ? WIFI_AP : sta ? WIFI_STA : WIFI_OFF);
}

// ---------------------------------------------------------------------
//  The setup page. One small HTML file kept in flash (PROGMEM). Its
//  script asks /scan for nearby networks and /result for progress, both
//  answered as JSON.
// ---------------------------------------------------------------------
const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket Nova Wi-Fi</title><style>
:root{--bg:#f3f2f7;--card:#fff;--fg:#1c1b24;--mu:#5d5a6e;--ln:#dcdae6;--ac:#7a2cf0;--bad:#c62828;--ok:#1b8a3a}
@media(prefers-color-scheme:dark){:root{--bg:#121118;--card:#1b1a23;--fg:#ecebf3;--mu:#a29fb5;--ln:#2f2d3b;--ac:#b48cff;--bad:#ff6b6b;--ok:#4cd17a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:24px 16px;display:grid;gap:16px}
h1{margin:0;font-size:1.5rem}p{margin:0}.mu{color:var(--mu);font-size:.9rem}
.card{background:var(--card);border:1px solid var(--ln);border-radius:12px;padding:16px;display:grid;gap:12px}
.net{display:flex;justify-content:space-between;gap:8px;width:100%;padding:12px;border:1px solid var(--ln);border-radius:8px;background:none;color:inherit;font:inherit;text-align:left;cursor:pointer}
.net[aria-pressed=true]{border-color:var(--ac);outline:2px solid var(--ac)}
input{width:100%;padding:12px;border:1px solid var(--ln);border-radius:8px;background:var(--bg);color:inherit;font:inherit}
button.go{padding:12px;border:0;border-radius:8px;background:var(--ac);color:#fff;font:inherit;font-weight:700;cursor:pointer}
.bars{font-family:monospace;color:var(--mu)}#msg.bad{color:var(--bad)}#msg.ok{color:var(--ok)}
label{display:grid;gap:4px;font-size:.9rem}
</style></head><body><main>
<h1>Pocket Nova Wi-Fi</h1>
<p class="mu">Pick your home network and type its password. Pocket Nova only uses it to get the time from the internet.</p>
<div class="card"><div id="list" class="mu">Looking for networks...</div>
<button class="net" type="button" id="rescan" style="justify-content:center">Search again</button></div>
<form class="card" id="f">
<label>Network name<input id="ssid" maxlength="32" autocomplete="off" required></label>
<label>Password<input id="pass" type="password" maxlength="64" autocomplete="off"></label>
<button class="go">Connect</button><p id="msg" role="status"></p></form>
</main><script>
const $=id=>document.getElementById(id);
function bars(r){return r>-55?"||||":r>-67?"|||.":r>-78?"||..":"|..."}
async function scan(again){
 const r=await fetch("/scan"+(again?"?again=1":"")).then(r=>r.json()).catch(()=>null);
 if(!r||r.busy){setTimeout(scan,1200);return}
 const L=$("list");L.textContent="";
 if(!r.list.length){L.textContent="No networks found. Type the name instead.";return}
 r.list.forEach(n=>{const b=document.createElement("button");b.type="button";b.className="net";
  b.setAttribute("aria-pressed","false");
  b.innerHTML="<span></span><span class=bars></span>";b.children[0].textContent=n.ssid+(n.open?" (open)":"");
  b.children[1].textContent=bars(n.rssi)+" "+n.rssi+" dBm";
  b.onclick=()=>{[...L.children].forEach(x=>x.setAttribute("aria-pressed","false"));b.setAttribute("aria-pressed","true");$("ssid").value=n.ssid;$("pass").focus()};
  L.appendChild(b)})}
$("rescan").onclick=()=>{$("list").textContent="Looking for networks...";scan(true)};
$("f").onsubmit=async e=>{e.preventDefault();const m=$("msg");m.className="";m.textContent="Connecting...";
 const body=new URLSearchParams({ssid:$("ssid").value,pass:$("pass").value,tz:-new Date().getTimezoneOffset()*60});
 await fetch("/save",{method:"POST",body}).catch(()=>{});poll()};
async function poll(){const r=await fetch("/result").then(r=>r.json()).catch(()=>null);const m=$("msg");
 if(!r||r.state=="connecting"){setTimeout(poll,1000);return}
 if(r.state=="online"){m.className="ok";m.textContent="Connected! Pocket Nova's address on your network is "+r.ip+". This setup network closes in a few seconds.";return}
 m.className="bad";m.textContent="Couldn't join: "+r.reason+". Check the name and password, then try again."}
scan(false);
</script></body></html>)HTML";

// Every address we don't know (the phone's connectivity checks included)
// is sent to our page. That redirect is what triggers the sign-in pop-up.
void webRedirect() {
  web->sendHeader("Location", "http://192.168.4.1/", true);
  web->send(302, "text/plain", "");
}

void webScan() {
  if (web->hasArg("again")) { WiFi.scanDelete(); WiFi.scanNetworks(true); }
  int n = WiFi.scanComplete();          // -1 = still scanning, -2 = not started
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); n = WIFI_SCAN_RUNNING; }
  JsonDocument d;
  if (n < 0) {
    d["busy"] = true;
  } else {
    JsonArray a = d["list"].to<JsonArray>();
    // The same network can show up once per router/access point: keep the
    // strongest of each name. The scan list is already sorted strongest first.
    for (int i = 0; i < n && a.size() < 20; i++) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;        // hidden networks don't broadcast a name
      bool dup = false;
      for (JsonObject o : a) if (s == o["ssid"].as<const char*>()) { dup = true; break; }
      if (dup) continue;
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = s;
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
  }
  String out;
  serializeJson(d, out);
  web->send(200, "application/json", out);
}

void webSave() {
  String s = web->arg("ssid"), p = web->arg("pass");
  if (s.isEmpty() || s.length() > 32 || p.length() > 64) { web->send(400, "text/plain", "bad"); return; }
  strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
  strncpy(wifiPass, p.c_str(), sizeof(wifiPass) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = 0;
  wifiPass[sizeof(wifiPass) - 1] = 0;
  if (web->hasArg("tz")) {               // the phone's time zone, so night mode is right
    tzOffsetSec = web->arg("tz").toInt();
    cfg.tz = tzOffsetSec;
  }
  cfg.wifiOn = true;
  saveConfig();
  wifiSave();
  web->send(200, "application/json", "{\"ok\":true}");
  // ONE RADIO, ONE CHANNEL: the ESP32 has a single radio, so its own setup
  // network and your router must share a channel. If they differ, the radio
  // hops back and forth and the join times out (seen on a TP-Link at -76 dBm).
  // So move the setup network onto the router's channel first. The phone
  // drops for a second and rejoins by itself; the page just keeps asking.
  int ch = 0;
  for (int i = 0, n = WiFi.scanComplete(); i < n; i++)
    if (WiFi.SSID(i) == wifiSsid) { ch = WiFi.channel(i); break; }
  if (ch && ch != WiFi.channel()) {
    Serial.printf("[WIFI] Moving the setup network to channel %d (the router's)\n", ch);
    WiFi.softAP(apSsid, apPass, ch);
  }
  setupJoining = true;
  wifiJoin();                            // AP stays up meanwhile (AP+STA mode)
}

void webResult() {
  JsonDocument d;
  d["state"] = WIFI_STATE_NAMES[wifiState];
  d["ip"] = wifiIp();
  d["reason"] = wifiReason();
  String out;
  serializeJson(d, out);
  web->send(200, "application/json", out);
}

void wifiStartSetup() {
  if (setupOn) { setupUntil = millis() + WIFI_SETUP_MS; return; }
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(apSsid, sizeof(apSsid), "NOVA-%02X%02X", mac[4], mac[5]);
  // A new password each time. esp_random() uses radio noise, so it's
  // properly unpredictable while Wi-Fi or Bluetooth is running.
  for (int i = 0; i < 8; i++) apPass[i] = '0' + esp_random() % 10;
  apPass[8] = 0;

  WiFi.persistent(false);                // we save the network ourselves (above)
  wifiRadioFor(wifiHasNetwork() && cfg.wifiOn, true);
  WiFi.softAP(apSsid, apPass);           // WPA2: the form is encrypted over the air
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());  // "*" = answer every name with our address
  web = new WebServer(80);
  web->on("/", HTTP_GET, [] {
    if (web->hostHeader() != "192.168.4.1") { webRedirect(); return; }
    web->send_P(200, "text/html", SETUP_PAGE);
  });
  web->on("/scan", HTTP_GET, webScan);
  web->on("/save", HTTP_POST, webSave);
  web->on("/result", HTTP_GET, webResult);
  web->onNotFound(webRedirect);
  web->begin();
  WiFi.scanNetworks(true);               // true = in the background; /scan collects it

  setupOn = true;
  setupJoining = false;
  setupDoneAt = 0;
  setupUntil = millis() + WIFI_SETUP_MS;
  Serial.printf("[WIFI] Setup network \"%s\", password %s, page http://192.168.4.1/\n", apSsid, apPass);
}

void wifiStopSetup() {
  if (!setupOn) return;
  web->stop();
  delete web; web = nullptr;
  dns->stop();
  delete dns; dns = nullptr;
  WiFi.softAPdisconnect(true);
  setupOn = false;
  bool sta = cfg.wifiOn && wifiHasNetwork();
  wifiRadioFor(sta, false);
  if (!sta) wifiState = WF_OFF;
  else if (wifiState == WF_OFF || wifiState == WF_FAILED) wifiJoin();   // radio is free now: try at once
  Serial.println("[WIFI] Setup network closed");
}

// ---- switches used by Settings and the PC panel ----
void wifiSetOn(bool on) {
  cfg.wifiOn = on;
  saveConfig();
  if (setupOn) { if (on) wifiJoin(); return; }   // AP stays up; wifiStopSetup() fixes the mode
  if (on && wifiHasNetwork()) { WiFi.persistent(false); wifiRadioFor(true, false); wifiJoin(); }
  else { WiFi.disconnect(true); wifiRadioFor(false, false); wifiState = WF_OFF; }
}

void wifiForget() {
  wifiSsid[0] = wifiPass[0] = 0;
  wifiSave();
  WiFi.disconnect(true);
  if (!setupOn) wifiRadioFor(false, false);
  wifiState = WF_OFF;
  Serial.println("[WIFI] Forgot the saved network");
}

void wifiBegin() {
  wifiLoad();
  if (cfg.wifiOn && wifiHasNetwork()) {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    wifiRadioFor(true, false);
    wifiJoin();
  } else {
    WiFi.mode(WIFI_OFF);                 // radio fully off: saves power and memory
  }
}

void wifiUpdate() {
  uint32_t now = millis();
  if (setupOn) {
    dns->processNextRequest();
    web->handleClient();
    if (setupDoneAt && now - setupDoneAt > 15000) wifiStopSetup();   // let the page show "Connected!"
    else if ((int32_t)(now - setupUntil) > 0) wifiStopSetup();
  }

  wl_status_t st = WiFi.status();
  switch (wifiState) {
    case WF_CONNECTING:
      if (st == WL_CONNECTED) {
        wifiState = WF_ONLINE;
        Serial.printf("[WIFI] Online: %s, signal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        if (!ntpStarted) {               // NTP = Network Time Protocol (UDP port 123)
          configTime(0, 0, "pool.ntp.org", "time.google.com");   // UTC; Nova adds tzOffsetSec
          ntpStarted = true;
        }
        if (setupOn && setupJoining) setupDoneAt = now;
      } else if (now - wifiJoinAt > WIFI_JOIN_MS || wifiRefusals >= 3) {   // 3 "no"s = it won't change its mind
        wifiState = WF_FAILED;
        wifiFailedAt = now;
        setupJoining = false;
        WiFi.disconnect();
        Serial.printf("[WIFI] Couldn't join \"%s\": %s\n", wifiSsid, wifiReason());
      }
      break;
    case WF_ONLINE:
      if (st != WL_CONNECTED) {          // router restarted, walked out of range...
        wifiState = WF_CONNECTING;
        wifiJoinAt = now;
        Serial.println("[WIFI] Lost the network, reconnecting");
      }
      break;
    case WF_FAILED:
      if (!setupOn && now - wifiFailedAt > WIFI_RETRY_MS && cfg.wifiOn) wifiJoin();
      break;
    default:
      break;
  }
}
