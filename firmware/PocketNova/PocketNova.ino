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

const char* const FW_VERSION = "2.4.1";

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
#include "Wifi.h"
#include "Apps.h"
#include "Pet.h"

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
  {"KEYS",     CRGB(255, 120, 0),   ICON_KEYS,   FRAMES(ICON_KEYS),   keysEnter,     keysFrame,     nullptr,    false},
  {"LEVEL",    CRGB(0, 255, 0),     ICON_LEVEL,  FRAMES(ICON_LEVEL),  nullptr,       levelFrame,    nullptr,    false},
  {"DICE",     CRGB(255, 255, 255), ICON_DICE,   FRAMES(ICON_DICE),   nullptr,       diceFrame,     nullptr,    false},
  {"TIMER",    CRGB(255, 200, 0),   ICON_TIMER,  FRAMES(ICON_TIMER),  timerEnter,    timerFrame,    nullptr,    false},
  {"LIGHTS",   CRGB(255, 0, 180),   ICON_LIGHT,  FRAMES(ICON_LIGHT),  lightEnter,    lightFrame,    nullptr,    false},
  {"TORCH",    CRGB(255, 255, 255), ICON_TORCH,  FRAMES(ICON_TORCH),  torchEnter,    torchFrame,    torchLeave, false},
  {"SNAKE",    CRGB(0, 255, 0),     ICON_SNAKE,  FRAMES(ICON_SNAKE),  snakeEnter,    snakeFrame,    nullptr,    false},
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
  menu.index = i;
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
  const App& a = apps[i];
  drawSprite(a.icon[(millis() / 350) % a.frames], xo, 0);
}
const char* menuName(int i) { return apps[i].name; }

void menuFrame(Event e) {
  uint32_t now = millis();
  if (e != EV_NONE) menuIdleSince = now;
  switch (e) {
    case EV_LEFT:  menu.move(-1); break;
    case EV_RIGHT: menu.move(+1); break;
    case EV_TAP:   openApp(menu.index); return;
    case EV_HOLD:  goPet(); return;          // hold in the menu = back to the pet
    default: break;
  }
  if (now - menuIdleSince > MENU_IDLE_MS) { goPet(); return; }
  clearFb();
  menu.draw(drawMenuItem, menuName, apps[menu.index].color);
  // Bluetooth dot: top-right corner glows blue while connected.
  if (bleOK() && !fb[4]) fb[4] = CRGB(0, 0, beatsin8(20, 15, 60));
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
                bleOK() ? "connected" : "waiting", cfg.btSlot + 1, cfg.rotation,
                cfg.brightIdx + 1, tvBrandName(cfg.tvBrand), PET_MOOD_NAMES[petMood], cfg.petLove);
}

#include "Remote.h"
#include "NovaRemote.h"

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
  displayBegin();

  if (M5.IMU.Init() != 0) Serial.println("[ERROR] IMU not found");
  bleApplySlot();                       // pick this slot's Bluetooth address first
  bleKeyboard.setName(cfg.name);
  bleKeyboard.setScanService(REMOTE_SVC);
  bleKeyboard.begin();
  tzOffsetSec = cfg.tz;                 // until the PC or the internet says otherwise
  wifiBegin();
  irsend.begin();

  // A restart Pocket Nova asked for itself (slot switch, new name...) skips
  // the boot animation, so it's back about a second sooner.
  if (esp_reset_reason() != ESP_RST_SW) bootAnimation();
  for (int i = 0; i < 15; i++) { readTilt(); delay(10); }   // let the filter settle
  setTiltBase();
  evHead = evTail;   // drop any events from settling
  menu.reset(APP_COUNT, constrain(cfg.lastApp, 0, APP_COUNT - 1));

  Serial.printf("\n=== %s ready (firmware %s, Bluetooth slot %d, %s) ===\n",
                cfg.name, FW_VERSION, cfg.btSlot + 1, bleOwnAddress().c_str());
  Serial.println("Home = pet. Hold = menu/back, tilt = browse, tap = select, double tap = last app.");
  Serial.println("Serial test keys: a tap, d double, b hold, h/l left/right, s shake, L/R/F/K fake tilt, 0, ?");

  if (!cfg.tutorialDone) startTutorial();
  else goPet();
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

  bool ble = bleOK();
  if (ble != lastBle) {
    lastBle = ble;
    Serial.println(ble ? "[BLE] Connected" : "[BLE] Disconnected");
  }
  swiftPairUpdate();
  bleSlotMapUpdate();
  wifiUpdate();
  remoteUpdate();

  Event e = popEvent();
  if (e != EV_NONE) Serial.printf("[EV] %s\n", eventName(e));

  switch (screen) {
    case SCR_TUTORIAL: tutorialFrame(e); break;
    case SCR_PET:      petFrame(e); break;
    case SCR_MENU:     menuFrame(e); break;
    case SCR_APP:
      if (!apps[currentApp].frame(e)) closeApp();
      break;
  }

  drawFx();
  remoteDrawOverlay();
  present();

  if (mirrorOn && now - lastMirror >= 100) {   // live copy of the screen for the PC app
    lastMirror = now;
    sendMirrorFrame();
  }
}
