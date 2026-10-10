#pragma once
// =====================================================================
//  Home.h - smart home over MQTT (Home Assistant, Node-RED, openHAB...)
// =====================================================================
//  MQTT is a small "post office" protocol. One server, the BROKER (in
//  Home Assistant it's the Mosquitto add-on), passes messages around.
//  Devices PUBLISH a message to a TOPIC, a name like a file path, and
//  whoever SUBSCRIBED to that topic gets it. Nobody talks directly.
//
//  Pocket Nova's topics (base = "pocketnova/<router name>" unless you pick one):
//    <base>/status   "online" / "offline" (the broker posts "offline" for
//                    us if we vanish: the "last will")
//    <base>/scene    the scene name, each time you pick one in the HOME app
//    <base>/sensor   {"temperature":21.5,"humidity":40} every minute (Grove sensor)
//    <base>/net      {"router_ms":3,"internet_ms":25,"internet":"up"} every minute
//    <base>/alert    WE LISTEN: post text here and it pops up on the LEDs
//                    (or {"kind":"note","text":"..."})
//
//  HOME ASSISTANT DISCOVERY: we also post small "config" messages under
//  homeassistant/... so Home Assistant adds Pocket Nova by itself, with a
//  "button pressed" trigger per scene and the sensors.
//
//  Plain MQTT (port 1883) isn't encrypted: fine on your home network,
//  not across the internet.
// =====================================================================

#include <PubSubClient.h>

const uint8_t SCENES_MAX = 6;
bool     hmOn = false;                // settings (NVS "home")
char     hmHost[64] = "";
uint16_t hmPort = 1883;
char     hmUser[32] = "";
char     hmPass[64] = "";
char     hmBase[48] = "";             // "" = automatic
bool     hmDiscovery = true;
char     hmScenes[SCENES_MAX][16];
uint8_t  hmSceneCount = 0;

WiFiClient   hmNet;
PubSubClient mqtt(hmNet);
char     hmTopic[48];                 // the base actually used
char     hmId[24];                    // unique id for Home Assistant
uint32_t hmNextTry = 0, hmRetryMs = 5000, hmPubAt = 0;
int      hmLastState = MQTT_DISCONNECTED;
bool     hmSensorAnnounced = false;
uint32_t hmSent = 0, hmGot = 0;
Lookup   hmLookup;                    // the broker's address (NetTools.h)

void hmLoad() {
  prefs.begin("home", true);
  hmOn = prefs.getBool("on", false);
  if (prefs.isKey("host")) prefs.getString("host", hmHost, sizeof(hmHost));
  hmPort = prefs.getUShort("port", 1883);
  if (prefs.isKey("user")) prefs.getString("user", hmUser, sizeof(hmUser));
  if (prefs.isKey("pass")) prefs.getString("pass", hmPass, sizeof(hmPass));
  if (prefs.isKey("base")) prefs.getString("base", hmBase, sizeof(hmBase));
  hmDiscovery = prefs.getBool("disc", true);
  hmSceneCount = 0;
  for (int i = 0; i < SCENES_MAX; i++) {
    char k[4] = {'s', char('0' + i), 0};
    hmScenes[i][0] = 0;
    if (prefs.isKey(k)) prefs.getString(k, hmScenes[i], sizeof(hmScenes[i]));
    if (hmScenes[i][0]) hmSceneCount = i + 1;
  }
  prefs.end();
  if (!hmSceneCount) {                // a starting set, renamed in the panel
    strcpy(hmScenes[0], "LIGHTS");
    strcpy(hmScenes[1], "MOVIE");
    strcpy(hmScenes[2], "GOOD NIGHT");
    hmSceneCount = 3;
  }
}
void hmSave() {
  prefs.begin("home", false);
  prefs.putBool("on", hmOn);
  prefs.putString("host", hmHost);
  prefs.putUShort("port", hmPort);
  prefs.putString("user", hmUser);
  prefs.putString("pass", hmPass);
  prefs.putString("base", hmBase);
  prefs.putBool("disc", hmDiscovery);
  for (int i = 0; i < SCENES_MAX; i++) {
    char k[4] = {'s', char('0' + i), 0};
    prefs.putString(k, i < hmSceneCount ? hmScenes[i] : "");
  }
  prefs.end();
}

const char* hmStateName() {
  if (!hmOn) return "off";
  if (!hmHost[0]) return "not set up";
  if (wifiState != WF_ONLINE) return "waiting for Wi-Fi";
  if (mqtt.connected()) return "connected";
  switch (hmLastState) {
    case MQTT_CONNECTION_TIMEOUT:     return "broker didn't answer in time";
    case MQTT_CONNECTION_LOST:        return "connection lost";
    case MQTT_CONNECT_FAILED:         return "can't reach the broker (check the address and port)";
    case MQTT_CONNECT_BAD_PROTOCOL:   return "broker refused the MQTT version";
    case MQTT_CONNECT_BAD_CLIENT_ID:  return "broker refused the client name";
    case MQTT_CONNECT_UNAVAILABLE:    return "broker unavailable";
    case MQTT_CONNECT_BAD_CREDENTIALS:return "wrong username or password";
    case MQTT_CONNECT_UNAUTHORIZED:   return "not allowed (check the user's rights)";
    default:                          return "connecting";
  }
}

void hmMakeTopics() {
  if (hmBase[0]) strcpy(hmTopic, hmBase);
  else {
    snprintf(hmTopic, sizeof(hmTopic), "pocketnova/%s", wifiHostname);
    for (char* p = hmTopic; *p; p++) *p = tolower((unsigned char)*p);
  }
  uint8_t m[6];
  WiFi.macAddress(m);
  snprintf(hmId, sizeof(hmId), "pocketnova_%02x%02x%02x", m[3], m[4], m[5]);
}

bool hmPublish(const char* sub, const char* payload, bool retain = false) {
  if (!mqtt.connected()) return false;
  char t[80];
  snprintf(t, sizeof(t), "%s/%s", hmTopic, sub);
  bool ok = mqtt.publish(t, payload, retain);
  if (ok) hmSent++;
  return ok;
}

void hmDevice(JsonObject d) {
  d["identifiers"][0] = hmId;
  d["name"] = cfg.name;
  d["manufacturer"] = "Pocket Nova";
  d["model"] = "M5Stack Atom Matrix";
  d["sw_version"] = FW_VERSION;
}

// One retained config message per thing Home Assistant should create.
void hmDiscover() {
  if (!hmDiscovery || !mqtt.connected()) return;
  char t[120], st[80];
  String out;
  for (int i = 0; i < SCENES_MAX; i++) {
    snprintf(t, sizeof(t), "homeassistant/device_automation/%s/scene%d/config", hmId, i);
    if (i >= hmSceneCount || !hmScenes[i][0]) { mqtt.publish(t, "", true); continue; }   // empty = remove it
    JsonDocument d;
    d["automation_type"] = "trigger";
    snprintf(st, sizeof(st), "%s/scene", hmTopic);
    d["topic"] = st;
    d["payload"] = hmScenes[i];
    d["type"] = "button_short_press";
    d["subtype"] = hmScenes[i];
    hmDevice(d["device"].to<JsonObject>());
    out = "";
    serializeJson(d, out);
    mqtt.publish(t, out.c_str(), true);
  }
  struct { const char* key; const char* name; const char* topic; const char* tmpl; const char* unit; const char* cls; bool need; } S[] = {
    {"temperature", "Temperature", "sensor", "{{ value_json.temperature }}", "\xC2\xB0" "C", "temperature", sensorOk()},
    {"humidity", "Humidity", "sensor", "{{ value_json.humidity }}", "%", "humidity", sensorOk()},
    {"internet_ms", "Internet latency", "net", "{{ value_json.internet_ms }}", "ms", nullptr, nmOn},
    {"router_ms", "Router latency", "net", "{{ value_json.router_ms }}", "ms", nullptr, nmOn},
  };
  for (auto& s : S) {
    snprintf(t, sizeof(t), "homeassistant/sensor/%s/%s/config", hmId, s.key);
    if (!s.need) continue;
    JsonDocument d;
    d["name"] = s.name;
    snprintf(st, sizeof(st), "%s/%s", hmTopic, s.topic);
    d["state_topic"] = st;
    d["value_template"] = s.tmpl;
    d["unit_of_measurement"] = s.unit;
    if (s.cls) d["device_class"] = s.cls;
    d["state_class"] = "measurement";
    snprintf(st, sizeof(st), "%s_%s", hmId, s.key);
    d["unique_id"] = st;
    snprintf(st, sizeof(st), "%s/status", hmTopic);
    d["availability_topic"] = st;
    hmDevice(d["device"].to<JsonObject>());
    out = "";
    serializeJson(d, out);
    mqtt.publish(t, out.c_str(), true);
  }
  hmSensorAnnounced = sensorOk();
}

void hmOnMessage(char* topic, byte* payload, unsigned int len) {
  hmGot++;
  char msg[96];
  len = min<unsigned>(len, sizeof(msg) - 1);
  memcpy(msg, payload, len);
  msg[len] = 0;
  JsonDocument d;
  if (msg[0] == '{' && !deserializeJson(d, msg)) alertShow(d["kind"] | "note", d["text"] | "");
  else alertShow("note", msg);
}

void hmPublishReadings() {
  char buf[96];
  if (sensorOk()) {
    snprintf(buf, sizeof(buf), "{\"temperature\":%.1f,\"humidity\":%.0f}", senTemp, senHum);
    hmPublish("sensor", buf, true);
    if (!hmSensorAnnounced) hmDiscover();             // the sensor was plugged in after we connected
  }
  if (nmOn && nmCount) {
    int16_t r = nmLast(nmRouter), n = nmLast(nmNet);
    snprintf(buf, sizeof(buf), "{\"router_ms\":%d,\"internet_ms\":%d,\"internet\":\"%s\"}",
             max<int16_t>(r, -1), max<int16_t>(n, -1), strcmp(nmState(nmNet), "down") ? "up" : "down");
    hmPublish("net", buf, true);
  }
}

void homeUpdate() {
  uint32_t now = millis();
  if (!hmOn || !hmHost[0] || wifiState != WF_ONLINE) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (!mqtt.connected()) {
    if ((int32_t)(now - hmNextTry) < 0) return;
    // First the broker's address, looked up in the background (no freezing).
    if (hmLookup.state == 0 || strcmp(hmLookup.name, hmHost)) {
      if (!lookupStart(hmLookup, hmHost)) return;    // another lookup is running: next frame
    }
    if (hmLookup.state == 1) return;
    if (hmLookup.state == 3) {
      hmLookup.state = 0;
      hmLastState = MQTT_CONNECT_FAILED;
      hmNextTry = now + hmRetryMs;
      hmRetryMs = min<uint32_t>(hmRetryMs * 2, 60000);
      Serial.printf("[HOME] Couldn't find %s on the network\n", hmHost);
      return;
    }
    hmMakeTopics();
    mqtt.setServer(hmLookup.ip, hmPort);
    mqtt.setCallback(hmOnMessage);
    mqtt.setBufferSize(768);
    mqtt.setSocketTimeout(3);
    char will[64];
    snprintf(will, sizeof(will), "%s/status", hmTopic);
    Serial.printf("[HOME] Connecting to %s:%u\n", hmHost, hmPort);
    // A dead address can hold the loop for up to 3 s (the TCP connect timeout); retries back off to once a minute.
    bool ok = mqtt.connect(hmId, hmUser[0] ? hmUser : nullptr, hmUser[0] ? hmPass : nullptr, will, 0, true, "offline");
    hmLastState = mqtt.state();
    if (!ok) {
      hmLookup.state = 0;                            // look the address up again next time
      hmNextTry = now + hmRetryMs;
      hmRetryMs = min<uint32_t>(hmRetryMs * 2, 60000);
      Serial.printf("[HOME] Couldn't connect: %s\n", hmStateName());
      return;
    }
    hmRetryMs = 5000;
    Serial.printf("[HOME] Connected, topics under %s/\n", hmTopic);
    hmPublish("status", "online", true);
    char t[64];
    snprintf(t, sizeof(t), "%s/alert", hmTopic);
    mqtt.subscribe(t);
    hmDiscover();
    hmPubAt = 0;
  }
  mqtt.loop();
  hmLastState = mqtt.state();
  if ((int32_t)(now - hmPubAt) >= 0) { hmPubAt = now + 60000; hmPublishReadings(); }
}

bool hmScene(int i) {
  if (i < 0 || i >= hmSceneCount) return false;
  bool ok = hmPublish("scene", hmScenes[i]);
  Serial.printf("[HOME] Scene %s %s\n", hmScenes[i], ok ? "sent" : "not sent (not connected)");
  return ok;
}

// ---------------------------------------------------------------------
//  HOME app: tilt through your scenes, tap to send one. The top-right
//  corner shows the link: green = connected, red = not.
// ---------------------------------------------------------------------
Carousel homeMenu;
Scroller homeScroll;
const CRGB SCENE_COL[SCENES_MAX] = {CRGB(255, 200, 0), CRGB(160, 60, 255), CRGB(0, 120, 255),
                                    CRGB(0, 255, 80), CRGB(255, 60, 160), CRGB(0, 220, 255)};

void homeDrawItem(int i, int xo) {
  drawGlyph(hmScenes[i][0], xo + 1, 0, SCENE_COL[i]);
  if (xo == 0) px(4, 0, mqtt.connected() ? CRGB(0, 160, 40) : CRGB(160, 0, 0));
}
const char* homeName(int i) { return hmScenes[i]; }
void homeEnter() { homeMenu.reset(max<int>(hmSceneCount, 1)); homeScroll.stop(); }

bool homeFrame(Event e) {
  if (e == EV_HOLD) return false;
  clearFb();
  if (!hmOn || !hmHost[0]) {
    if (!homeScroll.draw()) homeScroll.start("SET UP SMART HOME IN THE PANEL", CRGB(255, 200, 0));
    return true;
  }
  if (homeMenu.count != hmSceneCount) homeMenu.reset(max<int>(hmSceneCount, 1));
  if (e == EV_LEFT)  homeMenu.move(-1);
  if (e == EV_RIGHT) homeMenu.move(+1);
  if (e == EV_TAP) {
    if (hmScene(homeMenu.index)) { flash(); fxRipple(SCENE_COL[homeMenu.index]); }
    else { fxError(); homeMenu.showLabelNow(wifiState != WF_ONLINE ? "NO WI-FI" : "NOT CONNECTED", CRGB(255, 60, 0)); }
  }
  homeMenu.draw(homeDrawItem, homeName, SCENE_COL[homeMenu.index]);
  return true;
}
