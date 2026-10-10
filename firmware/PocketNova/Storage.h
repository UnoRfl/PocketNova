#pragma once
// =====================================================================
//  Storage.h — settings that survive a power-off
// =====================================================================
//  The ESP32 has a small key/value store in flash called NVS. The
//  Preferences library wraps it: putUChar("key", v) / getUChar("key").
// =====================================================================

#include <Preferences.h>

// Which raw accelerometer axis (0 = X, 1 = Y) and sign gives a direction.
struct AxisMap {
  uint8_t axis;
  int8_t  sign;
};

struct Config {
  uint8_t  brightIdx    = 2;          // index into BRIGHT_LEVELS
  uint8_t  rotation     = 0;          // 0..3 = 0/90/180/270 degrees
  bool     autoRotate   = false;      // turn the screen upright when held vertically
  AxisMap  pX           = {0, 1};     // physical "tilt right"  (set by Calibrate)
  AxisMap  pUp          = {1, -1};    // physical "tilt top edge down"
  uint8_t  tvBrand      = 0;          // index into BRANDS (Ir.h); BRAND_COUNT = all brands
  int16_t  tvPower      = -1;         // power code FIND TV found for an unlisted TV
  float    levelX0      = 0;          // spirit-level zero offsets
  float    levelY0      = 0;
  uint8_t  btSlot       = 0;          // 0..2: which saved Bluetooth identity to use
  bool     swiftPair    = true;       // Windows "Connect?" pop-up while pairing (BleCtl.h)
  bool     wifiOn       = false;      // join the saved Wi-Fi network (Wifi.h)
  int32_t  tz           = 0;          // time zone offset in seconds, from the PC or the setup page
  int8_t   lastApp      = 0;          // menu opens here; double-tap on the pet opens it
  bool     tutorialDone = false;
  uint16_t snakeHigh    = 0;
  uint16_t petLove      = 0;          // goes up every time you're nice to the pet
  uint8_t  mouseSpeed   = 2;          // air mouse: 0..4 (MOUSE_GAIN in Apps.h)
  uint8_t  mouseFlip    = 0;          // air mouse: bit 0 = swap left/right, bit 1 = swap up/down
  uint16_t reactBest    = 0;          // REFLEX best time in ms (0 = none yet)
  uint8_t  simonBest    = 0;          // SIMON longest sequence
  char     name[25]     = "Pocket Nova";
} cfg;

// FastLED brightness steps. The Atom Matrix LEDs get hot — M5Stack's own
// library never goes above 40, so neither do we.
const uint8_t BRIGHT_LEVELS[] = {6, 12, 20, 30, 40};
const uint8_t BRIGHT_COUNT    = sizeof(BRIGHT_LEVELS);
const uint8_t BT_SLOTS        = 3;

Preferences prefs;
const uint8_t CONFIG_VERSION = 3;   // bump when saved settings need fixing up

void saveConfig() {
  prefs.begin("multitool", false);
  prefs.putUChar("bright", cfg.brightIdx);
  prefs.putUChar("rot", cfg.rotation);
  prefs.putBool("autorot", cfg.autoRotate);
  prefs.putUChar("pxA", cfg.pX.axis);
  prefs.putChar("pxS", cfg.pX.sign);
  prefs.putUChar("puA", cfg.pUp.axis);
  prefs.putChar("puS", cfg.pUp.sign);
  prefs.putUChar("tv", cfg.tvBrand);
  prefs.putShort("tvpow", cfg.tvPower);
  prefs.putFloat("lx0", cfg.levelX0);
  prefs.putFloat("ly0", cfg.levelY0);
  prefs.putUChar("slot", cfg.btSlot);
  prefs.putBool("swift", cfg.swiftPair);
  prefs.putBool("wifi", cfg.wifiOn);
  prefs.putInt("tz", cfg.tz);
  prefs.putChar("lastapp", cfg.lastApp);
  prefs.putBool("tut", cfg.tutorialDone);
  prefs.putUShort("snake", cfg.snakeHigh);
  prefs.putUShort("love", cfg.petLove);
  prefs.putUChar("mspeed", cfg.mouseSpeed);
  prefs.putUChar("mflip", cfg.mouseFlip);
  prefs.putUShort("react", cfg.reactBest);
  prefs.putUChar("simon", cfg.simonBest);
  prefs.putString("name", cfg.name);
  prefs.putUChar("ver", CONFIG_VERSION);
  prefs.end();
}

void loadConfig() {
  prefs.begin("multitool", true);   // true = read-only
  cfg.brightIdx    = prefs.getUChar("bright", cfg.brightIdx);
  cfg.rotation     = prefs.getUChar("rot", cfg.rotation);
  cfg.autoRotate   = prefs.getBool("autorot", cfg.autoRotate);
  cfg.pX.axis      = prefs.getUChar("pxA", cfg.pX.axis);
  cfg.pX.sign      = prefs.getChar("pxS", cfg.pX.sign);
  cfg.pUp.axis     = prefs.getUChar("puA", cfg.pUp.axis);
  cfg.pUp.sign     = prefs.getChar("puS", cfg.pUp.sign);
  cfg.tvBrand      = prefs.getUChar("tv", cfg.tvBrand);
  cfg.tvPower      = prefs.getShort("tvpow", -1);
  cfg.levelX0      = prefs.getFloat("lx0", 0);
  cfg.levelY0      = prefs.getFloat("ly0", 0);
  cfg.btSlot       = prefs.getUChar("slot", 0);
  cfg.swiftPair    = prefs.getBool("swift", true);
  cfg.wifiOn       = prefs.getBool("wifi", false);
  cfg.tz           = prefs.getInt("tz", 0);
  cfg.lastApp      = prefs.getChar("lastapp", 0);
  cfg.tutorialDone = prefs.getBool("tut", false);
  cfg.snakeHigh    = prefs.getUShort("snake", 0);
  cfg.petLove      = prefs.getUShort("love", 0);
  cfg.mouseSpeed   = prefs.getUChar("mspeed", 2);
  cfg.mouseFlip    = prefs.getUChar("mflip", 0);
  cfg.reactBest    = prefs.getUShort("react", 0);
  cfg.simonBest    = prefs.getUChar("simon", 0);
  if (prefs.isKey("name")) prefs.getString("name", cfg.name, sizeof(cfg.name));
  uint8_t ver      = prefs.getUChar("ver", 0);
  bool    hadOld   = prefs.isKey("pxS");   // anything saved before?
  prefs.end();

  if (cfg.brightIdx >= BRIGHT_COUNT) cfg.brightIdx = 2;
  if (cfg.btSlot >= BT_SLOTS) cfg.btSlot = 0;
  if (cfg.rotation > 3) cfg.rotation = 0;
  if (cfg.mouseSpeed > 4) cfg.mouseSpeed = 2;

  // Settings migrations, oldest first. Each runs once, then the version
  // number is stamped so it never runs again.
  if (ver < CONFIG_VERSION) {
    if (ver < 2 && hadOld) {            // v1 saved left/right backwards
      cfg.pX.sign = -cfg.pX.sign;
      Serial.println("[CFG] Flipped left/right (one-time fix)");
    }
    if (ver < 3 && hadOld) {            // upgrading users already know the controls
      cfg.tutorialDone = true;
      strncpy(cfg.name, "Pocket Nova", sizeof(cfg.name));
    }
    saveConfig();
  }
}

// Wipes every saved setting. (Bluetooth pairings are cleared separately.)
void eraseConfig() {
  prefs.begin("multitool", false);
  prefs.clear();
  prefs.end();
}
