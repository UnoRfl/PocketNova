#pragma once
// =====================================================================
//  Display.h — framebuffer, rotation, sprites, text and effects
// =====================================================================
//  Apps never touch the LEDs directly. They draw into `fb`, a 5x5
//  canvas indexed x + y*5 (x = column, y = row, 0,0 = top-left).
//  The main loop calls present() ~50 times a second, which rotates the
//  canvas to match how you hold the device and pushes it to the LEDs.
//  Drawing the whole screen every frame ("immediate mode") is what makes
//  smooth animation easy: nothing is ever left over from last frame.
// =====================================================================

const uint8_t  LED_PIN = 27;    // Atom Matrix WS2812 data pin
const uint16_t FADE_MS = 180;   // length of the fade-in transition

CRGB leds[25];   // what the hardware shows (after rotation)
CRGB fb[25];     // what apps draw on (upright)

uint8_t  brightnessOverride = 0;  // non-zero = temporary brightness (torch)
float    navProgress = 0;         // -1..1: how close a tilt is to a LEFT/RIGHT step
                                  // (set by Input.h, drawn as an edge glow)
uint32_t fadeInStart = 0;

void displayBegin() {
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, 25);
  FastLED.setBrightness(BRIGHT_LEVELS[cfg.brightIdx]);
  FastLED.clear(true);
}

inline void clearFb() { fill_solid(fb, 25, CRGB::Black); }

inline void px(int x, int y, CRGB c) {
  if (x >= 0 && x < 5 && y >= 0 && y < 5) fb[x + y * 5] = c;
}

inline void addPx(int x, int y, CRGB c) {   // additive: light adds up
  if (x >= 0 && x < 5 && y >= 0 && y < 5) fb[x + y * 5] += c;
}

void fadeIn() { fadeInStart = millis(); }

// Copy the canvas to the LEDs, rotated, faded and at the right brightness.
void present() {
  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 5; x++) {
      int hx, hy;   // hardware coordinates
      switch (cfg.rotation & 3) {
        case 0:  hx = x;     hy = y;     break;
        case 1:  hx = 4 - y; hy = x;     break;
        case 2:  hx = 4 - x; hy = 4 - y; break;
        default: hx = y;     hy = 4 - x; break;
      }
      leds[hx + hy * 5] = fb[x + y * 5];
    }
  }
  uint32_t el = millis() - fadeInStart;
  if (el < FADE_MS) {
    uint8_t s = el * 255 / FADE_MS;
    for (auto& l : leds) l.nscale8_video(s);
  }
  FastLED.setBrightness(brightnessOverride ? brightnessOverride
                                           : BRIGHT_LEVELS[cfg.brightIdx]);
  FastLED.show();
}

// ---------- sprites ----------

CRGB spriteColor(char c) {
  switch (c) {
    case 'R': return CRGB(255, 0, 0);
    case 'G': return CRGB(0, 255, 0);
    case 'B': return CRGB(0, 60, 255);
    case 'C': return CRGB(0, 255, 200);
    case 'M': return CRGB(255, 0, 180);
    case 'Y': return CRGB(255, 200, 0);
    case 'O': return CRGB(255, 90, 0);
    case 'P': return CRGB(140, 0, 255);
    case 'W': return CRGB(255, 255, 255);
    case 'w': return CRGB(40, 40, 50);
    case 'g': return CRGB(0, 60, 0);
    case 'b': return CRGB(0, 15, 70);
    case 'r': return CRGB(70, 0, 0);
    default:  return CRGB::Black;
  }
}

// Draw a 25-char sprite at an offset (offsets let sprites slide off-screen).
void drawSprite(const char* s, int ox = 0, int oy = 0) {
  for (int i = 0; i < 25; i++)
    if (s[i] != '.') px(ox + i % 5, oy + i / 5, spriteColor(s[i]));
}

// Same shape, but every lit pixel in one colour.
void drawSpriteTint(const char* s, int ox, int oy, CRGB tint) {
  for (int i = 0; i < 25; i++)
    if (s[i] != '.') px(ox + i % 5, oy + i / 5, tint);
}

// ---------- feedback effects (drawn on top of every app) ----------

uint32_t flashUntil = 0, errorStart = 0, rippleStart = 0;
CRGB rippleColor;

void flash(uint16_t ms = 110) { flashUntil = millis() + ms; }
bool flashing() { return (int32_t)(flashUntil - millis()) > 0; }
uint32_t lastGoodAt = 0, lastBadAt = 0;   // the pet reads these to pick a mood

void fxRipple(CRGB c) { rippleStart = millis() | 1; rippleColor = c; lastGoodAt = millis(); }
void fxError() { errorStart = millis() | 1; lastBadAt = millis(); }

// Sprite that turns white for a moment after flash() — "button pressed" feel.
void drawSpriteFx(const char* s, int ox = 0, int oy = 0) {
  if (flashing() && ox == 0) drawSpriteTint(s, ox, oy, CRGB::White);
  else drawSprite(s, ox, oy);
}

void drawFx() {
  uint32_t now = millis();
  // Error: blinking red X
  if (errorStart && now - errorStart < 450) {
    clearFb();
    if (((now - errorStart) / 75) % 2 == 0) drawSprite(SPR_X);
  }
  // Ripple: a ring that grows out from the centre
  if (rippleStart && now - rippleStart < 420) {
    float r = (now - rippleStart) / 80.0f;
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++) {
        float d = sqrtf((x - 2) * (x - 2) + (y - 2) * (y - 2));
        if (fabsf(d - r) < 0.6f) addPx(x, y, rippleColor);
      }
  }
}

// ---------- text ----------

void drawGlyph(char c, int ox, int oy, CRGB col) {
  const char* g = glyphFor(c);
  if (!g) return;
  for (int r = 0; r < 5; r++)
    for (int cx = 0; cx < 3; cx++)
      if (g[r * 3 + cx] == '#') px(ox + cx, oy + r, col);
}

void drawText(const char* t, int ox, CRGB col) {
  for (int i = 0; t[i]; i++) drawGlyph(t[i], ox + i * 4, 0, col);
}

// Scrolls a word across the screen, right to left, without blocking.
struct Scroller {
  char     text[112] = "";       // room for "JOIN <32-char network> PASS <63-char password>"
  CRGB     color;
  int      offset = 0;
  uint32_t last = 0;
  bool     active = false;
  uint16_t stepMs = 70;

  void start(const char* t, CRGB c) {
    strncpy(text, t, sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    color = c;
    offset = 5;
    last = millis();
    active = true;
  }
  void stop() { active = false; }
  int width() { return strlen(text) * 4 - 1; }

  // Draws the text; returns false once it has scrolled fully off.
  bool draw() {
    if (!active) return false;
    if (millis() - last >= stepMs) {
      last = millis();
      if (--offset < -width()) { active = false; return false; }
    }
    drawText(text, offset, color);
    return true;
  }
};

// A list you browse by tilting: items slide in/out, and if you rest on
// one for a moment its name scrolls past (like Flipper's menu labels).
struct Carousel {
  int      index = 0, prev = 0, count = 1;
  int8_t   dir = 0;
  uint32_t movedAt = 0;
  bool     labelDone = false;
  Scroller label;
  static const uint16_t SLIDE_MS = 320;   // slide animation length (slower = easier to follow)

  void reset(int n, int start = 0) {
    count = n; index = start; dir = 0;
    movedAt = millis(); labelDone = false; label.stop();
  }
  void move(int8_t d) {
    prev = index;
    index = (index + d + count) % count;
    dir = d;
    movedAt = millis();
    labelDone = false;
    label.stop();
  }
  void showLabelNow(const char* t, CRGB c) { label.start(t, c); labelDone = true; }

  // drawItem(index, xOffset) draws one item; nameOf(index) gives its label.
  void draw(void (*drawItem)(int, int), const char* (*nameOf)(int) = nullptr,
            CRGB labelColor = CRGB::White, uint16_t hoverMs = 1400) {
    uint32_t el = millis() - movedAt;
    if (dir != 0 && el < SLIDE_MS) {
      int k = el * 5 / SLIDE_MS;
      drawItem(prev, -dir * k);
      drawItem(index, dir * (5 - k));
      return;
    }
    dir = 0;
    if (nameOf && !labelDone && el > hoverMs) {
      label.start(nameOf(index), labelColor);
      labelDone = true;
    }
    if (label.draw()) return;
    drawItem(index, 0);
    drawNavHint();
  }

  // Edge glow that brightens as you tilt toward the next item, so you can
  // feel where the "click" point is. Full brightness = about to switch.
  static void drawNavHint() {
    float p = fabsf(navProgress);
    if (p <= 0) return;
    int x = navProgress > 0 ? 4 : 0;
    uint8_t v = 15 + p * 110;
    for (int y = 0; y < 5; y++) {
      uint8_t edge = (y == 2) ? v : v / 2;   // brightest in the middle
      fb[x + y * 5] += CRGB(edge, edge, edge);
    }
  }
};
