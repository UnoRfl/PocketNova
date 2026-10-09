#pragma once
// =====================================================================
//  BleCtl.h - Bluetooth identity, saved devices and pairings
// =====================================================================
//  SLOTS: a Bluetooth device is recognised by its address. Pocket Nova
//  can use 3 different addresses ("slots"), so your PC can pair with
//  slot 1, your phone with slot 2 and a laptop with slot 3. Switching
//  slot restarts the radio under the other address, and the device that
//  remembers that address reconnects.
//
//  BONDS: the pairing keys the ESP32 keeps for each host it trusts.
//  Removing them makes a host "forget" Pocket Nova (pair it again).
// =====================================================================

#include <esp_gap_ble_api.h>
#include <esp_system.h>

// Must run BEFORE bleKeyboard.begin(): picks this slot's radio address.
void bleApplySlot() {
  if (cfg.btSlot == 0) return;   // slot 1 = the chip's own address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  mac[5] = (uint8_t)(mac[5] + cfg.btSlot * 4);   // Bluetooth uses base+2, so step by 4
  esp_base_mac_addr_set(mac);
}

String macToString(const uint8_t* m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

String bleOwnAddress() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  return macToString(mac);
}

int bleBondCount() { return esp_ble_get_bond_device_num(); }

// Fills `out` with up to `max` bonded host addresses; returns how many.
int bleBondList(esp_ble_bond_dev_t* out, int max) {
  int n = max;
  if (esp_ble_get_bond_device_list(&n, out) != ESP_OK) return 0;
  return n;
}

bool parseMac(const char* s, uint8_t* m) {
  unsigned int b[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)b[i];
  return true;
}

// ---------------------------------------------------------------------
//  WHICH SLOT EACH PAIRING BELONGS TO
//  The ESP32 keeps all pairings in one list, whatever slot made them, so
//  we note the slot ourselves: whenever a paired host is connected, its
//  address is saved with the current slot (NVS area "bondslot", key = the
//  address as 12 hex digits). Phones may connect with a changing "private"
//  address that doesn't match the saved one; then a pairing with no slot
//  yet is assumed to be the one connected.
// ---------------------------------------------------------------------
void bondKey(const uint8_t* m, char* key) {   // 13 bytes
  snprintf(key, 13, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int bondSlot(const uint8_t* m) {
  char k[13];
  bondKey(m, k);
  prefs.begin("bondslot", true);
  int s = prefs.isKey(k) ? prefs.getUChar(k, 0) : -1;
  prefs.end();
  return s;
}

void bondSlotSet(const uint8_t* m, uint8_t slot) {
  char k[13];
  bondKey(m, k);
  prefs.begin("bondslot", false);
  prefs.putUChar(k, slot);
  prefs.end();
}

void bondSlotForget(const uint8_t* m) {
  char k[13];
  bondKey(m, k);
  prefs.begin("bondslot", false);
  prefs.remove(k);
  prefs.end();
}

uint8_t hostBond[6];
bool    hostBondKnown = false;     // hostBond = the pairing that's connected now

// Every 2 s: work out which pairing is connected, and remember its slot.
void bleSlotMapUpdate() {
  static uint32_t last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  uint8_t host[6];
  if (!bleKeyboard.hostAddress(host)) { hostBondKnown = false; return; }
  esp_ble_bond_dev_t list[15];
  int n = bleBondList(list, 15);
  int match = -1;
  for (int i = 0; i < n; i++) if (!memcmp(list[i].bd_addr, host, 6)) match = i;
  if (match < 0) {                 // private address: take the one pairing with no slot yet
    int unknown = -1, count = 0;
    for (int i = 0; i < n; i++) if (bondSlot(list[i].bd_addr) < 0) { unknown = i; count++; }
    if (count == 1) match = unknown;
  }
  if (match < 0) {                 // or the only pairing already on this slot
    int mine = -1, count = 0;
    for (int i = 0; i < n; i++) if (bondSlot(list[i].bd_addr) == cfg.btSlot) { mine = i; count++; }
    if (count == 1) match = mine;
  }
  if (match < 0) { hostBondKnown = false; return; }
  memcpy(hostBond, list[match].bd_addr, 6);
  hostBondKnown = true;
  if (bondSlot(hostBond) != cfg.btSlot) bondSlotSet(hostBond, cfg.btSlot);
}

bool bleRemoveBond(const char* addr) {
  uint8_t m[6];
  if (!parseMac(addr, m)) return false;
  bondSlotForget(m);
  return esp_ble_remove_bond_device(m) == ESP_OK;
}

int bleRemoveAllBonds() {
  esp_ble_bond_dev_t list[15];
  int n = bleBondList(list, 15);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  prefs.begin("bondslot", false);
  prefs.clear();
  prefs.end();
  return n;
}

// Restart cleanly after changing identity/pairings.
void restartSoon(const char* why) {
  Serial.printf("[SYS] Restarting: %s\n", why);
  Serial.flush();
  delay(150);
  ESP.restart();
}

// ---------------------------------------------------------------------
//  SWIFT PAIR WINDOW
//  The Windows "Connect?" pop-up is only broadcast while Pocket Nova is
//  ready to pair: when nothing is paired yet (first start, or after
//  "Pair new"), or for the first 3 minutes after a start (switching slot
//  restarts it, so that covers pairing a new PC on another slot).
//  After that it stays quiet, so PCs nearby that you don't want paired
//  aren't nagged. Microsoft asks devices to do the same.
// ---------------------------------------------------------------------
const uint32_t SWIFT_PAIR_WINDOW_MS = 3UL * 60 * 1000;

void swiftPairUpdate() {
  static uint32_t lastCheck = 0;
  static bool windowOver = false;
  uint32_t now = millis();
  if (now - lastCheck < 500) return;
  lastCheck = now;

  bool connected = bleKeyboard.isConnected();
  if (connected || now > SWIFT_PAIR_WINDOW_MS) windowOver = true;
  bool want = cfg.swiftPair && !connected && (!windowOver || bleBondCount() == 0);
  if (want != bleKeyboard.swiftPairOn()) {
    bleKeyboard.setSwiftPair(want);
    Serial.println(want ? "[BT] Swift Pair pop-up on" : "[BT] Swift Pair pop-up off");
  }
}
