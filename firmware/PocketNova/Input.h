#pragma once
// =====================================================================
//  Input.h - turns the button + accelerometer into simple "events"
// =====================================================================
//  Apps don't read hardware. Each frame they get ONE event:
//    EV_TAP / EV_DOUBLE / EV_TRIPLE   1, 2 or 3 quick presses
//    EV_HOLD     long press (= back)
//    EV_LEFT / EV_RIGHT               one step per tilt
//    EV_SHAKE    shaken                EV_ROCK  rocked gently side to side
//    EV_FACEDOWN / EV_FACEUP          turned over / turned back
//  ...and can also look at the live tilt: tiltX, tiltY, tiltDir.
//
//  Tilt directions: +X = right edge down, +Y = top edge down ("forward").
//
//  Serial test commands (Serial Monitor, 115200 baud):
//    a = tap   d = double tap   b = hold   h = left   l = right   s = shake
//    L/R/F/K = fake a tilt left/right/forward/back,  0 = stop faking
//    ? = print status.   Lines starting with { are JSON commands (Remote.h)
// =====================================================================

enum Event : uint8_t {
  EV_NONE, EV_TAP, EV_DOUBLE, EV_TRIPLE, EV_HOLD, EV_LEFT, EV_RIGHT,
  EV_SHAKE, EV_ROCK, EV_FACEDOWN, EV_FACEUP
};
enum TiltDir : uint8_t { T_LEVEL, T_LEFT, T_RIGHT, T_FWD, T_BACK };

const char* eventName(Event e) {
  static const char* const N[] = {"NONE", "TAP", "DOUBLE", "TRIPLE", "HOLD", "LEFT",
                                  "RIGHT", "SHAKE", "ROCK", "FACEDOWN", "FACEUP"};
  return N[e];
}

const uint16_t HOLD_MS        = 650;   // press longer than this = HOLD
const uint16_t MULTI_TAP_MS   = 260;   // max gap between taps of a double/triple
// Tilt is measured in g: 0.5 g = 30 degrees, 0.6 g = 37 degrees, 0.71 g = 45.
const float    NAV_ON         = 0.60f; // ~37 deg: tilt this far to step LEFT/RIGHT
const float    NAV_OFF        = 0.25f; // ~15 deg: come back under this to re-arm
const float    NAV_HINT       = 0.20f; // edge glow starts here (~12 deg)
const uint16_t NAV_HOLD_MS    = 150;   // must stay tilted this long (ignores bumps)
const uint16_t NAV_MIN_GAP_MS = 650;   // at least this long between two steps
const uint16_t NAV_AFTER_PRESS_MS = 400; // ignore tilt right after a button press
const float    TILT_DIR_G     = 0.45f; // ~27 deg: tilt needed for gesture apps
const float    TILT_REACH     = 0.85f; // a step never needs more than this reading (~58 deg)
const float    TILT_MIN_STEP  = 0.30f; // ...nor less than this much tilt (~17 deg)
const float    SHAKE_G        = 1.1f;  // jolt above 1g of gravity
const float    FACE_UP_Z      = -1.0f; // sign of Z when the screen faces up (measured: -0.93)
const float    VERTICAL_Z     = 0.45f; // |Z| below this = held upright like a phone

float   rawSx = 0, rawSy = 0, rawSz = FACE_UP_Z;   // smoothed raw accel (Calibrate uses X/Y)
float   tiltX = 0, tiltY = 0;               // calibrated + rotated tilt, in g
float   tiltBaseX = 0, tiltBaseY = 0;       // the "rest" pose gestures are measured from

TiltDir  tiltDir = T_LEVEL;
char     tiltOverride = 0;
uint32_t lastActivityMs = 0;
bool     multiTapEnabled = false;   // set each frame by whoever wants double/triple taps
bool     faceDown = false;

void handleRemoteLine(const char* line);   // Remote.h
void statusPrint();                        // main sketch
void onRotationChanged();                  // main sketch

// Call when an app opens: "however you're holding it now = level".
void setTiltBase() {
  // Keep the old rest pose while a fake tilt is active, and cap it so a
  // wild angle can't lock you out.
  if (tiltOverride) return;
  tiltBaseX = constrain(tiltX, -0.5f, 0.5f);
  tiltBaseY = constrain(tiltY, -0.5f, 0.5f);
}

// ---- tiny event queue (ring buffer) ----
Event   evq[8];
uint8_t evHead = 0, evTail = 0;

void pushEvent(Event e) {
  uint8_t n = (evTail + 1) % 8;
  if (n != evHead) { evq[evTail] = e; evTail = n; }
  lastActivityMs = millis();
}

Event popEvent() {
  if (evHead == evTail) return EV_NONE;
  Event e = evq[evHead];
  evHead = (evHead + 1) % 8;
  return e;
}

// ---- button: taps (single/double/triple) vs hold ----
bool     holdFired = false;
uint32_t lastPressMs = 0;     // when the button was last down (tilt ignores jolts)
uint8_t  tapCount = 0;
uint32_t lastTapMs = 0;

void flushTaps() {
  if (!tapCount) return;
  pushEvent(tapCount == 1 ? EV_TAP : tapCount == 2 ? EV_DOUBLE : EV_TRIPLE);
  tapCount = 0;
}

void registerTap() {
  if (!multiTapEnabled) { pushEvent(EV_TAP); return; }
  tapCount++;
  lastTapMs = millis();
  if (tapCount >= 3) flushTaps();   // nothing above triple: fire right away
}

void readButton() {
  uint32_t now = millis();
  if (M5.Btn.isPressed()) lastPressMs = now;
  if (M5.Btn.pressedFor(HOLD_MS) && !holdFired) {
    holdFired = true;
    tapCount = 0;                   // a hold cancels half-finished taps
    pushEvent(EV_HOLD);
  }
  if (M5.Btn.wasReleased()) {
    if (holdFired) holdFired = false;   // end of a hold: not a tap
    else registerTap();
  }
  // Waited long enough for another tap? Then decide what it was.
  if (tapCount && !M5.Btn.isPressed() && now - lastTapMs > MULTI_TAP_MS) flushTaps();
}

// ---- serial: single-letter test keys, or JSON lines from the PC app ----
char    lineBuf[320];
uint8_t lineLen = 0;

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (lineLen > 0 || c == '{') {          // inside a JSON line
      if (c == '\n' || c == '\r') {
        lineBuf[lineLen] = 0;
        handleRemoteLine(lineBuf);
        lineLen = 0;
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      } else {
        lineLen = 0;                        // too long: drop it
      }
      continue;
    }
    switch (c) {
      case 'a': registerTap(); break;
      case 'd': pushEvent(EV_DOUBLE); break;
      case 'b': pushEvent(EV_HOLD); break;
      case 'h': pushEvent(EV_LEFT); break;
      case 'l': pushEvent(EV_RIGHT); break;
      case 's': pushEvent(EV_SHAKE); break;
      case 'L': case 'R': case 'F': case 'K': tiltOverride = c; break;
      case '0': tiltOverride = 0; break;
      case '?': statusPrint(); break;
    }
  }
}

// ---- accelerometer ----
int8_t   navDir = 0;
uint32_t navPendingSince = 0, lastShakeMs = 0, lastNavMs = 0;
uint32_t verticalSince = 0, faceChangeSince = 0;
int8_t   rockSide = 0;
uint8_t  rockSwings = 0;
uint32_t rockFirstMs = 0;

// Turn the screen upright when the Atom is held vertically like a phone.
void autoRotateCheck(float pX, float pUp, uint32_t now) {
  if (!cfg.autoRotate || tiltOverride || fabsf(rawSz) > VERTICAL_Z) { verticalSince = 0; return; }
  if (!verticalSince) { verticalSince = now; return; }
  if (now - verticalSince < 700) return;
  // "top edge down" for each rotation; the upright one is the most negative.
  float ty[4] = {pUp, pX, -pUp, -pX};
  uint8_t best = 0;
  for (uint8_t r = 1; r < 4; r++) if (ty[r] < ty[best]) best = r;
  if (best != cfg.rotation && ty[best] < -0.7f) {
    cfg.rotation = best;
    saveConfig();
    onRotationChanged();
  }
}

// Rocking = the tilt swings left/right/left/right gently, like a cradle.
void rockCheck(float gx, uint32_t now) {
  int8_t side = gx > 0.15f ? 1 : (gx < -0.15f ? -1 : 0);
  if (fabsf(gx) > 0.5f) { rockSwings = 0; return; }   // too big = navigating
  if (side && side != rockSide) {
    if (rockSwings == 0 || now - rockFirstMs > 4000) { rockSwings = 0; rockFirstMs = now; }
    rockSide = side;
    if (++rockSwings >= 5) { rockSwings = 0; pushEvent(EV_ROCK); }
  }
}

void readTilt() {
  float ax, ay, az;
  M5.IMU.getAccelData(&ax, &ay, &az);
  uint32_t now = millis();

  // Shake = total acceleration far from 1g (gravity alone).
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  if (fabsf(mag - 1.0f) > SHAKE_G && now - lastShakeMs > 800) {
    lastShakeMs = now;
    pushEvent(EV_SHAKE);
  }

  // Low-pass filter: smooths out hand jitter.
  rawSx = rawSx * 0.65f + ax * 0.35f;
  rawSy = rawSy * 0.65f + ay * 0.35f;
  rawSz = rawSz * 0.65f + az * 0.35f;
  float raw[2] = {rawSx, rawSy};

  // Face down / face up (held for half a second so flips mid-shake don't count).
  bool down = rawSz * FACE_UP_Z < -0.75f;
  if (down != faceDown) {
    if (!faceChangeSince) faceChangeSince = now;
    else if (now - faceChangeSince > 500) {
      faceDown = down;
      faceChangeSince = 0;
      pushEvent(down ? EV_FACEDOWN : EV_FACEUP);
    }
  } else {
    faceChangeSince = 0;
  }

  // Physical tilt (how this board's sensor is mounted, from Calibrate)...
  float pX  = cfg.pX.sign  * raw[cfg.pX.axis];
  float pUp = cfg.pUp.sign * raw[cfg.pUp.axis];
  autoRotateCheck(pX, pUp, now);

  // ...then rotated to match the screen rotation.
  switch (cfg.rotation & 3) {
    case 0:  tiltX =  pX;  tiltY =  pUp; break;
    case 1:  tiltX = -pUp; tiltY =  pX;  break;
    case 2:  tiltX = -pX;  tiltY = -pUp; break;
    default: tiltX =  pUp; tiltY = -pX;  break;
  }

  // Fake tilt from the Serial Monitor / PC app, measured from the rest pose
  // (so it works even when the Atom is propped up at an angle).
  switch (tiltOverride) {
    case 'L': tiltX = tiltBaseX - 0.75f; tiltY = tiltBaseY; break;
    case 'R': tiltX = tiltBaseX + 0.75f; tiltY = tiltBaseY; break;
    case 'F': tiltX = tiltBaseX; tiltY = tiltBaseY + 0.75f; break;
    case 'K': tiltX = tiltBaseX; tiltY = tiltBaseY - 0.75f; break;
  }

  // Gestures are measured from the "rest" pose (see setTiltBase), so
  // holding it at a comfy angle - or a cable propping it up - isn't a tilt.
  float gx = tiltX - tiltBaseX, gy = tiltY - tiltBaseY;
  bool upright = cfg.autoRotate && fabsf(rawSz) < VERTICAL_Z && !tiltOverride;

  // REACHABLE STEPS: the sensor only feels gravity, so it can never read
  // much past 1 g on an axis. If the rest pose already leans right (a
  // charger cable propping it up, say), "rest + 0.6 g" to the right can be
  // impossible while left stays easy. So each side's step is capped at what
  // can still be reached from the rest pose (TILT_REACH), but never less
  // than a clear tilt (TILT_MIN_STEP).
  float onR  = constrain(fminf(NAV_ON, TILT_REACH - tiltBaseX), TILT_MIN_STEP, NAV_ON);
  float onL  = constrain(fminf(NAV_ON, TILT_REACH + tiltBaseX), TILT_MIN_STEP, NAV_ON);
  float dirR = onR * (TILT_DIR_G / NAV_ON), dirL = onL * (TILT_DIR_G / NAV_ON);
  float dirF = constrain(fminf(TILT_DIR_G, TILT_REACH - tiltBaseY), TILT_MIN_STEP * 0.8f, TILT_DIR_G);
  float dirK = constrain(fminf(TILT_DIR_G, TILT_REACH + tiltBaseY), TILT_MIN_STEP * 0.8f, TILT_DIR_G);
  // Tilt as a fraction of each side's step: 1.0 = exactly at the step.
  float nx = gx >= 0 ? gx / dirR : gx / dirL;
  float ny = gy >= 0 ? gy / dirF : gy / dirK;

  // Strongest axis decides the direction.
  if (upright || fmaxf(fabsf(nx), fabsf(ny)) < 1.0f) tiltDir = T_LEVEL;
  else if (fabsf(nx) >= fabsf(ny)) tiltDir = gx > 0 ? T_RIGHT : T_LEFT;
  else tiltDir = gy > 0 ? T_FWD : T_BACK;

  rockCheck(gx, now);

  // How close are we to a LEFT/RIGHT step? (drives the edge glow hint)
  float on   = gx >= 0 ? onR : onL;
  float hint = on * (NAV_HINT / NAV_ON), off = on * (NAV_OFF / NAV_ON);
  navProgress = 0;
  if (!upright && fabsf(gx) > hint && fabsf(gx) >= fabsf(gy))
    navProgress = constrain((fabsf(gx) - hint) / (on - hint), 0.0f, 1.0f)
                  * (gx > 0 ? 1 : -1);

  // LEFT/RIGHT events: ONE step per tilt. Tilt past the step and hold it
  // there briefly -> one step. Then you must come back near level
  // (under `off`) before the next step. No racing through lists.
  int8_t want = gx > onR ? 1 : (gx < -onL ? -1 : 0);
  if (fabsf(gy) > fabsf(gx) || upright || faceDown) want = 0;
  if (now - lastPressMs < NAV_AFTER_PRESS_MS) want = 0;   // pressing jolts it

  if (navDir == 0) {                     // armed, waiting for a tilt
    if (want == 0) {
      navPendingSince = 0;
    } else if (navPendingSince == 0) {
      navPendingSince = now;             // start the "hold it there" timer
    } else if (now - navPendingSince >= NAV_HOLD_MS && now - lastNavMs >= NAV_MIN_GAP_MS) {
      lastNavMs = now;
      navDir = want;                     // fire once, then disarm
      navPendingSince = 0;
      pushEvent(want > 0 ? EV_RIGHT : EV_LEFT);
    }
  } else if (fabsf(gx) < off) {
    navDir = 0;                          // back near level: re-armed
  }
}

void inputUpdate() {
  readButton();
  readSerial();
  readTilt();
}
