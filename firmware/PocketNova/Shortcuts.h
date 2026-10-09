#pragma once
// =====================================================================
//  Shortcuts.h - the Keys app's shortcut library, and the ones you picked
// =====================================================================
//  A keyboard shortcut is some MODIFIER keys (Ctrl, Shift, Alt, Win) held
//  down while one more key is pressed. Over Bluetooth the keyboard sends
//  a little "report" saying which keys are down: first one with
//  everything pressed, then an empty one ("all keys up").
//
//  The Keys app shows up to KEY_LIST_MAX of these, in the order you pick
//  in the PC panel. Three CUSTOM slots hold any combo you set up there.
//  Your picks are saved in NVS (area "keys").
// =====================================================================

enum : uint8_t { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4, M_WIN = 8 };
const uint8_t KEY_PRTSC = 0xCE;   // Print Screen (HID usage 0x46 + the library's 0x88 offset)

struct Shortcut {
  const char* name;      // scrolls on the LEDs (3x5 font: letters, digits, + - . : / ?)
  const char* sprite;
  uint8_t     mods;      // M_ bits
  uint8_t     key;       // ASCII, or one of the KEY_ constants
  uint8_t     media;     // 1 = the Calculator media key instead of a combo
  bool        confirm;   // needs a second tap within 2 s
};

const Shortcut SHORTCUTS[] = {
  {"LOCK PC",    SPR_LOCK,     M_WIN,           'l',             0, true},
  {"SNIP",       SPR_SNIP,     M_WIN | M_SHIFT, 's',             0, false},
  {"DESKTOP",    SPR_DESK,     M_WIN,           'd',             0, false},
  {"TASK MGR",   SPR_TASK,     M_CTRL | M_SHIFT, KEY_ESC,        0, false},
  {"CALC",       SPR_CALC,     0,               0,               1, false},
  {"MIC MUTE",   SPR_MIC,      M_WIN | M_ALT,   'k',             0, false},   // Windows 11 calls
  {"SWITCH APP", SPR_ALTTAB,   M_ALT,           KEY_TAB,         0, false},
  {"TASK VIEW",  SPR_TASKVIEW, M_WIN,           KEY_TAB,         0, false},
  {"FILES",      SPR_FILES,    M_WIN,           'e',             0, false},
  {"CLIPBOARD",  SPR_CLIP,     M_WIN,           'v',             0, false},
  {"EMOJI",      SPR_EMOJI,    M_WIN,           '.',             0, false},
  {"SCREENSHOT", SPR_SHOT,     M_WIN,           KEY_PRTSC,       0, false},   // saved to Pictures
  {"RECORD",     SPR_REC,      M_WIN | M_SHIFT, 'r',             0, false},   // Snipping Tool video
  {"VOICE TYPE", SPR_VOICE,    M_WIN,           'h',             0, false},
  {"DESK LEFT",  SPR_DESKL,    M_WIN | M_CTRL,  KEY_LEFT_ARROW,  0, false},
  {"DESK RIGHT", SPR_DESKR,    M_WIN | M_CTRL,  KEY_RIGHT_ARROW, 0, false},
  {"NEW TAB",    SPR_NEWTAB,   M_CTRL,          't',             0, false},
  {"REOPEN TAB", SPR_REOPEN,   M_CTRL | M_SHIFT, 't',            0, false},
  {"MINIMIZE",   SPR_MIN,      M_WIN,           KEY_DOWN_ARROW,  0, false},
  {"CLOSE",      SPR_CLOSE,    M_ALT,           KEY_F4,          0, true},    // Alt+F4
  {"SETTINGS",   SPR_GEAR,     M_WIN,           'i',             0, false},
  {"COPY",       SPR_COPY,     M_CTRL,          'c',             0, false},
  {"PASTE",      SPR_PASTE,    M_CTRL,          'v',             0, false},
  {"UNDO",       SPR_UNDO,     M_CTRL,          'z',             0, false},
  {"GAME CLIP",  SPR_GAME,     M_WIN | M_ALT,   'g',             0, false},   // Xbox Game Bar: last 30 s
  {"NOTIFY",     SPR_BELL,     M_WIN,           'n',             0, false},
  {"QUICK SET",  SPR_QUICK,    M_WIN,           'a',             0, false},
};
const uint8_t SHORTCUT_COUNT = sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]);

// IDs: 0..SHORTCUT_COUNT-1 = the library, CUSTOM_ID+0..2 = your own.
const uint8_t CUSTOM_ID    = 100;
const uint8_t CUSTOM_SLOTS = 3;
const uint8_t KEY_LIST_MAX = 12;

struct CustomKey {
  char    name[12];
  uint8_t mods;
  uint8_t key;           // 0 = slot empty
};
CustomKey customKeys[CUSTOM_SLOTS];

uint8_t keyList[KEY_LIST_MAX];
uint8_t keyListLen = 0;
const uint8_t DEFAULT_KEYS[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 11};

bool keyIdValid(uint8_t id) {
  if (id < SHORTCUT_COUNT) return true;
  return id >= CUSTOM_ID && id < CUSTOM_ID + CUSTOM_SLOTS && customKeys[id - CUSTOM_ID].key;
}
const char* keyName(uint8_t id) {
  if (id < SHORTCUT_COUNT) return SHORTCUTS[id].name;
  if (id >= CUSTOM_ID && id < CUSTOM_ID + CUSTOM_SLOTS) return customKeys[id - CUSTOM_ID].name;
  return "?";
}
bool keyNeedsConfirm(uint8_t id) { return id < SHORTCUT_COUNT && SHORTCUTS[id].confirm; }

void keysSave() {
  prefs.begin("keys", false);
  prefs.putBytes("list", keyList, keyListLen);
  prefs.putUChar("n", keyListLen);
  prefs.putBytes("custom", customKeys, sizeof(customKeys));
  prefs.end();
}

void keysLoad() {
  memset(customKeys, 0, sizeof(customKeys));
  prefs.begin("keys", true);
  bool have = prefs.isKey("n");
  if (have) {
    keyListLen = min<uint8_t>(prefs.getUChar("n", 0), KEY_LIST_MAX);
    prefs.getBytes("list", keyList, keyListLen);
    if (prefs.getBytesLength("custom") == sizeof(customKeys)) prefs.getBytes("custom", customKeys, sizeof(customKeys));
  }
  prefs.end();
  for (auto& c : customKeys) c.name[sizeof(c.name) - 1] = 0;
  if (!have) {                       // first start: the default set
    keyListLen = sizeof(DEFAULT_KEYS);
    memcpy(keyList, DEFAULT_KEYS, keyListLen);
  }
  uint8_t n = 0;                     // drop anything no longer valid
  for (uint8_t i = 0; i < keyListLen; i++) if (keyIdValid(keyList[i])) keyList[n++] = keyList[i];
  keyListLen = n;
}

// Press the modifiers, then the key, then let go of everything.
void sendKeys(uint8_t mods, uint8_t key) {
  if (mods & M_CTRL)  bleKeyboard.press(KEY_LEFT_CTRL);
  if (mods & M_SHIFT) bleKeyboard.press(KEY_LEFT_SHIFT);
  if (mods & M_ALT)   bleKeyboard.press(KEY_LEFT_ALT);
  if (mods & M_WIN)   bleKeyboard.press(KEY_LEFT_GUI);
  if (key) bleKeyboard.press(key);
  delay(40);
  bleKeyboard.releaseAll();
}

void keyFire(uint8_t id) {
  if (id < SHORTCUT_COUNT) {
    const Shortcut& s = SHORTCUTS[id];
    if (s.media == 1) bleKeyboard.write(KEY_MEDIA_CALCULATOR);
    else sendKeys(s.mods, s.key);
  } else if (keyIdValid(id)) {
    sendKeys(customKeys[id - CUSTOM_ID].mods, customKeys[id - CUSTOM_ID].key);
  }
  Serial.printf("[KEYS] %s\n", keyName(id));
}
