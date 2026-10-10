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
bool        myApOpen = false;      // setup network with no password at all
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
  myApOpen = prefs.getBool("apOpen", false);
  prefs.end();
}
void wifiSave() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("apName", myApName);
  prefs.putString("apPass", myApPass);
  prefs.putString("host", myHost);
  prefs.putBool("apOpen", myApOpen);
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
//  The setup page. One small HTML file kept in flash (PROGMEM), in the
//  panel's pixel style. Fonts can't load (no internet here), so the title
//  and Nova are drawn as pixels by the script, from the same 3x5 letters
//  as Font.h. It asks /scan for nearby networks (they appear as
//  suggestions under Network) and /result for progress, both as JSON.
// ---------------------------------------------------------------------
const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket Nova</title><style>
:root{--bg:#07060d;--card:#1e1930;--ln:#342b4d;--fg:#efeafc;--mu:#a59cc0;--ac:#a46bff;--hi:#c9a6ff;--ok:#3ddc84;--bad:#ff5d6c;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:var(--fg);font:16px/1.4 ui-monospace,"Cascadia Mono",Menlo,Consolas,monospace;
background:var(--bg) radial-gradient(#ffffff26 1px,transparent 1.5px) 0 0/29px 31px}
main{max-width:380px;margin:0 auto;padding:40px 16px;display:grid;gap:22px}
.pix{clip-path:polygon(0 6px,6px 6px,6px 0,calc(100% - 6px) 0,calc(100% - 6px) 6px,100% 6px,100% calc(100% - 6px),calc(100% - 6px) calc(100% - 6px),calc(100% - 6px) 100%,6px 100%,6px calc(100% - 6px),0 calc(100% - 6px))}
header{display:grid;justify-items:center;gap:14px}
svg{image-rendering:pixelated;shape-rendering:crispEdges;display:block}
form{background:var(--card);box-shadow:inset 0 0 0 2px var(--ln);padding:22px;display:grid;gap:16px}
label{display:grid;gap:8px;font-size:12px;font-weight:700;letter-spacing:.16em;text-transform:uppercase;color:var(--mu)}
input{width:100%;padding:13px 12px;border:0;border-radius:0;background:#120f1c;box-shadow:inset 0 0 0 2px var(--ln);color:var(--fg);font:inherit;letter-spacing:0;text-transform:none}
input:focus{outline:0;box-shadow:inset 0 0 0 2px var(--ac)}
button{margin-top:4px;padding:14px;border:0;background:var(--ac);color:#140b24;font:inherit;font-weight:700;letter-spacing:.16em;text-transform:uppercase;box-shadow:inset 0 0 0 2px var(--hi)}
button:active{background:var(--hi)}button:disabled{opacity:.55}
#msg{margin:0;min-height:1.4em;font-size:14px;text-align:center;color:var(--mu)}#msg.ok{color:var(--ok)}#msg.bad{color:var(--bad)}
</style></head><body><main>
<header><svg id="nova" width="64" height="60" viewBox="0 0 16 15" aria-hidden="true"></svg>
<h1 style="margin:0"><svg id="title" height="25" viewBox="0 0 43 5" role="img" aria-label="Pocket Nova"></svg></h1></header>
<form class="pix" id="f">
<label>Network<input id="ssid" list="nets" maxlength="32" autocomplete="off" autocapitalize="off" required></label>
<datalist id="nets"></datalist>
<label>Password<input id="pass" type="password" maxlength="64" autocomplete="off"></label>
<button class="pix" id="go">Connect</button><p id="msg" role="status"></p></form>
</main><script>
const $=id=>document.getElementById(id);
const COL={P:"#8f5cff",L:"#c9a6ff",D:"#120f1a",d:"#241d36",Y:"#ffd34d",y:"#7d6aa6",W:"#f4f1ff",C:"#2affd2",'#':"#c9a6ff"};
function px(svg,rows){let o="";rows.forEach((r,y)=>[...r].forEach((c,x)=>{if(COL[c])o+='<rect x="'+x+'" y="'+y+'" width="1.02" height="1.02" fill="'+COL[c]+'"/>'}));svg.innerHTML=o}
px($("nova"),[".......Y........","......YyY.......",".......y........","...PPPPPPPPPP...","..PLLLLLLLLLLP..","..PLDDDDDDDDLP..","..PLDWWDDWWDLP..","..PLDWCDDWCDLP..","..PLDDDDDDDDLP..","..PLDDDDDDDDLP..","..PPDDDDDDDDPP..","...PPPPPPPPPP...","....PP....PP....","....dd....dd....","................"]);
const G={P:"##.#.###.#..#..",O:".#.#.##.##.#.#.",C:".###..#..#...##",K:"#.##.###.#.##.#",E:"####..##.#..###",T:"###.#..#..#..#.",N:"##.#.##.##.##.#",V:"#.##.##.##.#.#.",A:".#.#.#####.##.#"};
const rows=["","","","",""];[..."POCKET NOVA"].forEach((ch,i)=>{for(let y=0;y<5;y++)rows[y]+=(G[ch]?G[ch].substr(y*3,3):"...")+(i<10?".":"")});px($("title"),rows);
async function scan(){const r=await fetch("/scan").then(r=>r.json()).catch(()=>null);
 if(!r||r.busy){setTimeout(scan,1500);return}
 $("nets").innerHTML="";r.list.forEach(n=>{const o=document.createElement("option");o.value=n.ssid;$("nets").appendChild(o)})}
function say(t,c){const m=$("msg");m.className=c||"";m.textContent=t}
$("f").onsubmit=async e=>{e.preventDefault();$("go").disabled=true;say("Connecting...");
 const body=new URLSearchParams({ssid:$("ssid").value,pass:$("pass").value,tz:-new Date().getTimezoneOffset()*60});
 const r=await fetch("/save",{method:"POST",body}).catch(()=>null);
 if(r&&r.status==429){const j=await r.json().catch(()=>({}));say("Too many tries. Wait "+(j.wait||60)+" seconds.","bad");$("go").disabled=false;return}
 if(r&&r.status==409){say("Already connecting. One moment.");}
 poll()};
async function poll(){const r=await fetch("/result").then(r=>r.json()).catch(()=>null);
 if(!r||r.state=="connecting"){setTimeout(poll,1000);return}
 $("go").disabled=false;
 if(r.state=="online"){say("Connected! "+r.ip,"ok");return}
 say("Couldn't join: "+r.reason+".","bad")}
scan();
</script></body></html>)HTML";

// ---------------------------------------------------------------------
//  KEEPING THE SETUP NETWORK TIDY
//  Anyone nearby can see the setup network, and with no password anyone
//  can join it. So Pocket Nova writes everything down (History.h) and:
//    - limits Connect tries: after 3 failures from the same phone it has
//      to wait 30 s, then 1 min, 2 min... (slows down password guessing)
//    - kicks devices you block in the panel, every time they come back
//    - blocks a device for 10 minutes if it joins 6 times in a minute
//      (a script hammering the network, which would slow everyone down)
// ---------------------------------------------------------------------
const int   BLOCK_MAX = 16;
uint8_t     blocked[BLOCK_MAX][6];      // your block list (saved)
int         blockedCount = 0;

struct ApGuest { uint8_t mac[6]; uint8_t joins; uint32_t windowAt, bannedUntil; };
const int   GUESTS = 12;
ApGuest     guests[GUESTS];             // recent visitors, for the flood check
uint8_t     kickQueue[4][6];            // kicked from the main loop, not inside the Wi-Fi event
volatile int kickCount = 0;

struct TryLog { uint32_t ip; uint8_t fails; uint32_t until; };
TryLog      tries[8];                   // per phone (by address): failed Connects
uint32_t    lastTryIp = 0;
uint8_t     lastTryMac[6];

void blockLoad() {
  prefs.begin("wifi", true);
  size_t n = prefs.getBytes("block", blocked, sizeof(blocked));
  prefs.end();
  blockedCount = n / 6;
}
void blockSave() {
  prefs.begin("wifi", false);
  if (blockedCount) prefs.putBytes("block", blocked, blockedCount * 6);
  else prefs.remove("block");                 // putBytes() ignores an empty list, so the old one would come back
  prefs.end();
}
int blockFind(const uint8_t* m) {
  for (int i = 0; i < blockedCount; i++) if (!memcmp(blocked[i], m, 6)) return i;
  return -1;
}

void kickLater(const uint8_t* mac) {
  portENTER_CRITICAL(&histMux);
  if (kickCount < 4) memcpy(kickQueue[kickCount++], mac, 6);
  portEXIT_CRITICAL(&histMux);
}
void kickNow(const uint8_t* mac) {
  uint16_t aid;
  if (esp_wifi_ap_get_sta_aid(mac, &aid) == ESP_OK && aid) esp_wifi_deauth_sta(aid);
}

// Block (or unblock) a device; blocking also kicks it off right away.
bool wifiBlock(const uint8_t* mac, bool on) {
  int i = blockFind(mac);
  if (on && i < 0) {
    if (blockedCount >= BLOCK_MAX) return false;
    memcpy(blocked[blockedCount++], mac, 6);
  } else if (!on && i >= 0) {
    memmove(blocked[i], blocked[i + 1], (blockedCount - i - 1) * 6);
    blockedCount--;
    for (auto& g : guests) if (!memcmp(g.mac, mac, 6)) g.bannedUntil = 0;
  }
  blockSave();
  if (on && setupOn) { kickNow(mac); histAdd(H_KICK, mac); }
  return true;
}

uint32_t apIpOf(const uint8_t* mac) {
  wifi_sta_list_t sl;
  esp_netif_sta_list_t nl;
  if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || esp_netif_get_sta_list(&sl, &nl) != ESP_OK) return 0;
  for (int i = 0; i < nl.num; i++) if (!memcmp(nl.sta[i].mac, mac, 6)) return nl.sta[i].ip.addr;
  return 0;
}
bool apMacOf(uint32_t ip, uint8_t* mac) {
  wifi_sta_list_t sl;
  esp_netif_sta_list_t nl;
  if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || esp_netif_get_sta_list(&sl, &nl) != ESP_OK) return false;
  for (int i = 0; i < nl.num; i++) if (nl.sta[i].ip.addr == ip) { memcpy(mac, nl.sta[i].mac, 6); return true; }
  return false;
}

// Runs in the Wi-Fi task each time a device joins or leaves the setup network.
void apOnJoin(WiFiEvent_t, WiFiEventInfo_t info) {
  const uint8_t* m = info.wifi_ap_staconnected.mac;
  uint32_t now = millis();
  ApGuest* g = nullptr;
  for (auto& x : guests) if (!memcmp(x.mac, m, 6)) { g = &x; break; }
  if (!g) {                                  // new face: reuse the quietest slot
    g = &guests[0];
    for (auto& x : guests) if (x.windowAt < g->windowAt) g = &x;
    memcpy(g->mac, m, 6);
    g->joins = 0; g->windowAt = now; g->bannedUntil = 0;
  }
  if (now - g->windowAt > 60000) { g->windowAt = now; g->joins = 0; }
  g->joins++;
  if (blockFind(m) >= 0 || (g->bannedUntil && (int32_t)(g->bannedUntil - now) > 0)) {
    histAdd(H_BLOCKED, m);
    kickLater(m);
  } else if (g->joins >= 6) {
    g->bannedUntil = now + 10UL * 60 * 1000;
    histAdd(H_FLOOD, m, 0, "joined 6 times in a minute");
    kickLater(m);
  } else {
    histAdd(H_JOIN, m);
  }
}
void apOnLeave(WiFiEvent_t, WiFiEventInfo_t info) {
  histAdd(H_LEAVE, info.wifi_ap_stadisconnected.mac);
}

TryLog* tryFor(uint32_t ip) {
  TryLog* t = &tries[0];
  for (auto& x : tries) if (x.ip == ip) return &x;
  for (auto& x : tries) if (!x.fails && (int32_t)(x.until - millis()) <= 0) { t = &x; break; }
  t->ip = ip; t->fails = 0; t->until = 0;
  return t;
}

// Every address we don't know (the phone's connectivity checks included)
// is sent to our page. That redirect is what triggers the sign-in pop-up.
void webRedirect() {
  web->sendHeader("Location", "http://192.168.4.1/", true);
  web->send(302, "text/plain", "");
}

void webScan() {
  static uint32_t lastAgain = 0;
  if (web->hasArg("again") && millis() - lastAgain > 10000) {   // one fresh scan per 10 s, however often it's asked
    lastAgain = millis();
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
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
  uint32_t ip = (uint32_t)web->client().remoteIP();
  uint8_t mac[6] = {0};
  apMacOf(ip, mac);
  TryLog* t = tryFor(ip);
  int32_t wait = (int32_t)(t->until - millis());
  if (wait > 0) {
    histAdd(H_LIMITED, mac, ip, s.c_str());
    web->send(429, "application/json", String("{\"wait\":") + (wait / 1000 + 1) + "}");
    return;
  }
  if (setupJoining && wifiState == WF_CONNECTING) { web->send(409, "application/json", "{\"busy\":true}"); return; }
  char note[34];
  snprintf(note, sizeof(note), "%s", s.c_str());   // the network name; never the password
  histAdd(H_TRY, mac, ip, note);
  lastTryIp = ip;
  memcpy(lastTryMac, mac, 6);
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
    WiFi.softAP(apSsid, myApOpen ? nullptr : apPass, ch);
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
  static bool hooked = false;
  if (!hooked) {
    WiFi.onEvent(apOnJoin, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    WiFi.onEvent(apOnLeave, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
    hooked = true;
  }
  // With a password it's WPA2 (the form is encrypted over the air);
  // open, anyone can join and everything is sent in the clear.
  WiFi.softAP(apSsid, myApOpen ? nullptr : apPass);
  histAdd(H_SETUP_OPEN, nullptr, 0, apSsid);
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
  Serial.printf("[WIFI] Setup network \"%s\", %s%s, page http://192.168.4.1/\n", apSsid,
                myApOpen ? "no password" : "password ", myApOpen ? "" : apPass);
}

void wifiStopSetup() {
  if (!setupOn) return;
  web->stop();
  delete web; web = nullptr;
  dns->stop();
  delete dns; dns = nullptr;
  WiFi.softAPdisconnect(true);
  setupOn = false;
  histAdd(H_SETUP_CLOSE);
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
const char* wifiSetNames(const char* apName, const char* apPass_, const char* host, bool open) {
  if (strlen(apName) > 32) return "network name: 32 characters at most";
  size_t pl = strlen(apPass_);
  if (pl && (pl < 8 || pl > 63)) return "password: 8 to 63 characters";
  for (const char* p = apPass_; *p; p++) if (*p < 32 || *p > 126) return "password: plain letters, numbers and symbols only";
  if (strlen(host) > 31) return "router name: 31 characters at most";
  bool hostChanged = strcmp(host, myHost) != 0;
  strcpy(myApName, apName);
  strcpy(myApPass, apPass_);
  strcpy(myHost, host);
  myApOpen = open;
  wifiSave();
  wifiMakeHostname();
  Serial.printf("[WIFI] Setup network \"%s\", password %s, router name %s\n", myApName[0] ? myApName : "(auto)",
                myApOpen ? "NONE (open)" : myApPass[0] ? "(yours)" : "(random)", wifiHostname);
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
  blockLoad();
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
int           netPass = 0;             // 0 = first sweep, 1 = second chance for the quiet ones
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
  for (int k = 0; k < 6 && netNext <= netLast; netNext++) {
    ip4_addr_t t;
    t.addr = htonl(netBase + netNext);
    bool have = false;                         // second sweep: only ask the ones that didn't answer
    if (netPass) for (int j = 0; j < netCount; j++) if (netFound[j].ip == t.addr) { have = true; break; }
    if (have) continue;
    etharp_request(netIf, &t);
    k++;
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
  netPass = 0;
  netCount = 0;
  netStepAt = millis();
  netScanning = true;
  Serial.printf("[WIFI] Looking for devices on %s/%d\n",IPAddress(htonl(netBase)).toString().c_str(), 32 - __builtin_ctz(~mask + 1));
  return true;
}

int netScanPercent() { return netLast > 0 ? constrain((netPass * netLast + netNext - 1) * 100 / (2 * netLast), 0, 100) : 0; }

void netScanUpdate(uint32_t now) {
  if (!netScanning || (int32_t)(now - netStepAt) < 0) return;
  netStepAt = now + 200;
  if (wifiState != WF_ONLINE) { netNext = netLast + 1; netTail = 0; netPass = 1; }   // lost the network: report what we have
  // PHONES NAP: with the screen off a phone only wakes its Wi-Fi every few
  // hundred ms to check for mail, so it can sleep through one question.
  // A second sweep, asking only the addresses that stayed quiet, catches most.
  if (netNext > netLast && netTail <= 0 && netPass == 0) {
    netPass = 1;
    netNext = 1;
    netTail = 5;
    netStepAt = now + 1500;                     // give sleepers a moment before asking again
    return;
  }
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
  while (kickCount) {
    uint8_t m[6];
    portENTER_CRITICAL(&histMux);
    memcpy(m, kickQueue[--kickCount], 6);
    portEXIT_CRITICAL(&histMux);
    kickNow(m);
  }
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
        if (setupOn && setupJoining) {
          setupDoneAt = now;
          histAdd(H_JOINED, lastTryMac, lastTryIp, wifiSsid);
          tryFor(lastTryIp)->fails = 0;
        }
      } else if (now - wifiJoinAt > WIFI_JOIN_MS || wifiRefusals >= 3) {   // 3 "no"s = it won't change its mind
        wifiState = WF_FAILED;
        wifiFailedAt = now;
        if (setupJoining) {                // a try from the setup page: count it against that phone
          histAdd(H_FAILED, lastTryMac, lastTryIp, wifiReason());
          TryLog* t = tryFor(lastTryIp);
          if (++t->fails >= 3) t->until = now + (30000UL << min(t->fails - 3, 5));   // 30 s, 1, 2, 4, 8, 16 min
        }
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
