#pragma once
// =====================================================================
//  Remote.h - commands from the Pocket Nova Panel (the PC app)
// =====================================================================
//  The PC sends one JSON object per line, e.g.  {"cmd":"status"}
//  Pocket Nova answers with one line that starts with '@' followed by
//  JSON, e.g.  @{"t":"status","screen":"PET",...}
//  Everything else it prints (lines like "[EV] TAP") is a human log.
//
//  Commands:
//    hello                        who are you? (device, firmware, lists)
//    get                          all settings
//    status                       live state (screen, tilt, Bluetooth, pet)
//    set   {"cfg":{...}}          change settings (brightIdx, rotation,
//                                 autoRotate, tvBrand, lastApp, name,
//                                 swiftPair, mouseSpeed 0..4, mouseFlip 0..3)
//    flip  {"axis":"x"|"y"}       swap left/right or forward/back
//    bonds                        paired devices
//    unbond {"addr":"AA:.."}      forget one device      (restarts)
//    unbond_all                   forget every device    (restarts)
//    slot  {"n":0..2}             switch Bluetooth slot  (restarts)
//    factory_reset                erase everything       (restarts)
//    reboot
//    input {"ev":"tap"|"double"|"triple"|"hold"|"left"|"right"|"shake"}
//    tilt  {"dir":"L"|"R"|"F"|"K"|"0"}   fake a held tilt (0 = stop)
//    ir    {"brand":n,"code":n}   send a TV code (brand = BRAND_COUNT: all brands)
//    tvbrands                     brand list with verification counts
//    findtv                       open TV and start FIND TV
//    time  {"epoch":n,"tz":seconds}  set the clock (for night mode)
//    wifi  {"action":"setup"|"stop"|"on"|"off"|"join"|"forget"}
//    wifi  {"action":"logpass","on":true}   keep typed setup-page passwords in the history
//    wifi  {"action":"names","apName":"..","apPass":"..","host":".."}
//                                 setup network name/password and router name ("" = automatic)
//    history {"since":n}          events after #n (History.h)
//    block {"mac":"AA:..","on":true}  block/unblock a device on the setup network (blocking kicks it)
//    wifi  {"action":"connect","ssid":"..","pass":".."}  join a network (saved only once it works)
//    wifiscan {"again":true}      nearby networks (answer has "busy" while still looking)
//    netscan                      list the devices on your Wi-Fi network
//                                 (answers @{"t":"netscan",...} when done)
//    keys  {"list":[ids], "custom":[{"n":0,"name":"..","mods":n,"key":n}]}
//                                 Keys app: the library, your list, custom slots
//    keytest {"id":n}             send one shortcut now   Wi-Fi (Wifi.h)
//    mirror {"on":true}           stream the screen (@{"t":"fb",...} 10x/s)
//    tutorial                     play the tutorial
//    pet   {"mood":"happy"|"love"|"dizzy"|"sleep"|"wake"}
//    alert {"kind":"download"|"cpu"|"battery"|"netdown"|"netup"|"note","text":".."}
//                                 pop a message up over any screen
//    menu  {"list":["MEDIA",..]}  which apps the menu shows, in order
//    app   {"name":"FINDER"}      open an app
//    chanscan {"start":true}      Wi-Fi channel scan (answers "chans"; "busy" while scanning)
//    netcfg {"on":..,"target":"1.1.1.1","slow":150,"alert":true}   health monitor
//    nethist                      the last 30 ping pairs
//    findscan {"secs":10}         listen for Bluetooth devices
//    findlist                     what was heard (strongest first)
//    findtarget {"addr":"..","name":".."} | {"clear":true}   the FINDER target
//    homecfg {"on","host","port","user","pass","base","disc","scenes":[..]}   MQTT
//    homescene {"n":0}            send a scene now (a test)
// =====================================================================

#include <ArduinoJson.h>
#include <sys/time.h>
#include <algorithm>

extern bool replyBle;                   // NovaRemote.h: answering the phone page
void remoteNotify(const String& line);

void sendJson(JsonDocument& d) {
  if (replyBle) {
    String s = "@";
    serializeJson(d, s);
    remoteNotify(s);
    return;
  }
  Serial.print('@');
  serializeJson(d, Serial);
  Serial.println();
}

void replyOk(const char* cmd, const char* note = nullptr) {
  JsonDocument d;
  d["t"] = "ok";
  d["cmd"] = cmd;
  if (note) d["note"] = note;
  sendJson(d);
}

void replyError(const char* msg) {
  JsonDocument d;
  d["t"] = "error";
  d["msg"] = msg;
  sendJson(d);
}

void sendHello() {
  JsonDocument d;
  d["t"] = "hello";
  d["device"] = "PocketNova";
  d["fw"] = FW_VERSION;
  d["name"] = cfg.name;
  d["slot"] = cfg.btSlot;
  d["slots"] = BT_SLOTS;
  d["address"] = bleOwnAddress();
  d["reset"] = resetReasonName();
  d["brightLevels"] = BRIGHT_COUNT;
  JsonArray a = d["apps"].to<JsonArray>();
  for (int i = 0; i < APP_COUNT; i++) a.add(apps[i].name);
  JsonArray mo = d["menu"].to<JsonArray>();
  for (int i = 0; i < menuLen; i++) mo.add(apps[menuOrder[i]].name);
  JsonArray b = d["brands"].to<JsonArray>();
  for (int i = 0; i < BRAND_COUNT; i++) b.add(BRANDS[i].name);
  JsonArray c = d["tvCmds"].to<JsonArray>();
  for (int i = 0; i < CMD_COUNT; i++) c.add(CMD_NAMES[i]);
  sendJson(d);
}

void sendConfig() {
  JsonDocument d;
  d["t"] = "config";
  d["name"] = cfg.name;
  d["brightIdx"] = cfg.brightIdx;
  d["rotation"] = cfg.rotation;
  d["autoRotate"] = cfg.autoRotate;
  d["tvBrand"] = cfg.tvBrand;
  d["tvPower"] = cfg.tvPower;
  d["slot"] = cfg.btSlot;
  d["swiftPair"] = cfg.swiftPair;
  d["wifiOn"] = cfg.wifiOn;
  d["apName"] = myApName;
  d["host"] = myHost;
  d["hostname"] = wifiHostname;
  d["apPassCustom"] = myApPass[0] != 0;
  d["apOpen"] = myApOpen;
  d["logPass"] = myLogPass;
  JsonArray bl = d["blocked"].to<JsonArray>();
  for (int i = 0; i < blockedCount; i++) bl.add(macToString(blocked[i]));
  if (!replyBle) d["apPass"] = myApPass;          // the PC (USB) only, never the phone page
  d["lastApp"] = cfg.lastApp;
  d["tutorialDone"] = cfg.tutorialDone;
  d["snakeHigh"] = cfg.snakeHigh;
  d["petLove"] = cfg.petLove;
  d["mouseSpeed"] = cfg.mouseSpeed;
  d["mouseFlip"] = cfg.mouseFlip;
  d["reactBest"] = cfg.reactBest;
  d["simonBest"] = cfg.simonBest;
  d["otaPort"] = OTA_PORT;
  d["tempF"] = cfg.tempF;
  d["tempOffset"] = cfg.tempOffset;
  JsonObject n = d["net"].to<JsonObject>();
  n["on"] = nmOn;
  n["target"] = nmTarget;
  n["slow"] = nmSlowMs;
  n["alert"] = nmAlert;
  JsonObject h = d["home"].to<JsonObject>();
  h["on"] = hmOn;
  h["host"] = hmHost;
  h["port"] = hmPort;
  h["user"] = hmUser;
  h["passSet"] = hmPass[0] != 0;                  // never the password itself
  h["base"] = hmBase;
  hmMakeTopics();
  h["topic"] = hmTopic;
  h["disc"] = hmDiscovery;
  JsonArray sc = h["scenes"].to<JsonArray>();
  for (int i = 0; i < hmSceneCount; i++) sc.add(hmScenes[i]);
  JsonObject f = d["find"].to<JsonObject>();
  if (findHave) { f["addr"] = macToString(findAddr); f["name"] = findName; }
  if (!replyBle) d["otaKey"] = otaKey;            // Wi-Fi update key: the PC (USB) only
  d["pX"]["axis"] = cfg.pX.axis;   d["pX"]["sign"] = cfg.pX.sign;
  d["pUp"]["axis"] = cfg.pUp.axis; d["pUp"]["sign"] = cfg.pUp.sign;
  sendJson(d);
}

// The Keys app: every shortcut in the library, your list and custom slots.
void sendKeysInfo() {
  JsonDocument d;
  d["t"] = "keys";
  d["max"] = KEY_LIST_MAX;
  d["customId"] = CUSTOM_ID;
  JsonArray lib = d["lib"].to<JsonArray>();
  for (int i = 0; i < SHORTCUT_COUNT; i++) {
    JsonObject o = lib.add<JsonObject>();
    o["name"] = SHORTCUTS[i].name;
    o["spr"] = SHORTCUTS[i].sprite;          // the 5x5 picture, for the panel
    o["mods"] = SHORTCUTS[i].mods;
    o["key"] = SHORTCUTS[i].key;
    if (SHORTCUTS[i].media) o["media"] = SHORTCUTS[i].media;
    if (SHORTCUTS[i].confirm) o["confirm"] = true;
  }
  JsonArray cu = d["custom"].to<JsonArray>();
  for (auto& c : customKeys) {
    JsonObject o = cu.add<JsonObject>();
    o["name"] = c.name;
    o["mods"] = c.mods;
    o["key"] = c.key;
  }
  JsonArray l = d["list"].to<JsonArray>();
  for (uint8_t i = 0; i < keyListLen; i++) l.add(keyList[i]);
  sendJson(d);
}

// Per-brand detail for the panel: how many buttons, how well verified, protocol.
void sendTvBrands() {
  JsonDocument d;
  d["t"] = "tvbrands";
  d["all"] = BRAND_COUNT;          // index meaning "ALL BRANDS"
  d["sweep"] = SWEEP_COUNT;
  d["found"] = cfg.tvPower >= 0 ? POWER_SWEEP[cfg.tvPower].label : "";
  JsonArray a = d["list"].to<JsonArray>();
  for (int i = 0; i < BRAND_COUNT; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = BRANDS[i].name;
    o["letter"] = String(BRANDS[i].letter);
    o["proto"] = PROTO_NAMES[BRANDS[i].codes[CMD_POWER].proto];
    o["verified"] = BRANDS[i].verified;
    int n = 0;
    for (int c = 0; c < CMD_COUNT; c++) if (BRANDS[i].codes[c].proto != IR_NONE) n++;
    o["buttons"] = n;
  }
  sendJson(d);
}

void sendStatus() {
  JsonDocument d;
  d["t"] = "status";
  d["screen"] = SCREEN_NAMES[screen];
  d["app"] = currentApp >= 0 ? apps[currentApp].name : "";
  d["menuIndex"] = menu.index;
  d["ble"] = bleOK();
  d["bonds"] = bleBondCount();
  d["slot"] = cfg.btSlot;
  d["host"] = hostBondKnown && bleOK() ? macToString(hostBond) : String("");
  d["swiftPair"] = bleKeyboard.swiftPairOn();   // true = broadcasting the pop-up right now
  JsonObject w = d["wifi"].to<JsonObject>();
  w["on"] = cfg.wifiOn;
  w["state"] = WIFI_STATE_NAMES[wifiState];
  w["ssid"] = wifiSsid;                         // never the password
  w["ip"] = wifiIp();
  w["host"] = wifiHostname;
  if (wifiTrying) { w["trying"] = wifiSsid; w["prev"] = prevSsid; }
  if (lastTrySsid[0]) {
    JsonObject t = w["lastTry"].to<JsonObject>();
    t["ssid"] = lastTrySsid;
    t["outcome"] = lastTryOutcome == 1 ? "ok" : lastTryOutcome == 2 ? "failed" : "trying";
    if (lastTryOutcome == 2) t["reason"] = lastTryReason;
  }
  if (netScanning) w["scan"] = netScanPercent();
  if (wifiState == WF_ONLINE) w["rssi"] = WiFi.RSSI();
  if (wifiState == WF_FAILED) w["reason"] = wifiReason();
  w["setup"] = setupOn;
  if (setupOn) {
    w["apSsid"] = apSsid;
    w["apOpen"] = myApOpen;
    if (!replyBle) w["apPass"] = apPass;        // not to the phone page
    w["setupLeft"] = (int32_t)(setupUntil - millis()) / 1000;
    // Phones and laptops joined to the setup network right now.
    wifi_sta_list_t sl;
    esp_netif_sta_list_t nl;
    JsonArray cl = w["apClients"].to<JsonArray>();
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK && esp_netif_get_sta_list(&sl, &nl) == ESP_OK)
      for (int i = 0; i < nl.num; i++) {
        JsonObject o = cl.add<JsonObject>();
        o["mac"] = macToString(nl.sta[i].mac);
        if (nl.sta[i].ip.addr) o["ip"] = IPAddress(nl.sta[i].ip.addr).toString();
      }
  }
  d["tiltX"] = serialized(String(tiltX - tiltBaseX, 2));
  d["tiltY"] = serialized(String(tiltY - tiltBaseY, 2));
  d["baseX"] = serialized(String(tiltBaseX, 2));   // the rest pose tilt is measured from
  d["baseY"] = serialized(String(tiltBaseY, 2));
  d["faceDown"] = faceDown;
  d["pet"] = PET_MOOD_NAMES[petMood];
  d["love"] = cfg.petLove;
  d["night"] = isNight();
  JsonObject se = d["sensor"].to<JsonObject>();
  se["kind"] = SENSOR_NAMES[senKind];
  if (sensorOk()) { se["t"] = serialized(String(senTemp, 1)); se["h"] = serialized(String(senHum, 0)); }
  JsonObject nm = d["net"].to<JsonObject>();
  nm["router"] = nmState(nmRouter);
  nm["internet"] = nmState(nmNet);
  nm["r"] = nmLast(nmRouter);
  nm["n"] = nmLast(nmNet);
  nm["lossR"] = nmLost(nmRouter, NET_HIST);
  nm["lossN"] = nmLost(nmNet, NET_HIST);
  nm["count"] = nmCount;
  JsonObject hm = d["home"].to<JsonObject>();
  hm["state"] = hmStateName();
  hm["connected"] = mqtt.connected();
  hm["sent"] = hmSent;
  hm["got"] = hmGot;
  JsonObject fd = d["find"].to<JsonObject>();
  fd["scanning"] = scanOn;
  fd["have"] = findHave;
  if (findHave && findSeenAt) { fd["rssi"] = (int)findRssi; fd["ago"] = millis() - findSeenAt; fd["trend"] = serialized(String(findTrend, 1)); }
  d["chanScanning"] = chScanning;
  d["clock"] = clockValid();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  sendJson(d);
}

void sendBonds() {
  JsonDocument d;
  d["t"] = "bonds";
  d["slot"] = cfg.btSlot;
  d["connected"] = bleOK();
  d["host"] = hostBondKnown && bleOK() ? macToString(hostBond) : String("");   // connected now
  JsonArray a = d["list"].to<JsonArray>();
  JsonArray s = d["slots"].to<JsonArray>();      // slot of each pairing, -1 = not known yet
  esp_ble_bond_dev_t list[15];
  int n = bleBondList(list, 15);
  for (int i = 0; i < n; i++) { a.add(macToString(list[i].bd_addr)); s.add(bondSlot(list[i].bd_addr)); }
  sendJson(d);
}

void sendMirrorFrame() {
  static const char HEX_[] = "0123456789abcdef";
  char hex[25 * 6 + 1];
  for (int i = 0; i < 25; i++) {
    const uint8_t c[3] = {fb[i].r, fb[i].g, fb[i].b};
    for (int k = 0; k < 3; k++) {
      hex[i * 6 + k * 2]     = HEX_[c[k] >> 4];
      hex[i * 6 + k * 2 + 1] = HEX_[c[k] & 15];
    }
  }
  hex[150] = 0;
  Serial.print("@{\"t\":\"fb\",\"d\":\"");
  Serial.print(hex);
  Serial.println("\"}");
}

Event eventFromName(const char* s) {
  if (!strcmp(s, "tap")) return EV_TAP;
  if (!strcmp(s, "double")) return EV_DOUBLE;
  if (!strcmp(s, "triple")) return EV_TRIPLE;
  if (!strcmp(s, "hold")) return EV_HOLD;
  if (!strcmp(s, "left")) return EV_LEFT;
  if (!strcmp(s, "right")) return EV_RIGHT;
  if (!strcmp(s, "shake")) return EV_SHAKE;
  if (!strcmp(s, "rock")) return EV_ROCK;
  return EV_NONE;
}

// Events after #since, oldest first. "ago" is in ms so the PC can turn
// it into a clock time without Pocket Nova needing to know the time.
void sendHistory(uint32_t since) {
  JsonDocument d;
  d["t"] = "history";
  d["boot"] = histBoot;
  d["last"] = histSeq;
  JsonArray a = d["list"].to<JsonArray>();
  uint32_t first = histSeq > HIST_LEN ? histSeq - HIST_LEN + 1 : 1;
  if (since + 1 > first) first = since + 1;
  for (uint32_t s = first; s <= histSeq && a.size() < 16; s++) {   // 16 at a time keeps Bluetooth replies small
    HistEntry e;
    portENTER_CRITICAL(&histMux);
    e = hist[(s - 1) % HIST_LEN];
    portEXIT_CRITICAL(&histMux);
    JsonObject o = a.add<JsonObject>();
    o["n"] = e.seq;
    o["k"] = HIST_NAMES[e.kind];
    o["ago"] = millis() - e.at;
    static const uint8_t ZERO[6] = {0};
    if (memcmp(e.mac, ZERO, 6)) o["mac"] = macToString(e.mac);
    if (e.ip) o["ip"] = IPAddress(e.ip).toString();
    if (e.note[0]) o["note"] = e.note;
    if (e.pass[0]) o["pass"] = e.pass;
  }
  sendJson(d);
}

// The result of a network scan (Wifi.h), sorted by address.
void sendNetScan() {
  JsonDocument d;
  d["t"] = "netscan";
  d["me"] = WiFi.localIP().toString();
  d["myMac"] = WiFi.macAddress();
  d["router"] = WiFi.gatewayIP().toString();
  d["host"] = wifiHostname;
  d["ssid"] = wifiSsid;
  int n = netCount;
  std::sort(netFound, netFound + n, [](const NetDevice& a, const NetDevice& b) { return ntohl(a.ip) < ntohl(b.ip); });
  JsonArray a = d["list"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["ip"] = IPAddress(netFound[i].ip).toString();
    o["mac"] = macToString(netFound[i].mac);
  }
  sendJson(d);
}

void applySettings(JsonObject c) {
  bool nameChanged = false;
  if (c["brightIdx"].is<int>()) cfg.brightIdx = constrain(c["brightIdx"].as<int>(), 0, BRIGHT_COUNT - 1);
  if (c["rotation"].is<int>()) { cfg.rotation = c["rotation"].as<int>() & 3; onRotationChanged(); }
  if (c["autoRotate"].is<bool>()) cfg.autoRotate = c["autoRotate"].as<bool>();
  if (c["tvBrand"].is<int>()) { cfg.tvBrand = constrain(c["tvBrand"].as<int>(), 0, BRAND_COUNT); cfg.tvPower = -1; }
  if (c["swiftPair"].is<bool>()) cfg.swiftPair = c["swiftPair"].as<bool>();
  if (c["mouseSpeed"].is<int>()) cfg.mouseSpeed = constrain(c["mouseSpeed"].as<int>(), 0, 4);
  if (c["mouseFlip"].is<int>()) cfg.mouseFlip = c["mouseFlip"].as<int>() & 3;
  if (c["tempF"].is<bool>()) cfg.tempF = c["tempF"].as<bool>();
  if (c["tempOffset"].is<int>()) cfg.tempOffset = constrain(c["tempOffset"].as<int>(), -100, 100);
  if (c["lastApp"].is<int>()) cfg.lastApp = constrain(c["lastApp"].as<int>(), 0, APP_COUNT - 1);
  if (c["name"].is<const char*>()) {
    const char* n = c["name"];
    if (strlen(n) >= 1 && strlen(n) < sizeof(cfg.name) && strcmp(n, cfg.name)) {
      strncpy(cfg.name, n, sizeof(cfg.name) - 1);
      cfg.name[sizeof(cfg.name) - 1] = 0;
      nameChanged = true;
    }
  }
  saveConfig();
  sendConfig();
  if (nameChanged) restartSoon("new Bluetooth name");
}

void handleRemoteLine(const char* line) {
  JsonDocument in;
  if (deserializeJson(in, line)) { replyError("bad json"); return; }
  const char* cmd = in["cmd"] | "";

  if (!strcmp(cmd, "hello"))       { sendHello(); return; }
  if (!strcmp(cmd, "get"))         { sendConfig(); return; }
  if (!strcmp(cmd, "status"))      { sendStatus(); return; }
  if (!strcmp(cmd, "bonds"))       { sendBonds(); return; }
  if (!strcmp(cmd, "set"))         { applySettings(in["cfg"].as<JsonObject>()); return; }

  if (!strcmp(cmd, "flip")) {
    const char* axis = in["axis"] | "x";
    if (axis[0] == 'y') cfg.pUp.sign = -cfg.pUp.sign;
    else cfg.pX.sign = -cfg.pX.sign;
    saveConfig();
    setTiltBase();
    sendConfig();
    return;
  }
  if (!strcmp(cmd, "unbond")) {
    const char* addr = in["addr"] | "";
    if (!bleRemoveBond(addr)) { replyError("unknown device"); return; }
    replyOk(cmd, "restarting");
    restartSoon("forgot a device");
    return;
  }
  if (!strcmp(cmd, "unbond_all")) {
    int n = bleRemoveAllBonds();
    Serial.printf("[BT] Forgot %d device(s)\n", n);
    replyOk(cmd, "restarting");
    restartSoon("ready to pair a new device");
    return;
  }
  if (!strcmp(cmd, "slot")) {
    int n = in["n"] | 0;
    if (n < 0 || n >= BT_SLOTS) { replyError("slot out of range"); return; }
    cfg.btSlot = n;
    saveConfig();
    replyOk(cmd, "restarting");
    restartSoon("switching Bluetooth slot");
    return;
  }
  if (!strcmp(cmd, "factory_reset")) {
    bleRemoveAllBonds();
    prefs.begin("keys", false); prefs.clear(); prefs.end();
    eraseConfig();
    replyOk(cmd, "restarting");
    restartSoon("factory reset");
    return;
  }
  if (!strcmp(cmd, "reboot")) { replyOk(cmd, "restarting"); restartSoon("asked by PC"); return; }

  if (!strcmp(cmd, "input")) {
    Event e = eventFromName(in["ev"] | "");
    if (e == EV_NONE) { replyError("unknown event"); return; }
    pushEvent(e);
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "tilt")) {
    const char* dir = in["dir"] | "0";
    tiltOverride = (dir[0] == 'L' || dir[0] == 'R' || dir[0] == 'F' || dir[0] == 'K') ? dir[0] : 0;
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "ir")) {
    int b = in["brand"] | (int)cfg.tvBrand, c = in["code"] | 0;
    if (c == CMD_POWER) {               // POWER is always universal
      if (!(screen == SCR_APP && currentApp == 1)) openApp(1);
      startUniversalPower();
      replyOk(cmd, "universal power");
      return;
    }
    if (b < 0 || b >= BRAND_COUNT || c < 0 || c >= CMD_COUNT) { replyError("pick your TV brand first"); return; }
    if (irSendCmd(b, c)) { fxRipple(TV_COLOR); replyOk(cmd); }
    else replyError("this brand has no such button");
    return;
  }
  if (!strcmp(cmd, "time")) {
    struct timeval tv = {(time_t)(in["epoch"] | 0L), 0};
    settimeofday(&tv, nullptr);
    tzOffsetSec = in["tz"] | 0;
    if (cfg.tz != tzOffsetSec) { cfg.tz = tzOffsetSec; saveConfig(); }
    replyOk(cmd, isNight() ? "night" : "day");
    return;
  }
  if (!strcmp(cmd, "wifi")) {
    const char* a = in["action"] | "";
    if (!strcmp(a, "setup")) wifiStartSetup();
    else if (!strcmp(a, "stop")) wifiStopSetup();
    else if (!strcmp(a, "on")) wifiSetOn(true);
    else if (!strcmp(a, "join")) {           // try again now, with a fresh scan for the log
      if (!wifiHasNetwork()) { replyError("no network saved"); return; }
      WiFi.enableSTA(true);
      WiFi.scanNetworks();                    // ~3 s, blocking: fine for a manual retry
      wifiJoin();
    }
    else if (!strcmp(a, "off")) wifiSetOn(false);
    else if (!strcmp(a, "forget")) wifiForget();
    else if (!strcmp(a, "connect")) {        // join a network from the panel (kept only if it works)
      const char* s = in["ssid"] | "";
      const char* p = in["pass"] | "";
      size_t pl = strlen(p);
      if (!*s || strlen(s) > 32) { replyError("network name: 1 to 32 characters"); return; }
      if (pl && (pl < 8 || pl > 64)) { replyError("password: 8 to 64 characters (or empty for an open network)"); return; }
      wifiTry(s, p);
    }
    else if (!strcmp(a, "logpass")) {         // log the passwords typed on the setup page?
      myLogPass = in["on"] | false;
      wifiSave();
      sendConfig();
    }
    else if (!strcmp(a, "names")) {
      char keep[64];                          // no "apPass" sent = keep the current one
      strcpy(keep, myApPass);
      const char* pw = in["apPass"].is<const char*>() ? in["apPass"].as<const char*>() : keep;
      const char* err = wifiSetNames(in["apName"] | "", pw, in["host"] | "", in["open"] | myApOpen);
      if (err) { replyError(err); return; }
      sendConfig();
    }
    else { replyError("unknown wifi action"); return; }
    replyOk(cmd, a);
    sendStatus();
    return;
  }
  if (!strcmp(cmd, "wifiscan")) {          // nearby networks; ask again while "busy"
    JsonDocument d;
    d["t"] = "wifiscan";
    if (in["again"] | false) { WiFi.scanDelete(); }
    JsonArray a = d["list"].to<JsonArray>();
    d["busy"] = !wifiScanList(a);
    sendJson(d);
    return;
  }
  if (!strcmp(cmd, "apdevices")) {         // browser descriptions of setup-network visitors (Wifi.h noteClient)
    JsonDocument d;
    d["t"] = "apdevices";
    JsonArray a = d["list"].to<JsonArray>();
    for (auto& g : guests) {
      if (!g.ua[0]) continue;
      JsonObject o = a.add<JsonObject>();
      o["mac"] = macToString(g.mac);
      o["ua"] = g.ua;
      if (g.scr[0]) o["scr"] = g.scr;
    }
    sendJson(d);
    return;
  }
  if (!strcmp(cmd, "history")) { sendHistory(in["since"] | 0); return; }
  if (!strcmp(cmd, "block")) {
    uint8_t m[6];
    if (sscanf(in["mac"] | "", "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
      replyError("bad address"); return;
    }
    if (!wifiBlock(m, in["on"] | true)) { replyError("block list is full (16)"); return; }
    sendConfig();
    return;
  }
  if (!strcmp(cmd, "netscan")) {
    if (!netScanStart()) { replyError("join a Wi-Fi network first"); return; }
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "keys")) {
    if (in["custom"].is<JsonArray>()) {
      for (JsonObject o : in["custom"].as<JsonArray>()) {
        int n = o["n"] | -1;
        if (n < 0 || n >= CUSTOM_SLOTS) continue;
        CustomKey& c = customKeys[n];
        const char* name = o["name"] | "";
        size_t k = 0;                    // keep what the 3x5 font can show
        for (const char* p = name; *p && k < sizeof(c.name) - 1; p++)
          if (isalnum((unsigned char)*p) || strchr(" +-.:/?", *p)) c.name[k++] = toupper((unsigned char)*p);
        c.name[k] = 0;
        if (!k) strcpy(c.name, "CUSTOM");
        c.mods = (o["mods"] | 0) & 15;
        c.key = o["key"] | 0;
      }
    }
    if (in["list"].is<JsonArray>()) {
      keyListLen = 0;
      for (JsonVariant v : in["list"].as<JsonArray>()) {
        int id = v | -1;
        if (id < 0 || id > 255 || !keyIdValid(id) || keyListLen >= KEY_LIST_MAX) continue;
        bool dup = false;
        for (uint8_t i = 0; i < keyListLen; i++) if (keyList[i] == id) dup = true;
        if (!dup) keyList[keyListLen++] = id;
      }
    } else {                             // a custom slot was cleared: drop it from the list
      uint8_t n = 0;
      for (uint8_t i = 0; i < keyListLen; i++) if (keyIdValid(keyList[i])) keyList[n++] = keyList[i];
      keyListLen = n;
    }
    if (in["custom"].is<JsonArray>() || in["list"].is<JsonArray>()) keysSave();
    sendKeysInfo();
    return;
  }
  if (!strcmp(cmd, "keytest")) {
    int id = in["id"] | -1;
    if (id < 0 || id > 255 || !keyIdValid(id)) { replyError("no such shortcut"); return; }
    if (!bleOK()) { replyError("Bluetooth isn't connected to a PC"); return; }
    keyFire(id);
    fxRipple(CRGB(255, 120, 0));
    replyOk(cmd, keyName(id));
    return;
  }
  if (!strcmp(cmd, "tvbrands")) { sendTvBrands(); return; }
  if (!strcmp(cmd, "findtv")) {
    int tvApp = 1;   // apps[1] is TV
    if (!(screen == SCR_APP && currentApp == tvApp)) openApp(tvApp);
    startFind();
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "alert")) {
    alertShow(in["kind"] | "note", in["text"] | "");
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "menu")) {
    if (!in["list"].is<JsonArray>()) { replyError("list missing"); return; }
    menuLen = 0;
    for (JsonVariant v : in["list"].as<JsonArray>()) menuAdd(appByName(v | ""));
    menuAdd(APP_COUNT - 1);                       // SETTINGS stays
    menuSave();
    menu.reset(menuLen, max(0, menuPos(currentApp >= 0 ? currentApp : cfg.lastApp)));
    sendHello();
    return;
  }
  if (!strcmp(cmd, "app")) {
    int i = appByName(in["name"] | "");
    if (i < 0) { replyError("no such app"); return; }
    if (screen == SCR_APP && currentApp >= 0 && currentApp != i && apps[currentApp].leave) apps[currentApp].leave();
    openApp(i);
    replyOk(cmd, apps[i].name);
    return;
  }
  if (!strcmp(cmd, "chanscan")) {
    if (in["start"] | false) {
      if (!chanScanStart()) { replyError("couldn't start a scan"); return; }
    }
    JsonDocument d;
    d["t"] = "chans";
    d["busy"] = chScanning;
    d["have"] = chHave;
    if (chHave && !chScanning) {
      JsonArray c = d["chans"].to<JsonArray>();
      for (int i = 1; i <= 13; i++) {
        JsonObject o = c.add<JsonObject>();
        o["ch"] = i;
        o["nets"] = chans[i].nets;
        o["load"] = serialized(String(chans[i].load, 2));
      }
      d["best"] = chBest;
      d["bestAny"] = chBestAny;
      d["total"] = chTotal;
      d["mine"] = wifiState == WF_ONLINE ? WiFi.channel() : 0;
      chanList(d["list"].to<JsonArray>());
    }
    sendJson(d);
    return;
  }
  if (!strcmp(cmd, "netcfg")) {
    if (in["on"].is<bool>()) nmOn = in["on"];
    if (in["alert"].is<bool>()) nmAlert = in["alert"];
    if (in["slow"].is<int>()) nmSlowMs = constrain(in["slow"].as<int>(), 20, 2000);
    if (in["target"].is<const char*>()) {
      const char* t = in["target"];
      if (!*t || strlen(t) >= sizeof(nmTarget)) { replyError("target: an address or name, up to 63 characters"); return; }
      strcpy(nmTarget, t);
      nmCount = 0;                                // new target: start the graph over
      nmNetIpOk = false;
      nmLookup.state = 0;
    }
    nmSave();
    sendConfig();
    return;
  }
  if (!strcmp(cmd, "nethist")) {
    JsonDocument d;
    d["t"] = "nethist";
    d["slow"] = nmSlowMs;
    netHistory(d.as<JsonObject>());
    sendJson(d);
    return;
  }
  if (!strcmp(cmd, "findscan")) {
    scanStart(constrain((int)(in["secs"] | 10), 3, 60) * 1000UL);
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "findlist")) {
    JsonDocument d;
    d["t"] = "findlist";
    d["scanning"] = scanOn;
    finderList(d["list"].to<JsonArray>());
    sendJson(d);
    return;
  }
  if (!strcmp(cmd, "findtarget")) {
    if (in["clear"] | false) finderClear();
    else {
      uint8_t m[6];
      if (sscanf(in["addr"] | "", "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        replyError("bad address"); return;
      }
      finderSetTarget(m, in["name"] | "");
    }
    sendConfig();
    return;
  }
  if (!strcmp(cmd, "homecfg")) {
    auto copyIn = [&](const char* key, char* dst, size_t n) -> bool {
      if (!in[key].is<const char*>()) return true;
      const char* v = in[key];
      if (strlen(v) >= n) return false;
      strcpy(dst, v);
      return true;
    };
    if (!copyIn("host", hmHost, sizeof(hmHost)) || !copyIn("user", hmUser, sizeof(hmUser)) ||
        !copyIn("pass", hmPass, sizeof(hmPass)) || !copyIn("base", hmBase, sizeof(hmBase))) {
      replyError("one of the fields is too long"); return;
    }
    size_t bl = strlen(hmBase);                   // a topic base can't end in '/' or use wildcards
    while (bl && hmBase[bl - 1] == '/') hmBase[--bl] = 0;
    if (strpbrk(hmBase, "#+")) { replyError("topic: no # or + please"); return; }
    if (in["port"].is<int>()) hmPort = constrain(in["port"].as<int>(), 1, 65535);
    if (in["on"].is<bool>()) hmOn = in["on"];
    if (in["disc"].is<bool>()) hmDiscovery = in["disc"];
    if (in["scenes"].is<JsonArray>()) {
      hmSceneCount = 0;
      for (JsonVariant v : in["scenes"].as<JsonArray>()) {
        if (hmSceneCount >= SCENES_MAX) break;
        const char* n = v | "";
        size_t k = 0;                             // what the 3x5 font can show
        for (const char* q = n; *q && k < sizeof(hmScenes[0]) - 1; q++)
          if (isalnum((unsigned char)*q) || strchr(" +-.:/?!", *q)) hmScenes[hmSceneCount][k++] = toupper((unsigned char)*q);
        hmScenes[hmSceneCount][k] = 0;
        if (k) hmSceneCount++;
      }
    }
    hmSave();
    if (mqtt.connected()) { hmPublish("status", "offline", true); mqtt.disconnect(); }   // reconnect with the new settings
    hmNextTry = 0;
    hmRetryMs = 5000;
    hmLookup.state = 0;                           // the address may have changed
    sendConfig();
    return;
  }
  if (!strcmp(cmd, "homescene")) {
    if (!mqtt.connected()) { replyError(hmStateName()); return; }
    if (!hmScene(in["n"] | 0)) { replyError("couldn't send it"); return; }
    replyOk(cmd);
    return;
  }
  if (!strcmp(cmd, "mirror"))   { mirrorOn = in["on"] | false; replyOk(cmd); return; }
  if (!strcmp(cmd, "tutorial")) { startTutorial(); replyOk(cmd); return; }
  if (!strcmp(cmd, "pet")) {
    const char* m = in["mood"] | "";
    if (screen != SCR_PET) goPet();
    if (!strcmp(m, "happy")) petEvent(EV_TAP);
    else if (!strcmp(m, "love")) petEvent(EV_TRIPLE);
    else if (!strcmp(m, "dizzy")) petEvent(EV_SHAKE);
    else if (!strcmp(m, "sleep")) petEvent(EV_ROCK);
    else if (!strcmp(m, "wake")) { petSet(PM_WAKING, 1300); petLastTouch = millis(); }
    replyOk(cmd);
    return;
  }
  replyError("unknown command");
}
