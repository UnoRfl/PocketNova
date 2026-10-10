#pragma once
// =====================================================================
//  NovaRemote.h - the phone remote (a Bluetooth service for a web page)
// =====================================================================
//  GATT: a Bluetooth LE device describes itself as SERVICES, each holding
//  CHARACTERISTICS (little values you can read, write or subscribe to).
//  The keyboard is the standard HID service (0x1812). This adds our own
//  service, with made-up 128-bit IDs (anyone can invent one):
//
//    CMD  (write)   the page writes a JSON command, the same ones the PC
//                   panel sends over USB, e.g. {"cmd":"input","ev":"tap"}
//    OUT  (notify)  Pocket Nova's replies, one JSON line each, split into
//                   pieces that fit one Bluetooth packet (the "MTU")
//
//  Browsers can't touch the keyboard service (Chrome blocks HID on
//  purpose, or any web page could read your typing), but they can use a
//  custom service like this one through Web Bluetooth.
//
//  SECURITY: anyone nearby could connect, so a new connection must first
//  type a 4-digit code that scrolls on the LEDs. That proves they can see
//  the device. Only harmless commands are allowed (no forgetting devices,
//  factory reset, renaming...).
//
//  PAIRED DEVICES are already trusted (they exchanged keys when pairing),
//  so they skip the code and may use every command, like the USB cable.
//  That's how the PC panel works over Bluetooth. Their connection is also
//  the keyboard connection, so it stays counted as the keyboard host.
// =====================================================================

#include <BLEDevice.h>
#include <BLE2902.h>

BLECharacteristic* remoteOut = nullptr;

// Commands arrive on the Bluetooth task; the main loop runs them. A small
// queue hands them across safely (the critical section stops both cores
// touching it at once).
const uint8_t RQ_LEN = 4;
char          rqBuf[RQ_LEN][200];
uint16_t      rqConn[RQ_LEN];
volatile uint8_t rqHead = 0, rqTail = 0;
portMUX_TYPE  rqMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t authMask = 0;              // bit n = conn_id n typed the right code
uint16_t trustMask = 0;             // bit n = conn_id n is a paired (bonded) device
uint16_t seenMask = 0;              // bit n = conn_id n has been sorted into one of the two
esp_gatt_if_t gattsIf = ESP_GATT_IF_NONE;   // our GATT server's handle (for Service Changed)
char     authCode[5] = "";          // the code on the LEDs right now ("" = none)
uint16_t authFor = 0xFFFF;          // which connection it's for
uint32_t authUntil = 0;
Scroller authScroll;

bool     replyBle = false;          // sendJson() goes to Bluetooth instead of USB
bool     bleMirror = false;         // stream the screen to the phone

class RemoteCmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c, esp_ble_gatts_cb_param_t* p) override {
    std::string v = c->getValue();
    uint16_t id = p->write.conn_id;   // sorted out (paired or remote) in remoteHandle()
    portENTER_CRITICAL(&rqMux);
    uint8_t next = (rqHead + 1) % RQ_LEN;
    if (next != rqTail) {           // full = drop it; the page will ask again
      size_t n = v.size() < sizeof(rqBuf[0]) - 1 ? v.size() : sizeof(rqBuf[0]) - 1;
      memcpy(rqBuf[rqHead], v.data(), n);
      rqBuf[rqHead][n] = 0;
      rqConn[rqHead] = id;
      rqHead = next;
    }
    portEXIT_CRITICAL(&rqMux);
  }
};

// Sees every GATT server event; we only need the server's handle.
void remoteGattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
  if (event == ESP_GATTS_CONNECT_EVT || event == ESP_GATTS_REG_EVT) gattsIf = gatts_if;
}

bool isBonded(const uint8_t* addr) {
  esp_ble_bond_dev_t list[15];
  int n = bleBondList(list, 15);
  for (int i = 0; i < n; i++) if (!memcmp(list[i].bd_addr, addr, 6)) return true;
  return false;
}

// Called by the keyboard while it sets up its Bluetooth server.
void remoteBegin(BLEServer* s) {
  BLEDevice::setCustomGattsHandler(remoteGattsEvent);
  BLEDevice::setMTU(247);           // ask for bigger packets: fewer pieces per reply
  BLEService* svc = s->createService(BLEUUID(REMOTE_SVC));
  BLECharacteristic* cmd = svc->createCharacteristic(BLEUUID(REMOTE_CMD),
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(new RemoteCmdCallbacks());
  remoteOut = svc->createCharacteristic(BLEUUID(REMOTE_OUT), BLECharacteristic::PROPERTY_NOTIFY);
  remoteOut->addDescriptor(new BLE2902());   // the "subscribe" switch for notifications
  svc->start();
}

// Sends one line to the remote page(s), in packet-sized pieces. The page
// glues pieces together until it sees the newline.
void remoteNotify(const String& line) {
  uint16_t to = bleKeyboard.remotes() | trustMask;   // code-approved remotes and paired devices
  if (!remoteOut || !to) return;
  uint16_t mtu = 247;
  for (auto& peer : bleKeyboard.getServer()->getPeerDevices(false)) {
    uint16_t id = peer.first;
    if (id < 16 && (to & (1u << id)) && peer.second.mtu < mtu) mtu = peer.second.mtu;
  }
  size_t piece = mtu > 23 ? mtu - 3 : 20;
  String all = line + "\n";
  for (size_t i = 0; i < all.length(); i += piece) {
    String part = all.substring(i, i + piece);
    remoteOut->setValue((uint8_t*)part.c_str(), part.length());
    remoteOut->notify();
    delay(4);                       // give the radio a moment between pieces
  }
}

void remoteReply(const char* t, const char* key = nullptr, const char* val = nullptr) {
  JsonDocument d;
  d["t"] = t;
  if (key) d[key] = val;
  remoteNotify(atJson(d));
}

void remoteAskCode(uint16_t id) {
  if (!authCode[0] || authFor != id || (int32_t)(millis() - authUntil) > 0) {
    for (int i = 0; i < 4; i++) authCode[i] = '0' + esp_random() % 10;
    authCode[4] = 0;
    authFor = id;
    authUntil = millis() + 60000;
    Serial.printf("[REMOTE] A phone wants to connect: code %s (also on the LEDs)%c", authCode, 10);
  }
  remoteReply("auth", "need", "code");
}

// Commands a remote page may use. Everything that forgets, resets or
// renames stays USB-only.
bool remoteAllowed(const char* c) {
  static const char* const OK[] = {"hello", "get", "status", "set", "input", "tilt", "ir",
                                   "tvbrands", "findtv", "mirror", "pet", "time", "tutorial"};
  for (const char* k : OK) if (!strcmp(c, k)) return true;
  return false;
}

void remoteHandle(const char* line, uint16_t id) {
  JsonDocument in;
  if (deserializeJson(in, line)) { remoteReply("error", "msg", "bad json"); return; }
  const char* cmd = in["cmd"] | "";
  if (id < 16 && !(seenMask & (1u << id))) {   // first message on this connection
    seenMask |= 1u << id;
    uint8_t a[6];
    if (bleKeyboard.peerAddress(id, a) && isBonded(a)) {
      trustMask |= 1u << id;                  // paired: trusted, and still the keyboard host
      authMask |= 1u << id;
      Serial.println("[REMOTE] Paired device connected to the remote service");
    } else {
      bleKeyboard.markRemote(id);             // a remote page, not the keyboard host
    }
  }
  bool trusted = id < 16 && (trustMask & (1u << id));
  bool authed = id < 16 && (authMask & (1u << id));

  if (!strcmp(cmd, "auth")) {
    if (authCode[0] && authFor == id && (int32_t)(millis() - authUntil) <= 0 &&
        !strcmp(in["code"] | "", authCode)) {
      authMask |= 1u << id;
      authCode[0] = 0;
      authScroll.stop();
      uint8_t a[6];
      histAdd(H_CODE_OK, bleKeyboard.peerAddress(id, a) ? a : nullptr);
      fxRipple(CRGB::Green);
      Serial.println("[REMOTE] Phone connected");
      remoteReply("auth", "ok", "yes");
    } else {
      uint8_t a[6];
      histAdd(H_CODE_BAD, bleKeyboard.peerAddress(id, a) ? a : nullptr);
      authCode[0] = 0;              // a wrong guess gets a new code
      fxError();
      remoteAskCode(id);
    }
    return;
  }
  if (!authed) { remoteAskCode(id); return; }
  if (!trusted && !remoteAllowed(cmd)) { remoteReply("error", "msg", "not allowed from the phone"); return; }
  if (!strcmp(cmd, "mirror")) { bleMirror = in["on"] | false; remoteReply("ok", "cmd", "mirror"); return; }
  if (!trusted && !strcmp(cmd, "set")) in["cfg"].remove("name");   // renaming restarts: USB or paired only

  char buf[200];
  serializeJson(in, buf, sizeof(buf));
  replyBle = true;
  handleRemoteLine(buf);
  replyBle = false;
}

// Main loop: run queued commands, tidy up, stream the screen.
void remoteUpdate() {
  while (rqTail != rqHead) {
    char line[200];
    uint16_t id;
    portENTER_CRITICAL(&rqMux);
    memcpy(line, rqBuf[rqTail], sizeof(line));
    id = rqConn[rqTail];
    rqTail = (rqTail + 1) % RQ_LEN;
    portEXIT_CRITICAL(&rqMux);
    remoteHandle(line, id);
  }
  // Forget connections that closed (the keyboard keeps a list of open ones).
  uint16_t open = 0;
  uint8_t tmp[6];
  for (int i = 0; i < 16; i++) if (bleKeyboard.peerAddress(i, tmp)) open |= 1u << i;
  authMask &= open; trustMask &= open; seenMask &= open;
  if (!authMask) bleMirror = false;
  if (authCode[0] && (!(bleKeyboard.remotes() & (1u << authFor)) || (int32_t)(millis() - authUntil) > 0)) {
    authCode[0] = 0;
    authScroll.stop();
  }

  static uint32_t lastFrame = 0;
  if (bleMirror && millis() - lastFrame >= 200) {
    lastFrame = millis();
    static const char HEX_[] = "0123456789abcdef";
    String s = "@{\"t\":\"fb\",\"d\":\"";
    for (int i = 0; i < 25; i++) {
      const uint8_t c[3] = {fb[i].r, fb[i].g, fb[i].b};
      for (int k = 0; k < 3; k++) { s += HEX_[c[k] >> 4]; s += HEX_[c[k] & 15]; }
    }
    s += "\"}";
    remoteNotify(s);
  }
}

// Draws the code over whatever is on screen while a phone is waiting.
void remoteDrawOverlay() {
  if (!authCode[0]) return;
  if (!authScroll.active) {
    char t[16];
    snprintf(t, sizeof(t), "CODE %s", authCode);
    authScroll.start(t, CRGB(0, 160, 255));
  }
  clearFb();
  authScroll.draw();
}
