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

bool bleRemoveBond(const char* addr) {
  uint8_t m[6];
  if (!parseMac(addr, m)) return false;
  return esp_ble_remove_bond_device(m) == ESP_OK;
}

int bleRemoveAllBonds() {
  esp_ble_bond_dev_t list[15];
  int n = bleBondList(list, 15);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  return n;
}

// Restart cleanly after changing identity/pairings.
void restartSoon(const char* why) {
  Serial.printf("[SYS] Restarting: %s\n", why);
  Serial.flush();
  delay(300);
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
