#pragma once
// =====================================================================
//  Apps.h - every app on the multitool
// =====================================================================
//  Each app is up to three plain functions:
//    xxxEnter()        once, when the app opens (reset its state)
//    xxxFrame(Event)   ~50x a second: react to the event, draw into fb.
//                      Return false to go back to the menu.
//    xxxLeave()        optional clean-up when leaving
//  Convention: HOLD (long press) = back, everywhere.
// =====================================================================

// ---------- shared helpers ----------

bool bleOK() { return bleKeyboard.isConnected(); }

void drawBleWaiting() {   // pulsing yellow Bluetooth rune = "pair me first"
  uint8_t v = beatsin8(40, 30, 255);
  drawSpriteTint(SPR_BT, 0, 0, CRGB(v, v * 3 / 4, 0));
}


// =====================================================================
//  MEDIA - Bluetooth music remote. Tilt picks the action, tap sends it.
//    level = play/pause   left = previous   right = next
//    forward = volume up  back = volume down
//    double tap = mute
//    VOLUME DIAL: keep it tilted forward/back for half a second and the
//    volume keeps moving (lights show a bar) until you level it again.
// =====================================================================

const uint16_t DIAL_START_MS = 550, DIAL_STEP_MS = 200;
uint32_t dialSince = 0, dialNext = 0, dialShowUntil = 0;
int8_t   dialLevel = 12;   // a 0..25 guide bar, not the PC's real volume

void mediaEnter() { dialSince = 0; dialShowUntil = 0; }

bool mediaFrame(Event e) {
  if (e == EV_HOLD) return false;
  bool conn = bleOK();
  uint32_t now = millis();

  if (e == EV_DOUBLE) {
    if (!conn) fxError();
    else {
      bleKeyboard.write(KEY_MEDIA_MUTE);
      flash();
      fxRipple(CRGB(255, 90, 0));
      Serial.println("[MEDIA] MUTE");
    }
  }

  // Volume dial: held forward/back tilt repeats volume steps.
  bool volTilt = conn && (tiltDir == T_FWD || tiltDir == T_BACK);
  if (!volTilt) {
    dialSince = 0;
  } else if (!dialSince) {
    dialSince = now;
    dialNext = now + DIAL_START_MS;
  } else if (now >= dialNext) {
    bool up = tiltDir == T_FWD;
    bleKeyboard.write(up ? KEY_MEDIA_VOLUME_UP : KEY_MEDIA_VOLUME_DOWN);
    dialLevel = constrain(dialLevel + (up ? 1 : -1), 0, 25);
    dialNext = now + DIAL_STEP_MS;
    dialShowUntil = now + 900;
  }

  if (e == EV_TAP) {
    if (!conn) {
      fxError();
      Serial.println("[MEDIA] Not paired - nothing sent");
    } else {
      const char* what;
      switch (tiltDir) {
        case T_LEFT:  bleKeyboard.write(KEY_MEDIA_PREVIOUS_TRACK); what = "PREVIOUS"; break;
        case T_RIGHT: bleKeyboard.write(KEY_MEDIA_NEXT_TRACK);     what = "NEXT";     break;
        case T_FWD:   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);      what = "VOL+";
                      dialLevel = min(25, dialLevel + 1); dialShowUntil = now + 700; break;
        case T_BACK:  bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);    what = "VOL-";
                      dialLevel = max(0, dialLevel - 1); dialShowUntil = now + 700; break;
        default:      bleKeyboard.write(KEY_MEDIA_PLAY_PAUSE);     what = "PLAY/PAUSE";
      }
      flash();
      fxRipple(CRGB(120, 0, 255));
      Serial.printf("[MEDIA] %s\n", what);
    }
  }

  clearFb();
  if (!conn) { drawBleWaiting(); return true; }
  if (now < dialShowUntil) {   // volume bar: fills bottom-up, left to right
    for (int i = 0; i < 25; i++) {
      int x = i % 5, y = 4 - i / 5;
      if (i < dialLevel) px(x, y, CHSV(96 - i * 3, 255, 255));
      else px(x, y, CRGB(8, 8, 12));
    }
    return true;
  }
  const char* g;
  switch (tiltDir) {
    case T_LEFT:  g = SPR_PREV;  break;
    case T_RIGHT: g = SPR_NEXT;  break;
    case T_FWD:   g = SPR_VOLUP; break;
    case T_BACK:  g = SPR_VOLDN; break;
    default:      g = SPR_PLAY;
  }
  drawSpriteFx(g);
  return true;
}

// =====================================================================
//  TV - IR remote. Tilt left/right to pick a button, tap to send.
//  Items: POWER VOL+ VOL- MUTE CH+ CH- INPUT | BRAND | FIND TV
//   - POWER is UNIVERSAL: it sends the power code of every TV in the list
//     (your picked TV first). Tap or hold to stop once your TV reacts.
//   - Every other button needs your TV picked first (BRAND or FIND TV).
//   - tilt FORWARD / BACK and hold: volume keeps going up / down
//   - FIND TV: tries power codes slowly. Tap the moment your TV switches
//     off; Pocket Nova switches it back on to double-check, tap to save.
// =====================================================================

enum TvItem { TV_BRAND = CMD_COUNT, TV_FIND, TV_ITEMS };
const char* const TV_ITEM_SPR[] = {SPR_POWER, SPR_TVUP, SPR_TVDN, SPR_MUTE,
                                   SPR_CHUP, SPR_CHDN, SPR_INPUT, nullptr, SPR_FIND};
const CRGB     TV_COLOR(150, 0, 255);
const uint16_t SWEEP_STEP_MS   = 1300;   // FIND TV: time per power code
const uint16_t UNI_GAP_MS      = 60;     // universal power: gap between codes
const uint16_t TV_DIAL_START   = 450;    // hold a forward/back tilt this long...
const uint16_t TV_DIAL_STEP    = 300;    // ...then volume repeats this often
const uint16_t CONFIRM_WAIT_MS = 2200;   // pause before switching the TV back on
const uint16_t CONFIRM_TAP_MS  = 6000;   // time to confirm it came back on

enum FindPhase : uint8_t { FIND_OFF, FIND_SWEEP, FIND_CONFIRM };
Carousel  tvMenu;
FindPhase findPhase = FIND_OFF;
uint16_t  findIdx = 0;          // sweep position
uint32_t  findAt = 0;           // when the current code was sent
int16_t   candidates[3];        // codes to double-check, best guess first
uint8_t   candTry = 0;
bool      candSent = false;
uint32_t  tvDialSince = 0, tvDialNext = 0;
bool      uniRunning = false;   // universal power in progress
int16_t   uniStep = 0;          // -1 = picked TV's own code, then 0.. = POWER_SWEEP
uint32_t  uniNextAt = 0;

bool tvPicked() { return cfg.tvBrand < BRAND_COUNT; }

void tvDrawItem(int i, int xo) {
  if (i == TV_BRAND) {
    if (!tvPicked()) {             // blinking "?" = no TV picked yet
      if ((millis() / 400) % 2) drawGlyph('?', 1 + xo, 0, CRGB(255, 200, 0));
    } else {
      drawGlyph(BRANDS[cfg.tvBrand].letter, 1 + xo, 0, BRANDS[cfg.tvBrand].color);
    }
  } else {
    // Control buttons look dim until a TV is picked; POWER always works.
    if (i != CMD_POWER && !tvPicked()) drawSpriteTint(TV_ITEM_SPR[i], xo, 0, CRGB(30, 30, 40));
    else drawSpriteFx(TV_ITEM_SPR[i], xo, 0);
  }
}

const char* tvItemName(int i) {
  if (i == CMD_POWER) return "POWER - ANY TV";
  if (i < CMD_COUNT) return CMD_NAMES[i];
  return i == TV_BRAND ? tvBrandName(cfg.tvBrand) : "FIND TV";
}

void tvEnter() {
  findPhase = FIND_OFF;
  uniRunning = false;
  tvDialSince = 0;
  tvMenu.reset(TV_ITEMS, CMD_POWER);
  tvMenu.showLabelNow(tvBrandName(cfg.tvBrand), tvPicked() ? BRANDS[cfg.tvBrand].color : CRGB(255, 200, 0));
}

// ---- universal power ----
void startUniversalPower() {
  findPhase = FIND_OFF;
  uniRunning = true;
  uniStep = tvPicked() ? -1 : 0;   // your own TV goes first
  uniNextAt = millis();
  Serial.printf("[TV] Universal power: %d codes. Tap or hold to stop.\n", SWEEP_COUNT);
}

bool isPickedPower(const IrCode& c) {
  if (!tvPicked()) return false;
  const IrCode& p = BRANDS[cfg.tvBrand].codes[CMD_POWER];
  return c.proto == p.proto && c.addr == p.addr && c.data == p.data;
}

bool tvUniversalFrame(Event e, uint32_t now) {
  if (e == EV_TAP || e == EV_HOLD) {
    uniRunning = false;
    fxRipple(CRGB::Green);
    Serial.printf("[TV] Universal power stopped at %d/%d\n", max<int16_t>(uniStep, 0), SWEEP_COUNT);
    return true;
  }
  if (now >= uniNextAt) {
    if (uniStep < 0) {
      irSendBrandCmd(cfg.tvBrand, CMD_POWER);
    } else {
      while (uniStep < SWEEP_COUNT && isPickedPower(POWER_SWEEP[uniStep].code)) uniStep++;  // already sent
      if (uniStep >= SWEEP_COUNT) {
        uniRunning = false;
        fxRipple(TV_COLOR);
        Serial.println("[TV] Universal power done");
        return true;
      }
      irSendCode(POWER_SWEEP[uniStep].code);
    }
    uniStep++;
    uniNextAt = millis() + UNI_GAP_MS;
  }
  // Pulsing power symbol over a progress fill.
  clearFb();
  float p = (float)max<int16_t>(uniStep, 0) / SWEEP_COUNT * 25.0f;
  for (int i = 0; i < 25; i++) if (i < (int)p) fb[i] = CRGB(20, 0, 45);
  drawSpriteTint(SPR_POWER, 0, 0, CRGB(beatsin8(150, 90, 255), 0, 0));
  return true;
}

// Non-power buttons need a TV. Returns false (and points at BRAND) if none.
bool tvNeedsPick() {
  if (tvPicked()) return false;
  fxError();
  tvMenu.reset(TV_ITEMS, TV_BRAND);
  tvMenu.showLabelNow("PICK YOUR TV", CRGB(255, 200, 0));
  Serial.println("[TV] Pick your TV first (BRAND or FIND TV)");
  return true;
}

void startFind() {
  uniRunning = false;
  findPhase = FIND_SWEEP;
  findIdx = 0;
  findAt = millis();
  irSendCode(POWER_SWEEP[0].code);
  fxRipple(TV_COLOR);
  Serial.printf("[TV] Finding: %d power codes. Tap the moment your TV switches off.\n", SWEEP_COUNT);
}

void saveFound(int16_t idx) {
  const SweepEntry& s = POWER_SWEEP[idx];
  findPhase = FIND_OFF;
  if (s.brand < 0) {   // its power code is known, but no full button set for it
    tvMenu.reset(TV_ITEMS, CMD_POWER);
    tvMenu.showLabelNow("POWER ONLY - TRY BRANDS", CRGB(255, 200, 0));
    Serial.printf("[TV] %s: only POWER is known for this TV\n", s.label);
    return;
  }
  cfg.tvBrand = s.brand;
  saveConfig();
  tvMenu.reset(TV_ITEMS, CMD_VOLUP);
  tvMenu.showLabelNow(BRANDS[s.brand].name, CRGB::Green);
  fxRipple(CRGB::Green);
  Serial.printf("[TV] Saved: %s\n", BRANDS[s.brand].name);
}

bool tvFindFrame(Event e, uint32_t now) {
  if (e == EV_HOLD) {
    findPhase = FIND_OFF;
    fxError();
    Serial.println("[TV] Find cancelled");
    return true;
  }
  clearFb();

  if (findPhase == FIND_SWEEP) {
    if (e == EV_TAP) {
      // TVs react ~0.3-1 s after a code arrives, so a very quick tap
      // probably belongs to the code before this one.
      int16_t a = (now - findAt < 450 && findIdx > 0) ? findIdx - 1 : findIdx;
      int16_t b = a == findIdx ? (int16_t)findIdx - 1 : findIdx;
      candidates[0] = a; candidates[1] = b; candidates[2] = a > 1 ? a - 2 : -1;
      candTry = 0; candSent = false; findAt = now;
      findPhase = FIND_CONFIRM;
      Serial.printf("[TV] Got it? Switching your TV back on with: %s\n", POWER_SWEEP[a].label);
      return true;
    }
    if (now - findAt >= SWEEP_STEP_MS) {
      if (++findIdx >= SWEEP_COUNT) {
        findPhase = FIND_OFF;
        fxError();
        Serial.println("[TV] Went through every code - no TV confirmed");
        return true;
      }
      findAt = now;
      irSendCode(POWER_SWEEP[findIdx].code);
      fxRipple(TV_COLOR);
    }
    // Progress: the 25 LEDs fill up as the sweep goes through the list.
    float p = (float)(findIdx + 1) / SWEEP_COUNT * 25.0f;
    for (int i = 0; i < 25; i++) if (i < (int)p) fb[i] = CRGB(30, 0, 60);
    int8_t bi = POWER_SWEEP[findIdx].brand;
    if (bi >= 0) drawGlyph(BRANDS[bi].letter, 1, 0, BRANDS[bi].color);
    else drawSprite(SPR_POWER);
    return true;
  }

  // FIND_CONFIRM: switch the TV back on with the best guess and ask.
  int16_t cand = candidates[candTry];
  if (cand < 0 || candTry >= 3) {
    findPhase = FIND_OFF;
    fxError();
    Serial.println("[TV] Couldn't confirm - try FIND TV again");
    return true;
  }
  if (!candSent && now - findAt >= CONFIRM_WAIT_MS) {
    irSendCode(POWER_SWEEP[cand].code);
    candSent = true;
    findAt = now;
    Serial.printf("[TV] Sent %s - tap if your TV came back on\n", POWER_SWEEP[cand].label);
  }
  if (candSent && e == EV_TAP) { saveFound(cand); return true; }
  if (candSent && now - findAt > CONFIRM_TAP_MS) {   // no reaction: try the next guess
    candTry++;
    candSent = false;
    findAt = now - CONFIRM_WAIT_MS;
    return true;
  }
  if ((now / 300) % 2) drawGlyph('?', 1, 0, candSent ? CRGB(0, 255, 80) : CRGB(255, 200, 0));
  return true;
}

bool tvFrame(Event e) {
  uint32_t now = millis();
  if (uniRunning) return tvUniversalFrame(e, now);
  if (findPhase != FIND_OFF) return tvFindFrame(e, now);

  if (e == EV_HOLD) return false;
  if (e == EV_LEFT)  tvMenu.move(-1);
  if (e == EV_RIGHT) tvMenu.move(+1);
  if (e == EV_TAP) {
    int i = tvMenu.index;
    if (i == CMD_POWER) {
      startUniversalPower();
      return tvUniversalFrame(EV_NONE, now);
    } else if (i < CMD_COUNT) {
      if (!tvNeedsPick()) {
        if (irSendCmd(cfg.tvBrand, i)) { flash(); fxRipple(TV_COLOR); }
        else fxError();   // this brand has no such button
      }
    } else if (i == TV_BRAND) {
      cfg.tvBrand = tvPicked() ? (cfg.tvBrand + 1) % BRAND_COUNT : 0;
      saveConfig();
      tvMenu.showLabelNow(tvBrandName(cfg.tvBrand), BRANDS[cfg.tvBrand].color);
      Serial.printf("[TV] Brand: %s\n", tvBrandName(cfg.tvBrand));
    } else {
      startFind();
    }
  }

  // Volume dial: a held forward/back tilt keeps changing the volume.
  bool volTilt = tvPicked() && (tiltDir == T_FWD || tiltDir == T_BACK);
  if (!volTilt) tvDialSince = 0;
  else if (!tvDialSince) { tvDialSince = now; tvDialNext = now + TV_DIAL_START; }
  else if (now >= tvDialNext) {
    irSendCmd(cfg.tvBrand, tiltDir == T_FWD ? CMD_VOLUP : CMD_VOLDN);
    tvDialNext = millis() + TV_DIAL_STEP;
  }

  clearFb();
  if (tvDialSince && now - tvDialSince >= TV_DIAL_START) {   // show which way it's going
    drawSprite(tiltDir == T_FWD ? SPR_TVUP : SPR_TVDN);
    return true;
  }
  tvMenu.draw(tvDrawItem, tvItemName, TV_COLOR);
  return true;
}

// =====================================================================
//  SLIDES - presentation clicker (PowerPoint / Google Slides / Canva)
//    level or right = next   left = previous
//    forward = start show (F5)   back = end show (Esc)
// =====================================================================

bool slidesFrame(Event e) {
  if (e == EV_HOLD) return false;
  bool conn = bleOK();

  if (e == EV_TAP) {
    if (!conn) {
      fxError();
    } else {
      const char* what;
      switch (tiltDir) {
        case T_LEFT: bleKeyboard.write(KEY_LEFT_ARROW);  what = "PREV SLIDE"; break;
        case T_FWD:  bleKeyboard.write(KEY_F5);          what = "START (F5)"; break;
        case T_BACK: bleKeyboard.write(KEY_ESC);         what = "END (ESC)";  break;
        default:     bleKeyboard.write(KEY_RIGHT_ARROW); what = "NEXT SLIDE";
      }
      flash();
      fxRipple(CRGB(0, 255, 80));
      Serial.printf("[SLIDES] %s\n", what);
    }
  }

  clearFb();
  if (!conn) { drawBleWaiting(); return true; }
  const char* g = SPR_ARROW_R;
  if (tiltDir == T_LEFT) g = SPR_ARROW_L;
  else if (tiltDir == T_FWD) g = SPR_START;
  else if (tiltDir == T_BACK) g = SPR_STOP;
  drawSpriteFx(g);
  return true;
}

// =====================================================================
//  KEYS - Windows shortcuts. Tilt to pick, tap to fire.
// =====================================================================

// The shortcuts and your list live in Shortcuts.h; pick them in the PC
// panel's Keys tab. LOCK PC and CLOSE need a second tap within 2 s, so a
// bump can't lock your computer or close a window.
const CRGB KEYS_COLOR(255, 120, 0);
Carousel keysMenu;
const uint16_t LOCK_CONFIRM_MS = 2000;
uint32_t lockArmedAt = 0;

void keysDrawItem(int i, int xo) {
  uint8_t id = keyList[i];
  bool armed = keyNeedsConfirm(id) && lockArmedAt && millis() - lockArmedAt < LOCK_CONFIRM_MS;
  if (armed && xo == 0) {   // blinking "?" = tap again to confirm
    if ((millis() / 180) % 2) drawGlyph('?', 1, 0, CRGB(255, 200, 0));
    return;
  }
  if (id < SHORTCUT_COUNT) { drawSpriteFx(SHORTCUTS[id].sprite, xo, 0); return; }
  // A custom shortcut: its first letter, with a corner dot so it reads as "yours".
  drawGlyph(keyName(id)[0], xo + 1, 0, CRGB(255, 60, 200));
  px(xo + 4, 0, CRGB(255, 200, 0));
}
const char* keysName(int i) { return keyName(keyList[i]); }
void keysEnter() { keysMenu.reset(keyListLen ? keyListLen : 1); lockArmedAt = 0; }

bool keysFrame(Event e) {
  if (e == EV_HOLD) return false;
  clearFb();
  if (!bleOK()) {
    if (e == EV_TAP) fxError();
    drawBleWaiting();
    return true;
  }
  if (!keyListLen) {                   // everything was removed in the panel
    static Scroller none;
    if (!none.active) none.start("NO KEYS - PICK IN PANEL", KEYS_COLOR);
    none.draw();
    return true;
  }
  if (keysMenu.count != keyListLen) keysMenu.reset(keyListLen, min<int>(keysMenu.index, keyListLen - 1));
  if (e == EV_LEFT || e == EV_RIGHT) lockArmedAt = 0;
  if (e == EV_LEFT)  keysMenu.move(-1);
  if (e == EV_RIGHT) keysMenu.move(+1);
  uint8_t id = keyList[keysMenu.index];
  if (e == EV_TAP && keyNeedsConfirm(id)) {
    if (!lockArmedAt || millis() - lockArmedAt > LOCK_CONFIRM_MS) {
      lockArmedAt = millis();          // first tap: arm and wait for the second
      Serial.printf("[KEYS] Tap again for %s\n", keyName(id));
      e = EV_NONE;
    } else {
      lockArmedAt = 0;                 // second tap: go
    }
  }
  if (e == EV_TAP) {
    keyFire(id);
    flash();
    fxRipple(KEYS_COLOR);
  }
  keysMenu.draw(keysDrawItem, keysName, KEYS_COLOR);
  return true;
}

// =====================================================================
//  LEVEL - spirit level. The bubble floats to the high side.
//  Tap = "this is level now" (zero it on your desk).
// =====================================================================

bool levelFrame(Event e) {
  if (e == EV_HOLD) return false;
  if (e == EV_TAP) {
    cfg.levelX0 = tiltX;
    cfg.levelY0 = tiltY;
    saveConfig();
    fxRipple(CRGB::Green);
    Serial.println("[LEVEL] Zeroed");
  }
  float x = tiltX - cfg.levelX0, y = tiltY - cfg.levelY0;
  // Bubble position as a FLOAT (~10 degrees per pixel), then eased so it
  // glides like a real bubble instead of jumping between dots.
  static float bxs = 2, bys = 2;
  float tx = constrain(2 - x * 6, 0.0f, 4.0f);
  float ty = constrain(2 + y * 6, 0.0f, 4.0f);
  bxs += (tx - bxs) * 0.25f;
  bys += (ty - bys) * 0.25f;
  float off = max(fabsf(bxs - 2), fabsf(bys - 2));
  bool level = off < 0.3f;   // within ~2 degrees

  clearFb();
  CRGB cross = level ? CRGB(0, beatsin8(60, 10, 60), 0) : CRGB(0, 0, 25);
  for (int i = 0; i < 5; i++) { px(2, i, cross); px(i, 2, cross); }

  // Sub-pixel drawing: split the bubble's light across the 4 nearest LEDs
  // in proportion to how close it is to each one (bilinear blending).
  CRGB c = level ? CRGB::Green : off < 1.2f ? CRGB(255, 180, 0) : CRGB::Red;
  int x0 = (int)bxs, y0 = (int)bys;
  float fx = bxs - x0, fy = bys - y0;
  addPx(x0,     y0,     CRGB(c).nscale8_video((1 - fx) * (1 - fy) * 255));
  addPx(x0 + 1, y0,     CRGB(c).nscale8_video(fx * (1 - fy) * 255));
  addPx(x0,     y0 + 1, CRGB(c).nscale8_video((1 - fx) * fy * 255));
  addPx(x0 + 1, y0 + 1, CRGB(c).nscale8_video(fx * fy * 255));
  return true;
}

// =====================================================================
//  DICE - shake or tap to roll.
// =====================================================================

uint8_t  diceFace = 6, diceStep = 0, diceHue = 0;
bool     diceRolling = false;
uint32_t diceNext = 0;

bool diceFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  if ((e == EV_TAP || e == EV_SHAKE) && !diceRolling) {
    diceRolling = true;
    diceStep = 0;
    diceNext = now;
  }
  if (diceRolling && now >= diceNext) {
    uint8_t f;
    do { f = 1 + esp_random() % 6; } while (f == diceFace);   // always changes
    diceFace = f;
    diceHue = esp_random();
    diceStep++;
    diceNext = now + 40 + diceStep * diceStep * 5;   // slows down like a real die
    if (diceStep >= 11) {
      diceRolling = false;
      fxRipple(CHSV(diceHue, 255, 160));
      Serial.printf("[DICE] Rolled %d\n", diceFace);
    }
  }
  clearFb();
  drawSpriteTint(DICE_FACES[diceFace - 1], 0, 0,
                 diceRolling ? CRGB(CHSV(diceHue, 255, 255)) : CRGB(CHSV(diceHue, 120, 255)));
  return true;
}

// =====================================================================
//  TIMER - focus timer. Tilt to choose minutes, tap to start/pause.
//  The 25 LEDs drain as time runs out (green -> yellow -> red).
// =====================================================================

const uint8_t TIMER_PRESETS[] = {1, 3, 5, 10, 15, 25, 45, 60};
enum { TM_SET, TM_RUN, TM_PAUSE, TM_DONE } tmState = TM_SET;
uint8_t  tmPreset = 5;   // index -> 25 minutes
uint32_t tmTotal = 0, tmEnd = 0, tmRemain = 0;
Scroller tmScroll;

void timerEnter() { tmState = TM_SET; tmScroll.stop(); }

void drawTimeLeft(uint32_t remain, bool blinkOff) {
  float f = (float)remain / tmTotal;
  float lit = f * 25.0f;
  CRGB c = CHSV((uint8_t)(96 * f), 255, 255);
  for (int i = 0; i < 25; i++) {
    if (i < (int)lit) fb[i] = c;
    else if (i == (int)lit) fb[i] = c.nscale8_video((uint8_t)((lit - i) * 255)); // partial pixel
  }
  if (blinkOff) clearFb();
}

bool timerFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  clearFb();

  switch (tmState) {
    case TM_SET: {
      if (e == EV_LEFT || e == EV_RIGHT) {
        int n = FRAMES(TIMER_PRESETS);
        tmPreset = (tmPreset + (e == EV_RIGHT ? 1 : n - 1)) % n;
        tmScroll.stop();
      }
      if (e == EV_TAP) {
        tmTotal = TIMER_PRESETS[tmPreset] * 60000UL;
        tmEnd = now + tmTotal;
        tmState = TM_RUN;
        fxRipple(CRGB::Green);
        Serial.printf("[TIMER] %d min started\n", TIMER_PRESETS[tmPreset]);
        break;
      }
      uint8_t m = TIMER_PRESETS[tmPreset];
      if (m < 10) {
        drawGlyph('0' + m, 1, 0, CRGB(255, 200, 0));
      } else {
        if (!tmScroll.draw()) {   // two digits don't fit: keep scrolling
          char buf[8];
          snprintf(buf, sizeof(buf), "%d", m);
          tmScroll.start(buf, CRGB(255, 200, 0));
          tmScroll.draw();
        }
      }
      break;
    }
    case TM_RUN: {
      if (e == EV_TAP) { tmRemain = tmEnd - now; tmState = TM_PAUSE; break; }
      if ((int32_t)(tmEnd - now) <= 0) {
        tmState = TM_DONE;
        Serial.println("[TIMER] Done!");
        break;
      }
      drawTimeLeft(tmEnd - now, false);
      break;
    }
    case TM_PAUSE:
      if (e == EV_TAP) { tmEnd = now + tmRemain; tmState = TM_RUN; break; }
      drawTimeLeft(tmRemain, (now / 400) % 2);
      break;
    case TM_DONE:
      if (e == EV_TAP) { tmState = TM_SET; break; }
      for (int i = 0; i < 25; i++)
        fb[i] = CHSV(now / 8 + i * 10, 255, beatsin8(90, 0, 255));
      break;
  }
  return true;
}

// =====================================================================
//  LIGHTS - ambient mood lighting. Tap or tilt to change effect.
// =====================================================================

const char* const LIGHT_NAMES[] = {"RAINBOW", "FIRE", "OCEAN", "BREATHE", "TILT", "SPARKLE"};
const uint8_t LIGHT_COUNT = FRAMES(LIGHT_NAMES);
uint8_t  lightIdx = 0;
uint8_t  heat[5][5];   // fire simulation: heat[x][y]
uint32_t fireNext = 0;
Scroller lightLabel;

void lightEnter() { lightLabel.start(LIGHT_NAMES[lightIdx], CRGB::White); }

void drawFire(uint32_t now) {
  if (now >= fireNext) {
    fireNext = now + 55;
    for (int x = 0; x < 5; x++) {
      for (int y = 0; y < 5; y++) heat[x][y] = qsub8(heat[x][y], random8(20, 70));   // cool
      for (int y = 0; y < 4; y++)                                                      // rise
        heat[x][y] = (heat[x][y + 1] * 2 + heat[x][min(y + 2, 4)]) / 3;
      if (random8() < 170) heat[x][4] = qadd8(heat[x][4], random8(140, 255));        // spark
    }
  }
  for (int x = 0; x < 5; x++)
    for (int y = 0; y < 5; y++) px(x, y, HeatColor(heat[x][y]));
}

bool lightFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  if (e == EV_TAP || e == EV_RIGHT || e == EV_LEFT) {
    lightIdx = (lightIdx + (e == EV_LEFT ? LIGHT_COUNT - 1 : 1)) % LIGHT_COUNT;
    lightLabel.start(LIGHT_NAMES[lightIdx], CRGB::White);
    clearFb();
  }
  if (lightLabel.active) { clearFb(); lightLabel.draw(); return true; }

  switch (lightIdx) {
    case 0:  // RAINBOW
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++) px(x, y, CHSV((x + y) * 18 + now / 8, 255, 255));
      break;
    case 1: drawFire(now); break;
    case 2:  // OCEAN
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
          px(x, y, CHSV(140 + (sin8(x * 30 + now / 6) >> 3), 255,
                        60 + scale8(sin8(y * 60 + x * 20 + now / 4), 195)));
      break;
    case 3:  // BREATHE
      fill_solid(fb, 25, CHSV(now / 200, 230, beatsin8(10, 15, 255)));
      break;
    case 4: {  // TILT: colour follows the direction you tip it
      float ang = atan2f(tiltY, tiltX);
      float mag = sqrtf(tiltX * tiltX + tiltY * tiltY);
      uint8_t hue = (uint8_t)((ang + PI) / (2 * PI) * 255);
      fill_solid(fb, 25, CHSV(hue, 255, constrain((int)(mag * 500), 25, 255)));
      break;
    }
    default:  // SPARKLE (fades what was there, so no clearFb)
      fadeToBlackBy(fb, 25, 25);
      if (random8() < 70) fb[random8(25)] = CHSV(random8(), 160, 255);
      break;
  }
  return true;
}

// =====================================================================
//  TORCH - flashlight. Tap: white -> warm -> red (night vision) -> off.
//  Turns itself off after 3 minutes so the LEDs don't cook.
// =====================================================================

uint8_t  torchMode = 0;
uint32_t torchOnAt = 0;
const uint32_t TORCH_AUTO_OFF_MS = 180000;

void torchEnter() { torchMode = 0; torchOnAt = millis(); brightnessOverride = 40; }
void torchLeave() { brightnessOverride = 0; }

bool torchFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  if (e == EV_TAP) { torchMode = (torchMode + 1) % 4; torchOnAt = now; }
  if (torchMode != 3 && now - torchOnAt > TORCH_AUTO_OFF_MS) {
    torchMode = 3;
    Serial.println("[TORCH] Auto-off");
  }
  switch (torchMode) {
    case 0: fill_solid(fb, 25, CRGB(255, 255, 255)); break;
    case 1: fill_solid(fb, 25, CRGB(255, 140, 40)); break;
    case 2: fill_solid(fb, 25, CRGB(255, 0, 0)); break;
    default: clearFb(); px(2, 2, CRGB(beatsin8(20, 2, 25), 0, 0)); break;
  }
  return true;
}

// =====================================================================
//  SNAKE - tilt to steer, eat the red dots. Edges wrap around.
// =====================================================================

uint8_t  snX[25], snY[25], snLen = 0, foodX = 0, foodY = 0, snScore = 0;
int8_t   snDx = 1, snDy = 0;
enum { SN_READY, SN_PLAY, SN_DEAD, SN_WIN } snState = SN_READY;
uint32_t snNext = 0, snDeadAt = 0;
bool     snNewBest = false;
Scroller snScroll;

bool snakeOn(uint8_t x, uint8_t y) {
  for (int i = 0; i < snLen; i++) if (snX[i] == x && snY[i] == y) return true;
  return false;
}

bool placeFood() {
  if (snLen >= 25) return false;
  do { foodX = esp_random() % 5; foodY = esp_random() % 5; } while (snakeOn(foodX, foodY));
  return true;
}

void snakeReset() {
  snLen = 2; snScore = 0;
  snX[0] = 1; snY[0] = 2; snX[1] = 0; snY[1] = 2;
  snDx = 1; snDy = 0;
  placeFood();
}

void snakeEnter() { snState = SN_READY; snScroll.stop(); }

void snakeStep() {
  // Steering: tilt direction, but you can't reverse into yourself.
  int8_t dx = snDx, dy = snDy;
  if (tiltDir == T_LEFT)  { dx = -1; dy = 0; }
  if (tiltDir == T_RIGHT) { dx = 1;  dy = 0; }
  if (tiltDir == T_FWD)   { dx = 0;  dy = -1; }
  if (tiltDir == T_BACK)  { dx = 0;  dy = 1; }
  if (!(dx == -snDx && dy == -snDy)) { snDx = dx; snDy = dy; }

  uint8_t nx = (snX[0] + snDx + 5) % 5, ny = (snY[0] + snDy + 5) % 5;
  bool eat = nx == foodX && ny == foodY;
  // Hitting your body = game over (the tail tip is moving away, so it's safe).
  for (int i = 0; i < snLen - (eat ? 0 : 1); i++)
    if (snX[i] == nx && snY[i] == ny) {
      snState = SN_DEAD; snDeadAt = millis();
      snNewBest = snScore > cfg.snakeHigh;
      if (snNewBest) { cfg.snakeHigh = snScore; saveConfig(); }
      Serial.printf("[SNAKE] Game over, score %d (best %d)\n", snScore, cfg.snakeHigh);
      return;
    }
  if (eat) snLen++;
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = nx; snY[0] = ny;
  if (eat) {
    snScore++;
    fxRipple(CRGB(0, 120, 0));
    if (!placeFood()) { snState = SN_WIN; snDeadAt = millis(); }
  }
}

bool snakeFrame(Event e) {
  if (e == EV_HOLD) return false;
  uint32_t now = millis();
  clearFb();

  switch (snState) {
    case SN_READY:
      if (e == EV_TAP) { snakeReset(); snState = SN_PLAY; snNext = now + 400; break; }
      drawSprite(ICON_SNAKE[(now / 300) % 3]);
      break;
    case SN_PLAY:
      if (now >= snNext) {
        snakeStep();
        snNext = now + max(180, 450 - snScore * 20);   // speeds up as you grow
      }
      if ((now / 150) % 2) px(foodX, foodY, CRGB::Red);
      for (int i = snLen - 1; i >= 0; i--)
        px(snX[i], snY[i], i == 0 ? CRGB(0, 255, 0) : CRGB(0, max(30, 140 - i * 15), 0));
      break;
    case SN_DEAD:
    case SN_WIN:
      if (e == EV_TAP) { snakeReset(); snState = SN_PLAY; snNext = now + 400; snScroll.stop(); break; }
      if (now - snDeadAt < 700) {
        CRGB c = snState == SN_WIN ? CRGB(CHSV(now / 3, 255, 255)) : CRGB::Red;
        if ((now / 120) % 2) for (int i = 0; i < snLen; i++) px(snX[i], snY[i], c);
      } else if (!snScroll.draw()) {
        char buf[24];
        if (snState == SN_WIN) snprintf(buf, sizeof(buf), "WIN %d", snScore);
        else if (snNewBest) snprintf(buf, sizeof(buf), "NEW BEST %d!", snScore);
        else snprintf(buf, sizeof(buf), "%d BEST %d", snScore, cfg.snakeHigh);
        snScroll.start(buf, snState == SN_WIN || snNewBest ? CRGB::Green : CRGB(255, 200, 0));
      }
      break;
  }
  return true;
}

// =====================================================================
//  SETTINGS - brightness, screen rotation, tilt calibration, info
// =====================================================================

//  BLUETOOTH  tap = status, DOUBLE tap = switch to the next slot (restarts)
//  PAIR NEW   DOUBLE tap = forget every paired device (restarts)
//  WI-FI      tap = status, DOUBLE tap = open the setup network (again = close it)
//  Risky actions need a double tap so a stray press can't trigger them.

enum { ST_BRIGHT, ST_ROTATE, ST_AUTOROT, ST_BT, ST_PAIR, ST_WIFI, ST_CALIB, ST_TUTOR, ST_INFO, ST_ITEMS };
const char* const SET_NAMES[] = {"BRIGHT", "ROTATE", "AUTO ROTATE", "BLUETOOTH",
                                 "PAIR NEW", "WI-FI", "CALIBRATE", "TUTORIAL", "INFO"};
const char* const SET_SPR[]   = {SPR_SUN, SPR_ROT, SPR_AUTOROT, SPR_BT,
                                 SPR_PAIR, SPR_WIFI, SPR_CALIB, SPR_TUTOR, SPR_INFO};
Carousel setMenu;
uint8_t  calStep = 0;   // 0 = not calibrating, 1 = waiting for right, 2 = for forward
AxisMap  calLx;

void startTutorial();   // main sketch

void setDrawItem(int i, int xo) {
  if (i == ST_AUTOROT && !cfg.autoRotate) {     // dim when switched off
    drawSpriteTint(SET_SPR[i], xo, 0, CRGB(25, 25, 35));
    return;
  }
  if (i == ST_BT) {                              // blue = connected, yellow = waiting
    drawSpriteTint(SPR_BT, xo, 0, bleOK() ? CRGB(0, 60, 255) : CRGB(160, 120, 0));
    for (int s = 0; s <= cfg.btSlot; s++) px(xo + 4, 4 - s, CRGB(60, 60, 60));   // slot dots
    return;
  }
  if (i == ST_WIFI) {   // magenta pulse = setup network open, green = online, yellow = joining
    CRGB c = setupOn ? CRGB(beatsin8(40, 60, 255), 0, beatsin8(40, 60, 255))
           : wifiState == WF_ONLINE ? CRGB(0, 200, 60)
           : wifiState == WF_CONNECTING ? CRGB(160, 120, 0)
           : wifiState == WF_FAILED ? CRGB(200, 30, 0) : CRGB(25, 25, 35);
    drawSpriteTint(SPR_WIFI, xo, 0, c);
    return;
  }
  drawSpriteFx(SET_SPR[i], xo, 0);
}
const char* setName(int i) { return SET_NAMES[i]; }
void settingsEnter() { calStep = 0; setMenu.reset(ST_ITEMS); }

// Which raw axis moved most, and which way?
bool readDominantAxis(AxisMap& m) {
  float ax = rawSx, ay = rawSy;
  m.axis = fabsf(ax) > fabsf(ay) ? 0 : 1;
  float v = m.axis == 0 ? ax : ay;
  m.sign = v > 0 ? 1 : -1;
  return fabsf(v) > 0.25f;   // must actually be tilted
}

bool settingsFrame(Event e) {
  uint32_t now = millis();

  // ---- calibration wizard: "tilt toward the arrow, then tap" ----
  if (calStep) {
    if (e == EV_HOLD) { calStep = 0; fxError(); return true; }
    if (e == EV_TAP) {
      AxisMap m;
      if (!readDominantAxis(m)) { fxError(); return true; }   // not tilted enough
      if (calStep == 1) {
        calLx = m;
        calStep = 2;
        flash();
        Serial.println("[CAL] Now tilt toward the UP arrow (top edge down) and tap");
      } else {
        if (m.axis == calLx.axis) {      // both on the same axis: try again
          calStep = 1;
          fxError();
          Serial.println("[CAL] Same axis twice - start again");
          return true;
        }
        AxisMap Ly = m, Lx = calLx;
        // Store the mapping for rotation 0 so rotating later still works.
        switch (cfg.rotation & 3) {
          case 0: cfg.pX = Lx; cfg.pUp = Ly; break;
          case 1: cfg.pX = Ly; cfg.pUp = {Lx.axis, (int8_t)-Lx.sign}; break;
          case 2: cfg.pX = {Lx.axis, (int8_t)-Lx.sign}; cfg.pUp = {Ly.axis, (int8_t)-Ly.sign}; break;
          default: cfg.pX = {Ly.axis, (int8_t)-Ly.sign}; cfg.pUp = Lx; break;
        }
        saveConfig();
        calStep = 0;
        setMenu.showLabelNow("OK", CRGB::Green);
        Serial.printf("[CAL] Saved: right=axis%d*%d up=axis%d*%d\n",
                      cfg.pX.axis, cfg.pX.sign, cfg.pUp.axis, cfg.pUp.sign);
      }
    }
    clearFb();
    if ((now / 300) % 2) drawSprite(calStep == 1 ? SPR_ARROW_RY : SPR_ARROW_U);
    return true;
  }

  if (e == EV_HOLD) return false;
  if (e == EV_LEFT)  setMenu.move(-1);
  if (e == EV_RIGHT) setMenu.move(+1);
  if (e == EV_DOUBLE) {
    if (setMenu.index == ST_BT) {
      cfg.btSlot = (cfg.btSlot + 1) % BT_SLOTS;
      saveConfig();
      restartSoon("switching Bluetooth slot");
    } else if (setMenu.index == ST_PAIR) {
      int n = bleRemoveAllBonds();
      Serial.printf("[BT] Forgot %d device(s)\n", n);
      restartSoon("ready to pair a new device");
    } else if (setMenu.index == ST_WIFI) {
      if (setupOn) { wifiStopSetup(); setMenu.showLabelNow("SETUP OFF", CRGB::White); return true; }
      wifiStartSetup();
      e = EV_TAP;   // fall through to the tap below: scroll the name and password
    } else {
      e = EV_TAP;   // elsewhere a double tap is just a tap
    }
  }
  if (e == EV_TAP) {
    char buf[48];
    switch (setMenu.index) {
      case ST_AUTOROT:
        cfg.autoRotate = !cfg.autoRotate;
        saveConfig();
        setMenu.showLabelNow(cfg.autoRotate ? "ON" : "OFF", CRGB::White);
        break;
      case ST_BT:
        snprintf(buf, sizeof(buf), "SLOT %d %s - %d SAVED", cfg.btSlot + 1,
                 bleOK() ? "CONNECTED" : "WAITING", bleBondCount());
        setMenu.showLabelNow(buf, CRGB(0, 120, 255));
        break;
      case ST_PAIR:
        setMenu.showLabelNow("DOUBLE TAP TO FORGET ALL", CRGB(255, 120, 0));
        break;
      case ST_WIFI:
        if (setupOn)
          snprintf(buf, sizeof(buf), "JOIN %s PASS %s", apSsid, apPass);
        else if (wifiState == WF_ONLINE)
          snprintf(buf, sizeof(buf), "ONLINE %s", WiFi.localIP().toString().c_str());
        else if (!wifiHasNetwork())
          snprintf(buf, sizeof(buf), "NOT SET - DOUBLE TAP");
        else
          snprintf(buf, sizeof(buf), "%s", wifiState == WF_CONNECTING ? "JOINING" : wifiState == WF_FAILED ? "FAILED" : "OFF");
        setMenu.showLabelNow(buf, CRGB(0, 200, 200));
        break;
      case ST_TUTOR:
        startTutorial();
        return true;
      case ST_BRIGHT:
        cfg.brightIdx = (cfg.brightIdx + 1) % BRIGHT_COUNT;
        saveConfig();
        snprintf(buf, sizeof(buf), "%d", cfg.brightIdx + 1);
        setMenu.showLabelNow(buf, CRGB(255, 200, 0));
        break;
      case ST_ROTATE:
        cfg.rotation = (cfg.rotation + 1) % 4;
        saveConfig();
        flash();
        break;
      case ST_CALIB:
        calStep = 1;
        Serial.println("[CAL] Tilt toward the RIGHT arrow (right edge down) and tap");
        break;
      case ST_INFO:
        snprintf(buf, sizeof(buf), "%s V%s BLE %s TV %s", cfg.name, FW_VERSION, bleOK() ? "ON" : "OFF",
                 tvBrandName(cfg.tvBrand));
        setMenu.showLabelNow(buf, CRGB(0, 120, 255));
        break;
    }
  }
  clearFb();
  setMenu.draw(setDrawItem, setName, CRGB::White);
  return true;
}
