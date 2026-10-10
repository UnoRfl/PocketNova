/*
 * ============================================================================
 *  Pocket Nova  -  firmware v2 for the M5Stack Atom Matrix (ESP32-PICO-D4)
 * ============================================================================
 *  A pocket multitool with a pet called Nova living on the 5x5 matrix.
 *
 *  CONTROLS (the whole top face is the button):
 *    Hold ~0.7 s     pet -> menu, and back one step everywhere else
 *    Tap             open / do the thing
 *    Double tap      on the pet: jump straight into your last app
 *    Tilt            one step left/right per tilt (~37 deg), then level again
 *
 *  SCREENS:  first boot: TUTORIAL  ->  PET (home)  <->  MENU  <->  APP
 *
 *  PC APP:   the "Pocket Nova Panel" on Windows talks to this firmware over
 *            USB with one-line JSON commands (see Remote.h) and opens itself
 *            whenever Pocket Nova is plugged in.
 *
 *  FILES:
 *    Storage.h  saved settings       Font.h    3x5 letters
 *    Sprites.h  all pictures         Display.h drawing + animation helpers
 *    Input.h    button/tilt events   Ir.h      TV code table
 *    BleCtl.h   Bluetooth slots and pairings
 *    Wifi.h     Wi-Fi setup page and internet time
 *    Ota.h      firmware updates over Wi-Fi
 *    Sensor.h   temperature/humidity sensor on the Grove port
 *    NetTools.h Wi-Fi channel analyzer and network health monitor
 *    Finder.h   find a Bluetooth device by its signal
 *    Home.h     smart home over MQTT (Home Assistant)
 *    Shortcuts.h  the Keys app's shortcut library and your picks
 *    NovaRemote.h  Bluetooth service for the phone remote web page
 *    Apps.h     every app            Pet.h     Nova the pet
 *    Remote.h   commands from the PC app
 *
 *  BUILD: board "M5Stack-ATOM", Partition Scheme "Minimal SPIFFS",
 *         ESP32 core 2.0.17, FastLED 3.6.0, M5Atom, IRremoteESP8266,
 *         ArduinoJson 7. (The Bluetooth keyboard code is bundled: NovaKeyboard.*)
 * ============================================================================
 */

#include <M5Atom.h>
#include "NovaKeyboard.h"   // our fixed copy of ESP32-BLE-Keyboard (see NovaKeyboard.cpp)
#include <IRremoteESP8266.h>
#include <IRsend.h>

#include "Storage.h"
#include "Font.h"
#include "Sprites.h"
#include "Display.h"
#include "Input.h"
#include "Ir.h"
#include <esp_task_wdt.h>

const char* const FW_VERSION = "2.15.1";

// Why Pocket Nova last started (for the panel and the history).
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "froze (watchdog)";
    case ESP_RST_BROWNOUT: return "power dip";
    default:               return "other";
  }
}

// The phone remote's Bluetooth service and its two characteristics
// (NovaRemote.h). Made-up IDs; the web page uses the same ones.
const char* const REMOTE_SVC = "8f3c0001-5b2a-4c8e-9a71-2f6d1e0b9a10";
const char* const REMOTE_CMD = "8f3c0002-5b2a-4c8e-9a71-2f6d1e0b9a10";
const char* const REMOTE_OUT = "8f3c0003-5b2a-4c8e-9a71-2f6d1e0b9a10";

class BLEServer;
void remoteBegin(BLEServer* s);   // NovaRemote.h

// The keyboard, plus our remote service added while it sets up Bluetooth.
class PocketKeyboard : public NovaKeyboard {
 public:
  using NovaKeyboard::NovaKeyboard;
 protected:
  void onStarted(BLEServer* s) override { remoteBegin(s); }
};

// The name is replaced by the saved one (cfg.name) in setup().
PocketKeyboard bleKeyboard("Pocket Nova", "M5Stack", 100);

#include "BleCtl.h"
#include "History.h"
#include "Wifi.h"
#include "Ota.h"
#include "Shortcuts.h"
#include "Apps.h"
#include "Pet.h"
#include "Sensor.h"
#include "NetTools.h"
#include "Finder.h"
#include "Home.h"

// ----------------------------------------------------------------------------
//  The app list. To add an app: write its functions in Apps.h, draw an
//  icon in Sprites.h, and add one line here.
// ----------------------------------------------------------------------------
struct App {
  const char*        name;
  CRGB               color;     // colour of its name label
  const char* const* icon;      // animation frames
  uint8_t            frames;
  void (*enter)();
  bool (*frame)(Event);
  void (*leave)();
  bool               multiTap;  // wants double/triple taps (costs a short wait on single taps)
};

App apps[] = {
  {"MEDIA",    CRGB(160, 0, 255),   ICON_MEDIA,  FRAMES(ICON_MEDIA),  mediaEnter,    mediaFrame,    nullptr,    true},
  {"TV",       CRGB(150, 0, 255),   ICON_TV,     FRAMES(ICON_TV),     tvEnter,       tvFrame,       nullptr,    false},
  {"SLIDES",   CRGB(0, 255, 80),    ICON_SLIDES, FRAMES(ICON_SLIDES), nullptr,       slidesFrame,   nullptr,    false},
  {"AIR MOUSE",CRGB(255, 255, 255), ICON_MOUSE,  FRAMES(ICON_MOUSE),  mouseEnter,    mouseFrame,    nullptr,    false},
  {"KEYS",     CRGB(255, 120, 0),   ICON_KEYS,   FRAMES(ICON_KEYS),   keysEnter,     keysFrame,     nullptr,    false},
  {"HOME",     CRGB(255, 200, 0),   ICON_HOME,   FRAMES(ICON_HOME),   homeEnter,     homeFrame,     nullptr,    false},
  {"LEVEL",    CRGB(0, 255, 0),     ICON_LEVEL,  FRAMES(ICON_LEVEL),  nullptr,       levelFrame,    nullptr,    false},
  {"DICE",     CRGB(255, 255, 255), ICON_DICE,   FRAMES(ICON_DICE),   nullptr,       diceFrame,     nullptr,    false},
  {"TIMER",    CRGB(255, 200, 0),   ICON_TIMER,  FRAMES(ICON_TIMER),  timerEnter,    timerFrame,    nullptr,    false},
  {"LIGHTS",   CRGB(255, 0, 180),   ICON_LIGHT,  FRAMES(ICON_LIGHT),  lightEnter,    lightFrame,    nullptr,    false},
  {"TORCH",    CRGB(255, 255, 255), ICON_TORCH,  FRAMES(ICON_TORCH),  torchEnter,    torchFrame,    torchLeave, false},
  {"CLIMATE",  CRGB(0, 200, 255),   ICON_CLIMATE,FRAMES(ICON_CLIMATE),climateEnter,  climateFrame,  nullptr,    false},
  {"NETWORK",  CRGB(0, 255, 80),    ICON_NET,    FRAMES(ICON_NET),    netEnter,      netFrame,      nullptr,    false},
  {"CHANNELS", CRGB(255, 200, 0),   ICON_CHAN,   FRAMES(ICON_CHAN),   chanEnter,     chanFrame,     nullptr,    false},
  {"FINDER",   CRGB(0, 120, 255),   ICON_FIND,   FRAMES(ICON_FIND),   finderEnter,   finderFrame,   finderLeave,false},
  {"SNAKE",    CRGB(0, 255, 0),     ICON_SNAKE,  FRAMES(ICON_SNAKE),  snakeEnter,    snakeFrame,    nullptr,    false},
  {"REFLEX",   CRGB(0, 255, 0),     ICON_REFLEX, FRAMES(ICON_REFLEX), reflexEnter,   reflexFrame,   nullptr,    false},
  {"SIMON",    CRGB(255, 200, 0),   ICON_SIMON,  FRAMES(ICON_SIMON),  simonEnter,    simonFrame,    nullptr,    false},
  {"SETTINGS", CRGB(200, 200, 200), ICON_SET,    FRAMES(ICON_SET),    settingsEnter, settingsFrame, nullptr,    true},
};
const int APP_COUNT = sizeof(apps) / sizeof(apps[0]);

const uint16_t FRAME_MS     = 20;      // 50 frames per second
const uint32_t MENU_IDLE_MS = 25000;   // menu goes back to the pet after this

enum Screen : uint8_t { SCR_TUTORIAL, SCR_PET, SCR_MENU, SCR_APP };
const char* const SCREEN_NAMES[] = {"TUTORIAL", "PET", "MENU", "APP"};
Screen   screen = SCR_PET;
int      currentApp = -1;
Carousel menu;
uint32_t menuIdleSince = 0, screenSince = 0;
bool     mirrorOn = false;       // PC app asked for a live copy of the screen

// ============================================================================
//  Which apps are on the menu, in what order (picked in the PC panel).
//  Saved as NAMES, not numbers, so a firmware with new apps doesn't mix
//  them up; apps the saved list has never heard of are added at the end.
//  SETTINGS is always there, so you can't lock yourself out.
// ============================================================================
uint8_t menuOrder[32];
uint8_t menuLen = 0;

int appByName(const char* n) {
  for (int i = 0; i < APP_COUNT; i++) if (!strcmp(apps[i].name, n)) return i;
  return -1;
}
int menuPos(int app) {
  for (int i = 0; i < menuLen; i++) if (menuOrder[i] == app) return i;
  return -1;
}
bool nameInList(const char* list, const char* n) {   // "A,B,C" contains n?
  size_t l = strlen(n);
  for (const char* p = list; *p; ) {
    const char* e = strchr(p, ',');
    size_t k = e ? (size_t)(e - p) : strlen(p);
    if (k == l && !strncmp(p, n, l)) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}
void menuAdd(int app) { if (app >= 0 && menuPos(app) < 0 && menuLen < sizeof(menuOrder)) menuOrder[menuLen++] = app; }

void menuSave() {
  String vis, all;
  for (int i = 0; i < menuLen; i++) { if (i) vis += ','; vis += apps[menuOrder[i]].name; }
  for (int i = 0; i < APP_COUNT; i++) { if (i) all += ','; all += apps[i].name; }
  prefs.begin("menu", false);
  prefs.putString("vis", vis);
  prefs.putString("all", all);
  prefs.end();
}

void menuLoad() {
  String vis, all;
  prefs.begin("menu", true);
  bool has = prefs.isKey("vis");
  if (has) { vis = prefs.getString("vis"); all = prefs.getString("all"); }
  prefs.end();
  menuLen = 0;
  if (has) {
    int from = 0;
    while (from <= (int)vis.length()) {
      int comma = vis.indexOf(',', from);
      if (comma < 0) comma = vis.length();
      menuAdd(appByName(vis.substring(from, comma).c_str()));
      from = comma + 1;
    }
  }
  for (int i = 0; i < APP_COUNT; i++)
    if (!has || !nameInList(all.c_str(), apps[i].name)) menuAdd(i);   // new apps show up
  menuAdd(APP_COUNT - 1);                                              // SETTINGS, always
}

// ============================================================================
//  Moving between screens
// ============================================================================

void goPet() {
  uint32_t since = screenSince;
  screen = SCR_PET;
  screenSince = millis();
  currentApp = -1;
  setTiltBase();
  petGreet(since);
  fadeIn();
  Serial.println("[HOME] Pet");
}

void openMenu() {
  screen = SCR_MENU;
  screenSince = millis();
  menuIdleSince = millis();
  menu.movedAt = millis();
  menu.labelDone = false;   // show the app name again
  setTiltBase();
  fadeIn();
  Serial.println("[MENU] Open");
}

void openApp(int i) {
  if (i < 0 || i >= APP_COUNT) i = 0;
  screen = SCR_APP;
  screenSince = millis();
  currentApp = i;
  if (menuPos(i) >= 0) menu.index = menuPos(i);
  if (cfg.lastApp != i) { cfg.lastApp = i; saveConfig(); }   // remember your place
  Serial.printf("[APP] Open %s\n", apps[i].name);
  setTiltBase();
  if (apps[i].enter) apps[i].enter();
  fadeIn();
}

void closeApp() {
  if (apps[currentApp].leave) apps[currentApp].leave();
  Serial.println("[APP] Back to menu");
  uint32_t since = screenSince;
  currentApp = -1;
  openMenu();
  screenSince = since;      // so the pet can tell how the app went
  menu.labelDone = true;    // don't re-scroll the name we just came from
}

void onRotationChanged() {
  setTiltBase();
  fadeIn();
  Serial.printf("[ROT] Screen rotated to %d\n", cfg.rotation * 90);
}

// ============================================================================
//  Menu
// ============================================================================

void drawMenuItem(int i, int xo) {
  const App& a = apps[menuOrder[i]];
  drawSprite(a.icon[(millis() / 350) % a.frames], xo, 0);
}
const char* menuName(int i) { return apps[menuOrder[i]].name; }

void menuFrame(Event e) {
  uint32_t now = millis();
  if (e != EV_NONE) menuIdleSince = now;
  switch (e) {
    case EV_LEFT:  menu.move(-1); break;
    case EV_RIGHT: menu.move(+1); break;
    case EV_TAP:   openApp(menuOrder[menu.index]); return;
    case EV_HOLD:  goPet(); return;          // hold in the menu = back to the pet
    default: break;
  }
  if (now - menuIdleSince > MENU_IDLE_MS) { goPet(); return; }
  clearFb();
  menu.draw(drawMenuItem, menuName, apps[menuOrder[menu.index]].color);
  // Link dot, top-right: blue = Bluetooth connected, green = keys go over Wi-Fi (the panel).
  if (!fb[4]) {
    if (bleKeyboard.isConnected() && !(cfg.inputVia == 2 && netInputReady())) fb[4] = CRGB(0, 0, beatsin8(20, 15, 60));
    else if (netInputReady()) fb[4] = CRGB(0, beatsin8(20, 15, 60), 0);
  }
}

// ============================================================================
//  Pet (home)
// ============================================================================

void petFrame(Event e) {
  uint32_t now = millis();
  if (e == EV_HOLD)   { openMenu(); return; }                 // the only way to the menu
  if (e == EV_DOUBLE) { openApp(cfg.lastApp); return; }       // straight into the last app
  petEvent(e);
  petUpdate(now);
  clearFb();
  drawPet(now);
  if (nmOn && wifiState == WF_ONLINE && !nmInternetUp && (now / 500) % 2) fb[20] = CRGB(255, 0, 0);   // internet down
}

// ============================================================================
//  Tutorial (first start, or Settings > Tutorial, or from the PC app)
// ============================================================================

struct TutStep { const char* text; const char* sprite; Event wants; };
const TutStep TUT[] = {
  {"HI! I'M NOVA",  nullptr,       EV_NONE},
  {"TILT RIGHT",    SPR_ARROW_RY,  EV_RIGHT},
  {"TILT LEFT",     nullptr,       EV_LEFT},    // sprite drawn mirrored below
  {"TAP",           SPR_TAP_DOT,   EV_TAP},
  {"HOLD",          SPR_HOLD_RING, EV_HOLD},
  {"READY!",        nullptr,       EV_NONE},
};
const uint8_t TUT_STEPS = sizeof(TUT) / sizeof(TUT[0]);
uint8_t  tutStep = 0;
Scroller tutText;
uint32_t tutDoneAt = 0;

void tutBeginStep() {
  tutText.start(TUT[tutStep].text, tutStep == 0 || tutStep == TUT_STEPS - 1
                                       ? CRGB(150, 60, 255) : CRGB(255, 200, 0));
  tutDoneAt = 0;
}

void startTutorial() {
  if (screen == SCR_APP && currentApp >= 0 && apps[currentApp].leave) apps[currentApp].leave();
  currentApp = -1;
  screen = SCR_TUTORIAL;
  screenSince = millis();
  tutStep = 0;
  setTiltBase();
  tutBeginStep();
  fadeIn();
  Serial.println("[TUT] Tutorial started");
}

void tutorialFrame(Event e) {
  uint32_t now = millis();
  const TutStep& s = TUT[tutStep];
  clearFb();

  // Steps with no action just play their text, then move on.
  if (s.wants == EV_NONE) {
    if (!tutText.draw()) {
      if (++tutStep >= TUT_STEPS) {
        cfg.tutorialDone = true;
        saveConfig();
        Serial.println("[TUT] Done");
        goPet();
        return;
      }
      tutBeginStep();
    }
    return;
  }

  if (tutDoneAt) {                         // success: green tick, then next step
    drawSprite(SPR_CHECK);
    if (now - tutDoneAt > 700) { tutStep++; tutBeginStep(); setTiltBase(); }
    return;
  }
  if (e == s.wants) {
    tutDoneAt = now;
    fxRipple(CRGB::Green);
    Serial.printf("[TUT] Step %d OK\n", tutStep);
    return;
  }
  if (tutText.draw()) return;              // show the instruction first

  bool on = (now / 350) % 2;
  if (tutStep == 2) {                      // left arrow = right arrow mirrored
    if (on) for (int i = 0; i < 25; i++)
      if (SPR_ARROW_RY[i] != '.') px(4 - i % 5, i / 5, CRGB(255, 200, 0));
  } else if (tutStep == 4 && M5.Btn.isPressed()) {   // ring turns green while held
    drawSpriteTint(SPR_HOLD_RING, 0, 0, CRGB(0, beatsin8(200, 80, 255), 0));
  } else if (on || tutStep == 4) {
    drawSprite(s.sprite);
  }
}

// ============================================================================
//  Status line for the Serial Monitor ('?')
// ============================================================================

void statusPrint() {
  Serial.printf("[STATUS] screen=%s app=%s tilt=(%.2f,%.2f) base=(%.2f,%.2f) dir=%d z=%.2f "
                "ble=%s slot=%d rot=%d bright=%d tv=%s pet=%s love=%u\n",
                SCREEN_NAMES[screen], currentApp >= 0 ? apps[currentApp].name : "-",
                tiltX, tiltY, tiltBaseX, tiltBaseY, tiltDir, rawSz,
                bleKeyboard.isConnected() ? "connected" : "waiting", cfg.btSlot + 1, cfg.rotation,
                cfg.brightIdx + 1, tvBrandName(cfg.tvBrand), PET_MOOD_NAMES[petMood], cfg.petLove);
}

// ============================================================================
//  PC alerts: the panel can pop a message up over whatever is on screen
//  ({"cmd":"alert"} in Remote.h). An icon pulses, the text scrolls past,
//  the icon pulses again. A tap (or hold) clears it early, and that press
//  goes to the alert, not to the app underneath.
// ============================================================================

struct Alert {
  bool        on = false;
  const char* icon = nullptr;
  CRGB        color;
  uint8_t     phase = 0;         // 0 = icon, 1 = text, 2 = icon again
  uint32_t    at = 0;
  Scroller    text;
  char        msg[64];
} alert;

void alertShow(const char* kind, const char* msg) {
  if (!strcmp(kind, "download"))     { alert.icon = SPR_AL_DOWN; alert.color = CRGB(0, 255, 80); }
  else if (!strcmp(kind, "cpu"))     { alert.icon = SPR_AL_HOT;  alert.color = CRGB(255, 90, 0); }
  else if (!strcmp(kind, "battery")) { alert.icon = SPR_AL_BATT; alert.color = CRGB(255, 0, 0); }
  else if (!strcmp(kind, "netdown")) { alert.icon = SPR_AL_NET;  alert.color = CRGB(255, 0, 0); }
  else if (!strcmp(kind, "netup"))   { alert.icon = SPR_AL_NET;  alert.color = CRGB(0, 255, 80); }
  else                               { alert.icon = SPR_AL_BELL; alert.color = CRGB(160, 60, 255); }
  size_t k = 0;                  // keep what the 3x5 font can show
  for (const char* p = msg; *p && k < sizeof(alert.msg) - 1; p++)
    if (isalnum((unsigned char)*p) || strchr(" +-.:/?!", *p)) alert.msg[k++] = toupper((unsigned char)*p);
  alert.msg[k] = 0;
  alert.on = true;
  alert.phase = 0;
  alert.at = millis();
  Serial.printf("[ALERT] %s: %s\n", kind, alert.msg);
}

// A press while an alert shows clears it, and the app never sees that press.
void alertEat(Event& e) {
  if (!alert.on || !(e == EV_TAP || e == EV_DOUBLE || e == EV_TRIPLE || e == EV_HOLD)) return;
  alert.on = false;
  e = EV_NONE;
  fadeIn();
}

// Drawn over the app, which keeps running underneath.
void alertDraw() {
  if (!alert.on) return;
  uint32_t now = millis();
  clearFb();
  if (alert.phase == 1) {
    if (alert.text.draw()) return;
    alert.phase = 2;
    alert.at = now;
  }
  uint32_t el = now - alert.at;
  if (alert.phase == 0 && el > 1600) {
    alert.phase = 1;
    alert.text.start(alert.msg[0] ? alert.msg : "ALERT", alert.color);
    alert.text.draw();
    return;
  }
  if (alert.phase == 2 && el > 1200) { alert.on = false; fadeIn(); return; }
  uint8_t v = beatsin8(90, 60, 255);
  drawSpriteTint(alert.icon, 0, 0, CRGB(alert.color).nscale8_video(v));
}

#include "Remote.h"
#include "NovaRemote.h"
#include "NetLink.h"

// ============================================================================
//  MEMORY: the ESP32's Bluetooth chip can do "classic" Bluetooth (speakers,
//  old headsets) and Bluetooth LE. By default it sets aside RAM for both,
//  but Pocket Nova only speaks LE (keyboard, mouse, phone remote). Running
//  Wi-Fi AND Bluetooth on 2.14 left only ~17 KB free, and a PC connecting
//  then needed more than that: the Bluetooth stack crashed. Starting the
//  chip in LE-only mode and releasing the classic part returns that RAM
//  to the heap. (Must happen before anything starts Bluetooth.)
// ============================================================================
#include <esp_bt.h>

void bleOnlyStart() {
  uint32_t before = ESP.getFreeHeap();
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_bt_controller_config_t c = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  c.mode = ESP_BT_MODE_BLE;
  if (esp_bt_controller_init(&c) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK)
    Serial.println("[BT] LE-only start failed, the Bluetooth library will try its own way");
  Serial.printf("[MEM] Free memory %u -> %u bytes after dropping classic Bluetooth\n", before, ESP.getFreeHeap());
}

// ============================================================================
//  Boot animation: a rainbow spiral that winds in
// ============================================================================

void bootAnimation() {
  const uint8_t SPIRAL[25] = {0, 1, 2, 3, 4, 9, 14, 19, 24, 23, 22, 21, 20,
                              15, 10, 5, 6, 7, 8, 13, 18, 17, 16, 11, 12};
  clearFb();
  for (int i = 0; i < 25; i++) {
    fb[SPIRAL[i]] = CHSV(180 + i * 4, 255, 255);   // Nova purples into pinks
    present();
    delay(22);
  }
  for (int s = 0; s < 12; s++) {
    fadeToBlackBy(fb, 25, 60);
    present();
    delay(20);
  }
}

// ============================================================================
//  Arduino entry points
// ============================================================================

void setup() {
  M5.begin(true, false, false);   // Serial yes, I2C no (IMU has its own), LEDs: we drive them
  loadConfig();
  keysLoad();
  displayBegin();

  if (M5.IMU.Init() != 0) Serial.println("[ERROR] IMU not found");
  bleApplySlot();                       // pick this slot's Bluetooth address first
  bleOnlyStart();                       // before the keyboard: hands classic Bluetooth's memory back
  bleKeyboard.setName(cfg.name);
  bleKeyboard.setScanService(REMOTE_SVC);
  bleKeyboard.begin();
  bleKeyboard.setReportSink(netHidSink);  // keys/mouse may go over Wi-Fi instead (NetLink.h)
  tzOffsetSec = cfg.tz;                 // until the PC or the internet says otherwise
  histBoot = esp_random();
  esp_reset_reason_t why = esp_reset_reason();
  if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT || why == ESP_RST_BROWNOUT) {
    histAdd(H_RESTART, nullptr, 0, resetReasonName());
    Serial.printf("[SYS] Started again after: %s\n", resetReasonName());
  }
  wifiBegin();
  otaLoadKey();
  nmLoad();
  finderLoad();
  hmLoad();
  irsend.begin();

  // A restart Pocket Nova asked for itself (slot switch, new name...) skips
  // the boot animation, so it's back about a second sooner.
  if (esp_reset_reason() != ESP_RST_SW) bootAnimation();
  for (int i = 0; i < 15; i++) { readTilt(); delay(10); }   // let the filter settle
  setTiltBase();
  evHead = evTail;   // drop any events from settling
  menuLoad();
  menu.reset(menuLen, max(0, menuPos(constrain(cfg.lastApp, 0, APP_COUNT - 1))));

  Serial.printf("\n=== %s ready (firmware %s, Bluetooth slot %d, %s) ===\n",
                cfg.name, FW_VERSION, cfg.btSlot + 1, bleOwnAddress().c_str());
  Serial.println("Home = pet. Hold = menu/back, tilt = browse, tap = select, double tap = last app.");
  Serial.println("Serial test keys: a tap, d double, b hold, h/l left/right, s shake, L/R/F/K fake tilt, 0, ?");

  if (!cfg.tutorialDone) startTutorial();
  else goPet();

  // WATCHDOG: a timer the main loop has to reset every time round. If the
  // loop ever gets stuck for 15 s (a bug, a radio driver that never answers),
  // the timer runs out and the chip restarts itself instead of sitting
  // frozen. The next start logs "froze (watchdog)" in the history.
  esp_task_wdt_init(15, true);
  enableLoopWDT();                      // Arduino resets it after each loop()
}

void loop() {
  static uint32_t lastFrame = 0, lastMirror = 0;
  static bool lastBle = false;
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) { delay(1); return; }
  lastFrame = now;

  // Double/triple taps only where something uses them (they delay single taps a little).
  multiTapEnabled = screen == SCR_PET || (screen == SCR_APP && apps[currentApp].multiTap);

  M5.update();
  inputUpdate();

  bool ble = bleKeyboard.isConnected();
  if (ble != lastBle) {
    lastBle = ble;
    Serial.println(ble ? "[BLE] Connected" : "[BLE] Disconnected");
  }
  swiftPairUpdate();
  bleSlotMapUpdate();
  wifiUpdate();
  otaUpdate();
  linkUpdate();
  sensorUpdate();
  mdnsUpdate();
  netMonitorUpdate();
  chanScanUpdate();
  finderUpdate();
  homeUpdate();
  remoteUpdate();

  Event e = popEvent();
  if (e != EV_NONE) Serial.printf("[EV] %s\n", eventName(e));
  alertEat(e);

  switch (screen) {
    case SCR_TUTORIAL: tutorialFrame(e); break;
    case SCR_PET:      petFrame(e); break;
    case SCR_MENU:     menuFrame(e); break;
    case SCR_APP:
      if (!apps[currentApp].frame(e)) closeApp();
      break;
  }

  alertDraw();
  drawFx();
  remoteDrawOverlay();
  present();

  if (mirrorOn && now - lastMirror >= 100) {   // live copy of the screen for the PC app
    lastMirror = now;
    sendMirrorFrame();
  }
}
