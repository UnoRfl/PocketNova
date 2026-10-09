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
//  point. Its name and password are yours to pick in the PC panel; left
//  empty, it's "<Pocket Nova's name> Setup" with a fresh 8-digit password
//  each time. Join it with
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
//
//  NAME: your router lists Pocket Nova by its "hostname": the router name
//  you pick in the panel, or else Pocket Nova's name (see wifiMakeHostname).
//
//  WHO'S ON YOUR NETWORK: the PC panel can ask Pocket Nova to list every
//  device on your home network (see netScanStart below).
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_netif_sta_list.h>
#include <lwip/etharp.h>
#include <lwip/tcpip.h>

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
char        apSsid[33] = "";       // the setup network's name now (Wi-Fi names max out at 32)
char        apPass[64] = "";       // and its password (WPA2: 8..63 characters)

// Your choices from the PC panel. Empty = automatic.
char        myApName[33] = "";     // setup network name   (auto: "<name> Setup")
char        myApPass[64] = "";     // setup password       (auto: 8 random digits each time)
char        myHost[32] = "";       // name on your router  (auto: made from Pocket Nova's name)
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
  if (prefs.isKey("apName")) prefs.getString("apName", myApName, sizeof(myApName));
  if (prefs.isKey("apPass")) prefs.getString("apPass", myApPass, sizeof(myApPass));
  if (prefs.isKey("host")) prefs.getString("host", myHost, sizeof(myHost));
  prefs.end();
}
void wifiSave() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("apName", myApName);
  prefs.putString("apPass", myApPass);
  prefs.putString("host", myHost);
  prefs.end();
}
bool wifiHasNetwork() { return wifiSsid[0] != 0; }

// ---------------------------------------------------------------------
//  THE NAME YOUR ROUTER SHOWS. When Pocket Nova asks the router for an
//  address (DHCP) it sends its "hostname" along (DHCP option 12), and
//  routers show that in their list of connected devices. Hostnames may
//  only use letters, digits and '-', so "Uno's Pocket Nova" becomes
//  "Unos-Pocket-Nova". Without one, the ESP32 calls itself "esp32-A1B2C3".
// ---------------------------------------------------------------------
char wifiHostname[32] = "";       // the Wi-Fi library keeps at most 31 characters

void wifiMakeHostname() {
  size_t n = 0;
  bool gap = false;
  for (const char* p = myHost[0] ? myHost : cfg.name; *p && n < sizeof(wifiHostname) - 1; p++) {
    char c = *p;
    if (isalnum((unsigned char)c)) {
      if (gap && n) {
        if (n >= sizeof(wifiHostname) - 2) break;
        wifiHostname[n++] = '-';
      }
      wifiHostname[n++] = c;
      gap = false;
    } else if (c != '\'') {
      gap = true;                 // spaces and other symbols become one '-'; apostrophes vanish
    }
  }
  wifiHostname[n] = 0;
  if (!n) strcpy(wifiHostname, "PocketNova");
}

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
  if (myApName[0]) strcpy(apSsid, myApName);
  else snprintf(apSsid, sizeof(apSsid), "%s Setup", cfg.name);
  if (myApPass[0]) {
    strcpy(apPass, myApPass);
  } else {
    // A new password each time. esp_random() uses radio noise, so it's
    // properly unpredictable while Wi-Fi or Bluetooth is running.
    for (int i = 0; i < 8; i++) apPass[i] = '0' + esp_random() % 10;
    apPass[8] = 0;
  }

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

// New names/password from the PC panel. Wi-Fi rules: a network name is
// 1..32 bytes, a WPA2 password 8..63 characters. Empty = automatic.
// Returns an error message, or nullptr when saved.
const char* wifiSetNames(const char* apName, const char* apPass_, const char* host) {
  if (strlen(apName) > 32) return "network name: 32 characters at most";
  size_t pl = strlen(apPass_);
  if (pl && (pl < 8 || pl > 63)) return "password: 8 to 63 characters";
  for (const char* p = apPass_; *p; p++) if (*p < 32 || *p > 126) return "password: plain letters, numbers and symbols only";
  if (strlen(host) > 31) return "router name: 31 characters at most";
  bool hostChanged = strcmp(host, myHost) != 0;
  strcpy(myApName, apName);
  strcpy(myApPass, apPass_);
  strcpy(myHost, host);
  wifiSave();
  wifiMakeHostname();
  Serial.printf("[WIFI] Setup network \"%s\", password %s, router name %s\n", myApName[0] ? myApName : "(auto)",
                myApPass[0] ? "(yours)" : "(random)", wifiHostname);
  if (setupOn) { wifiStopSetup(); wifiStartSetup(); }   // reopen it with the new name and password
  // The router learns the name when Pocket Nova asks for an address, so
  // rejoin to tell it. (Some routers keep showing the old name for a while.)
  if (hostChanged) {
    WiFi.setHostname(wifiHostname);
    if (cfg.wifiOn && wifiHasNetwork() && !setupOn) { WiFi.disconnect(); wifiRadioFor(false, false); wifiRadioFor(true, false); wifiJoin(); }
  }
  return nullptr;
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
  wifiMakeHostname();
  WiFi.setHostname(wifiHostname);        // must come before the radio starts
  if (cfg.wifiOn && wifiHasNetwork()) {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    wifiRadioFor(true, false);
    wifiJoin();
  } else {
    WiFi.mode(WIFI_OFF);                 // radio fully off: saves power and memory
  }
}

// ---------------------------------------------------------------------
//  WHO ELSE IS ON YOUR NETWORK
//  Every device on a home network has two addresses:
//    IP address    192.168.1.23 - handed out by the router, can change
//    MAC address   3C:22:FB:..  - built into its Wi-Fi chip (phones make
//                  up a "private" one per network, for privacy)
//  Before a device can send anything to an IP on the same network, it
//  shouts "who has 192.168.1.23?" to everyone, and the owner answers with
//  its MAC. That's ARP (Address Resolution Protocol). So to list the
//  devices we ask about every address in the network, a few at a time,
//  and write down who answers. Tools like Fing and arp-scan do the same.
//  Phones with the screen off sometimes sleep through it: scan again.
// ---------------------------------------------------------------------
struct NetDevice { uint32_t ip; uint8_t mac[6]; };   // ip as lwIP keeps it (network byte order)
const int NET_MAX = 48;
NetDevice     netFound[NET_MAX];
volatile int  netCount = 0;
volatile int  netNext = 0;             // next host number to ask about
int           netLast = 0;             // last host number (254 on a normal home network)
int           netTail = 0;             // listening rounds left after the last question
uint32_t      netBase = 0;             // the network part of the address, e.g. 192.168.1.0
uint32_t      netStepAt = 0;
bool          netScanning = false;
struct netif* netIf = nullptr;
void sendNetScan();                    // Remote.h

// lwIP, the ESP32's network stack, runs in its own task, and its ARP
// table may only be touched from there, so tcpip_callback() runs this
// in that task. Each round: copy the answers that came in, then ask
// about the next 6 addresses. The table only holds 10 answers, so small
// batches mean none get pushed out before we read them.
void netStep(void*) {
  ip4_addr_t* ip;
  struct netif* nif;
  struct eth_addr* mac;
  for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
    if (!etharp_get_entry(i, &ip, &nif, &mac) || nif != netIf) continue;
    bool have = false;
    for (int k = 0; k < netCount; k++) if (netFound[k].ip == ip->addr) { have = true; break; }
    if (have || netCount >= NET_MAX) continue;
    netFound[netCount].ip = ip->addr;
    memcpy(netFound[netCount].mac, mac->addr, 6);
    netCount++;
  }
  for (int k = 0; k < 6 && netNext <= netLast; k++, netNext++) {
    ip4_addr_t t;
    t.addr = htonl(netBase + netNext);
    etharp_request(netIf, &t);
  }
}

bool netScanStart() {
  if (wifiState != WF_ONLINE) return false;
  if (netScanning) return true;
  netIf = (struct netif*)esp_netif_get_netif_impl(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
  if (!netIf) return false;
  uint32_t me = ntohl((uint32_t)WiFi.localIP());
  uint32_t mask = ntohl((uint32_t)WiFi.subnetMask());
  if (mask < 0xFFFFFF00) mask = 0xFFFFFF00;   // bigger networks: just our 254 neighbours
  netBase = me & mask;
  netNext = 1;                                // .0 is the network itself
  netLast = (int)(~mask) - 1;                 // the top address is "everyone" (broadcast)
  netTail = 5;
  netCount = 0;
  netStepAt = millis();
  netScanning = true;
  Serial.printf("[WIFI] Looking for devices on %s/%d\n",IPAddress(htonl(netBase)).toString().c_str(), 32 - __builtin_ctz(~mask + 1));
  return true;
}

int netScanPercent() { return netLast > 0 ? constrain((netNext - 1) * 100 / netLast, 0, 100) : 0; }

void netScanUpdate(uint32_t now) {
  if (!netScanning || (int32_t)(now - netStepAt) < 0) return;
  netStepAt = now + 200;
  if (wifiState != WF_ONLINE) { netNext = netLast + 1; netTail = 0; }   // lost the network: report what we have
  if (netNext > netLast && netTail-- <= 0) {
    netScanning = false;
    Serial.printf("[WIFI] Found %d other device(s)\n", netCount);
    sendNetScan();
    return;
  }
  tcpip_callback(netStep, nullptr);
}

void wifiUpdate() {
  uint32_t now = millis();
  netScanUpdate(now);
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
