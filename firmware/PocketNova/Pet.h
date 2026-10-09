#pragma once
// =====================================================================
//  Pet.h - Nova, the little creature on the home screen
// =====================================================================
//  Interactions:
//    tilt          eyes follow you          tap        happy ^ ^
//    triple tap    a big pulsing heart      shake      dizzy spinning eyes
//    rock gently   gets sleepy, falls asleep
//    tap asleep    wakes up with a yawn     ignore it  gets bored, then dozes
//    face down     hides (screen off)       turn back  "peekaboo!"
//    come back from an app: happy if things worked, sad if something failed
//    at night (time comes from the PC app) it's sleepy and dozes sooner
//  (HOLD opens the menu and DOUBLE TAP opens your last app - see main sketch.)
// =====================================================================

enum PetMood : uint8_t {
  PM_IDLE, PM_HAPPY, PM_LOVE, PM_DIZZY, PM_SAD, PM_SLEEPY, PM_ASLEEP,
  PM_WAKING, PM_HIDING, PM_PEEKABOO, PM_BORED
};
const char* const PET_MOOD_NAMES[] = {"idle", "happy", "love", "dizzy", "sad", "sleepy",
                                      "asleep", "waking", "hiding", "peekaboo", "bored"};

const uint32_t PET_BORED_MS      = 45000;    // idle this long -> looks around, yawns
const uint32_t PET_DOZE_MS       = 120000;   // -> falls asleep
const uint32_t PET_DOZE_NIGHT_MS = 30000;    // ...much sooner at night

PetMood  petMood = PM_IDLE;
uint32_t petMoodUntil = 0;      // temporary moods end here (0 = until changed)
uint32_t petLastTouch = 0;      // last time you interacted with the pet
uint32_t petMoodStart = 0;
bool     petLoveDirty = false;
uint32_t petLoveSavedAt = 0;
int32_t  tzOffsetSec = 0;       // set by the PC app along with the clock

// ---- clock (only known once the PC app has sent the time) ----
bool clockValid() { return time(nullptr) > 1700000000; }

int localHour() {
  time_t t = time(nullptr) + tzOffsetSec;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  return tmv.tm_hour;
}

bool isNight() {
  if (!clockValid()) return false;
  int h = localHour();
  return h >= 22 || h < 7;
}

// ---- mood helpers ----
void petSet(PetMood m, uint32_t ms = 0) {
  petMood = m;
  petMoodStart = millis();
  petMoodUntil = ms ? millis() + ms : 0;
}

void petAddLove(uint8_t n) {
  cfg.petLove = min(9999, cfg.petLove + n);
  petLoveDirty = true;
}

// Called when you come back to the pet (from the menu or an app).
// lastGoodAt / lastBadAt are stamped by fxRipple() / fxError() in Display.h.
void petGreet(uint32_t sinceMs) {
  uint32_t now = millis();
  petLastTouch = now;
  if (lastBadAt > sinceMs && lastBadAt > lastGoodAt) petSet(PM_SAD, 1600);
  else if (lastGoodAt > sinceMs) petSet(PM_HAPPY, 900);
  else petSet(PM_IDLE);
}

// React to an event. HOLD and DOUBLE are handled by the main sketch.
void petEvent(Event e) {
  uint32_t now = millis();
  bool asleep = petMood == PM_ASLEEP || petMood == PM_SLEEPY;
  switch (e) {
    case EV_TAP:
      if (petMood == PM_HIDING) break;
      if (asleep) petSet(PM_WAKING, 1300);
      else { petSet(PM_HAPPY, 800); petAddLove(1); }
      petLastTouch = now;
      break;
    case EV_TRIPLE:
      petSet(PM_LOVE, 2200);
      petAddLove(3);
      petLastTouch = now;
      Serial.printf("[PET] Love! (%u)\n", cfg.petLove);
      break;
    case EV_SHAKE:
      if (petMood != PM_HIDING) { petSet(PM_DIZZY, 1500); petLastTouch = now; }
      break;
    case EV_ROCK:
      if (!asleep && petMood != PM_HIDING) {
        petSet(PM_SLEEPY, 2500);
        petLastTouch = now;
        Serial.println("[PET] Rocked to sleep");
      }
      break;
    case EV_FACEDOWN: petSet(PM_HIDING); break;
    case EV_FACEUP:
      if (petMood == PM_HIDING) { petSet(PM_PEEKABOO, 1300); petLastTouch = now; }
      break;
    default: break;
  }
}

// Advance timed moods and the idle -> bored -> asleep drift.
void petUpdate(uint32_t now) {
  if (petMoodUntil && now >= petMoodUntil) {
    if (petMood == PM_SLEEPY) petSet(PM_ASLEEP);   // sleepy always ends asleep
    else petSet(PM_IDLE);
  }
  uint32_t idle = now - petLastTouch;
  uint32_t dozeAt = isNight() ? PET_DOZE_NIGHT_MS : PET_DOZE_MS;
  if (petMood == PM_IDLE && idle > PET_BORED_MS) petSet(PM_BORED);
  if ((petMood == PM_IDLE || petMood == PM_BORED) && idle > dozeAt) {
    petSet(PM_SLEEPY, 2500);
    Serial.println("[PET] Dozing off");
  }
  // Save affection now and then (not on every tap: flash wears out).
  if (petLoveDirty && now - petLoveSavedAt > 30000) {
    saveConfig();
    petLoveDirty = false;
    petLoveSavedAt = now;
  }
}

// ---- drawing ----

const CRGB EYE_WHITE(45, 45, 60), EYE_PUPIL(0, 200, 255);

// Two 2x3 eyes. lid: 0 = open, 1 = half closed, 2 = shut.
void drawEyes(int lookX, int lookY, uint8_t lid, CRGB white = EYE_WHITE, CRGB pupil = EYE_PUPIL) {
  for (int ex : {0, 3}) {
    if (lid >= 2) { px(ex, 3, white); px(ex + 1, 3, white); continue; }
    int top = lid == 1 ? 2 : 1;
    for (int y = top; y <= 3; y++) { px(ex, y, white); px(ex + 1, y, white); }
    int py = constrain(2 + lookY, top, 3);
    px(ex + constrain(lookX, 0, 1), py, pupil);
  }
}

void drawHeart(uint8_t v) {
  static const char* HEART = ".R.R." "RRRRR" "RRRRR" ".RRR." "..R..";
  drawSpriteTint(HEART, 0, 0, CRGB(v, 0, v / 3));
}

void drawPet(uint32_t now) {
  uint32_t t = now - petMoodStart;
  // Where to look: toward the tilt, or idle glances.
  float gx = tiltX - tiltBaseX, gy = tiltY - tiltBaseY;
  int lookX = gx > 0.2f ? 1 : (gx < -0.2f ? 0 : (now / 2600) % 2);
  int lookY = gy > 0.25f ? -1 : (gy < -0.25f ? 1 : 0);
  bool blink = (now % 4200) < 140 || (now % 9100) < 120;

  switch (petMood) {
    case PM_HAPPY:   // ^ ^ with a smile
      px(0, 2, EYE_WHITE); px(1, 1, EYE_WHITE); px(3, 1, EYE_WHITE); px(4, 2, EYE_WHITE);
      for (int x = 1; x <= 3; x++) px(x, 4, CRGB(255, 0, 120));
      return;

    case PM_LOVE:    // heartbeat
      drawHeart(beatsin8(110, 90, 255));
      return;

    case PM_DIZZY: {
      static const int8_t SPIN[6][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{0,0}};
      int s = (now / 90) % 6;
      drawEyes(SPIN[s][0], SPIN[s][1], 0);
      return;
    }

    case PM_SAD:     // droopy eyes looking down, a tear rolls
      drawEyes(lookX, 1, 1);
      px(1, constrain(3 + (int)(t / 400), 3, 4), CRGB(0, 60, 255));
      return;

    case PM_SLEEPY:  // lids slowly closing
      drawEyes(lookX, 1, t < 1200 ? 1 : 2, CRGB(30, 30, 45), CRGB(0, 90, 120));
      return;

    case PM_ASLEEP: {  // shut eyes + a "z" floating up from the corner
      CRGB lid(0, 0, beatsin8(8, 6, 40));
      px(0, 3, lid); px(1, 3, lid); px(3, 3, lid); px(4, 3, lid);
      int zy = 2 - (int)((now / 700) % 3);
      px(4, zy, CRGB(0, 0, 35));
      return;
    }

    case PM_WAKING:  // eyes open while it yawns
      drawEyes(0, 0, t < 400 ? 2 : (t < 800 ? 1 : 0));
      px(2, 4, CRGB(120, 40, 60));
      if (t > 300 && t < 1000) { px(1, 4, CRGB(60, 20, 30)); px(3, 4, CRGB(60, 20, 30)); }
      return;

    case PM_HIDING:  // face down: nothing to see
      return;

    case PM_PEEKABOO:  // wide awake, big bright eyes
      for (int ex : {0, 3})
        for (int y = 0; y <= 3; y++) { px(ex, y, CRGB(80, 80, 100)); px(ex + 1, y, CRGB(80, 80, 100)); }
      px(0 + lookX, 1, EYE_PUPIL); px(3 + lookX, 1, EYE_PUPIL);
      if (t < 200) for (int x = 0; x < 5; x++) px(x, 4, CRGB(40, 40, 40));
      return;

    case PM_BORED: {   // slow look-arounds and the odd yawn
      int phase = (now / 1800) % 4;
      int bx = phase == 1 ? 0 : (phase == 3 ? 1 : lookX);
      bool yawn = (now % 11000) < 1100;
      drawEyes(bx, 0, yawn ? 2 : (isNight() ? 1 : 0));
      if (yawn) px(2, 4, CRGB(120, 40, 60));
      return;
    }

    default: break;   // PM_IDLE
  }

  if (blink) {
    for (int ex : {0, 3}) { px(ex, 2, EYE_WHITE); px(ex + 1, 2, EYE_WHITE); }
  } else {
    drawEyes(lookX, lookY, isNight() ? 1 : 0);
  }
  // A happy pet sometimes floats a tiny heart.
  if (cfg.petLove >= 50 && (now % 20000) < 900) px(2, 0, CRGB(beatsin8(120, 40, 200), 0, 40));
}
