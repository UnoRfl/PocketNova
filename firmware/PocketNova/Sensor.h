#pragma once
// =====================================================================
//  Sensor.h - a temperature/humidity sensor on the Grove port
// =====================================================================
//  The Grove socket on the bottom is an I2C bus: two wires, SDA (data,
//  pin 26) and SCL (clock, pin 32), that many little chips can share.
//  Each chip has an address; the SHT temperature/humidity chips in M5's
//  ENV II, III and IV units all answer at 0x44.
//
//  Two kinds, told apart by how they answer:
//    SHT4x (ENV IV)      one-byte command 0xFD = "measure, high precision"
//    SHT3x (ENV II/III)  two-byte command 0x24 0x00 = the same idea
//  Both reply with 6 bytes: temperature (2 bytes + check byte), humidity
//  (2 bytes + check byte). The check byte is a CRC: a small sum that
//  catches a garbled reply, so a wrong guess about the chip shows up as
//  a bad CRC instead of a crazy reading.
//
//  The motion sensor sits on a different bus (Wire1), so this one (Wire)
//  is all ours. Plug the unit in any time: it's looked for every 10 s.
// =====================================================================

#include <Wire.h>

enum SensorKind : uint8_t { SEN_NONE, SEN_SHT3X, SEN_SHT4X };
const char* const SENSOR_NAMES[] = {"none", "SHT3x (ENV II/III)", "SHT4x (ENV IV)"};
const uint8_t SEN_ADDR = 0x44;

SensorKind senKind = SEN_NONE;
float      senTemp = NAN, senHum = NAN;   // degrees C (offset applied) and %RH
uint32_t   senNextAt = 0, senReadAt = 0, senLastOk = 0;
bool       senWaiting = false;            // a measurement was started, read it at senReadAt
bool       senBusUp = false;

uint8_t senCrc(const uint8_t* d, int n) {   // CRC-8, polynomial 0x31, start 0xFF (Sensirion's)
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = c & 0x80 ? (c << 1) ^ 0x31 : c << 1;
  }
  return c;
}

void senCommand(SensorKind k) {
  Wire.beginTransmission(SEN_ADDR);
  if (k == SEN_SHT4X) Wire.write(0xFD);
  else { Wire.write(0x24); Wire.write(0x00); }
  Wire.endTransmission();
}

// Reads the 6-byte answer. Returns false on a missing or garbled reply.
bool senRead(SensorKind k, float& t, float& h) {
  uint8_t b[6];
  if (Wire.requestFrom(SEN_ADDR, (uint8_t)6) != 6) return false;
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (senCrc(b, 2) != b[2] || senCrc(b + 3, 2) != b[5]) return false;
  uint16_t rt = b[0] << 8 | b[1], rh = b[3] << 8 | b[4];
  t = -45 + 175.0f * rt / 65535;
  h = k == SEN_SHT4X ? -6 + 125.0f * rh / 65535 : 100.0f * rh / 65535;
  h = constrain(h, 0.0f, 100.0f);
  return true;
}

// Which chip is there? Try each one's command and see whose answer checks out.
SensorKind senProbe() {
  float t, h;
  Wire.beginTransmission(SEN_ADDR);              // anyone at 0x44? (an ACK = yes)
  if (Wire.endTransmission() != 0) return SEN_NONE;
  for (SensorKind k : {SEN_SHT4X, SEN_SHT3X}) {
    senCommand(k);
    delay(20);
    if (senRead(k, t, h)) return k;
  }
  return SEN_NONE;
}

// Every frame: one measurement every 2 s, without waiting around for it.
void sensorUpdate() {
  uint32_t now = millis();
  if (!senBusUp) {
    Wire.begin(26, 32, 100000);
    Wire.setTimeOut(20);
    senBusUp = true;
  }
  if (senKind == SEN_NONE) {
    if ((int32_t)(now - senNextAt) < 0) return;
    senNextAt = now + 10000;
    senKind = senProbe();
    if (senKind != SEN_NONE) Serial.printf("[SENSOR] Found %s on the Grove port\n", SENSOR_NAMES[senKind]);
    return;
  }
  if (senWaiting && (int32_t)(now - senReadAt) >= 0) {
    senWaiting = false;
    float t, h;
    if (senRead(senKind, t, h)) {
      senTemp = t + cfg.tempOffset / 10.0f;
      senHum = h;
      senLastOk = now;
    } else if (now - senLastOk > 10000) {                  // unplugged
      Serial.println("[SENSOR] Sensor gone");
      senKind = SEN_NONE;
      senTemp = senHum = NAN;
      senNextAt = now + 2000;
    }
  }
  if (!senWaiting && (int32_t)(now - senNextAt) >= 0) {
    senCommand(senKind);
    senWaiting = true;
    senReadAt = now + 20;                                   // the chip needs ~10-15 ms to measure
    senNextAt = now + 2000;
  }
}

bool sensorOk() { return senKind != SEN_NONE && !isnan(senTemp); }
float tempShown() { return cfg.tempF ? senTemp * 9 / 5 + 32 : senTemp; }

// ---------------------------------------------------------------------
//  CLIMATE app: a thermometer bar (left, coloured cold blue -> warm red)
//  and a humidity bar (right, cyan). Tap = read it out.
// ---------------------------------------------------------------------
Scroller climScroll;

void climateEnter() { climScroll.stop(); }

bool climateFrame(Event e) {
  if (e == EV_HOLD) return false;
  clearFb();
  if (!sensorOk()) {
    if (e == EV_TAP || !climScroll.active) climScroll.start("PLUG AN ENV UNIT INTO THE GROVE PORT", CRGB(255, 200, 0));
    if (!climScroll.draw()) climScroll.stop();
    if (!climScroll.active && (millis() / 500) % 2) drawSprite(SPR_PLUG);
    return true;
  }
  if (e == EV_TAP) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f%c HUMIDITY %d", tempShown(), cfg.tempF ? 'F' : 'C', (int)roundf(senHum));
    climScroll.start(buf, CRGB::White);
  }
  if (climScroll.draw()) return true;
  // Thermometer: 10 C = empty, 35 C = full (5 rows x 3 columns, 15 steps).
  float f = constrain((senTemp - 10) / 25.0f, 0.0f, 1.0f);
  uint8_t hue = 160 - (uint8_t)(f * 160);                   // blue 160 -> red 0
  int lit = (int)roundf(f * 15);
  for (int i = 0; i < 15; i++) {
    int x = i % 3, y = 4 - i / 3;
    px(x, y, i < lit ? CRGB(CHSV(hue, 255, 255)) : CRGB(10, 10, 16));
  }
  // Humidity: one column, 20% per LED.
  int hl = (int)roundf(senHum / 20);
  for (int y = 0; y < 5; y++) px(4, 4 - y, y < hl ? CRGB(0, 180, 255) : CRGB(0, 10, 20));
  return true;
}
