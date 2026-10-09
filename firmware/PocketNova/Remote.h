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
//                                 swiftPair)
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
//    wifi  {"action":"setup"|"stop"|"on"|"off"|"join"|"forget"}   Wi-Fi (Wifi.h)
//    mirror {"on":true}           stream the screen (@{"t":"fb",...} 10x/s)
//    tutorial                     play the tutorial
//    pet   {"mood":"happy"|"love"|"dizzy"|"sleep"|"wake"}
// =====================================================================

#include <ArduinoJson.h>
#include <sys/time.h>

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
  d["brightLevels"] = BRIGHT_COUNT;
  JsonArray a = d["apps"].to<JsonArray>();
  for (int i = 0; i < APP_COUNT; i++) a.add(apps[i].name);
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
  d["lastApp"] = cfg.lastApp;
  d["tutorialDone"] = cfg.tutorialDone;
  d["snakeHigh"] = cfg.snakeHigh;
  d["petLove"] = cfg.petLove;
  d["pX"]["axis"] = cfg.pX.axis;   d["pX"]["sign"] = cfg.pX.sign;
  d["pUp"]["axis"] = cfg.pUp.axis; d["pUp"]["sign"] = cfg.pUp.sign;
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
  if (wifiState == WF_ONLINE) w["rssi"] = WiFi.RSSI();
  if (wifiState == WF_FAILED) w["reason"] = wifiReason();
  w["setup"] = setupOn;
  if (setupOn) {
    w["apSsid"] = apSsid;
    w["apPass"] = apPass;
    w["setupLeft"] = (int32_t)(setupUntil - millis()) / 1000;
  }
  d["tiltX"] = serialized(String(tiltX - tiltBaseX, 2));
  d["tiltY"] = serialized(String(tiltY - tiltBaseY, 2));
  d["faceDown"] = faceDown;
  d["pet"] = PET_MOOD_NAMES[petMood];
  d["love"] = cfg.petLove;
  d["night"] = isNight();
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

void applySettings(JsonObject c) {
  bool nameChanged = false;
  if (c["brightIdx"].is<int>()) cfg.brightIdx = constrain(c["brightIdx"].as<int>(), 0, BRIGHT_COUNT - 1);
  if (c["rotation"].is<int>()) { cfg.rotation = c["rotation"].as<int>() & 3; onRotationChanged(); }
  if (c["autoRotate"].is<bool>()) cfg.autoRotate = c["autoRotate"].as<bool>();
  if (c["tvBrand"].is<int>()) { cfg.tvBrand = constrain(c["tvBrand"].as<int>(), 0, BRAND_COUNT); cfg.tvPower = -1; }
  if (c["swiftPair"].is<bool>()) cfg.swiftPair = c["swiftPair"].as<bool>();
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
    else { replyError("unknown wifi action"); return; }
    replyOk(cmd, a);
    sendStatus();
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
