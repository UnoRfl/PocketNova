#pragma once
// =====================================================================
//  Ir.h - the universal TV remote engine
// =====================================================================
//  TVs don't share one "language". Each brand uses its own protocol
//  (NEC, Samsung, Sony SIRC, Panasonic, Philips RC5/RC6, JVC, Sharp...)
//  and its own codes. The code TABLE lives in TvCodes.h, which is
//  GENERATED from firmware/tools/tv_codes.json by gen_tvcodes.py - every
//  code there lists the sources that confirm it. Edit the JSON, not the .h.
//
//  Extra tricks for reliability:
//   - each button can have an ALT code (some model years differ); both
//     are sent, so either TV family responds
//   - "ALL BRANDS" mode sends a button for every brand in turn
//   - POWER_SWEEP is a long list of power codes used by FIND TV
// =====================================================================

const uint8_t IR_LED_PIN = 12;
IRsend irsend(IR_LED_PIN);

enum IrProto : uint8_t { IR_NONE, IR_NEC, IR_SAMSUNG, IR_SONY, IR_PANASONIC, IR_RC5, IR_RC6, IR_JVC, IR_SHARP, IR_MITSUBISHI };
const char* const PROTO_NAMES[] = {"-", "NEC", "SAMSUNG", "SONY", "PANASONIC", "RC5", "RC6", "JVC", "SHARP", "MITSUBISHI"};

enum TvCmd : uint8_t { CMD_POWER, CMD_VOLUP, CMD_VOLDN, CMD_MUTE, CMD_CHUP, CMD_CHDN, CMD_INPUT, CMD_COUNT };
const char* const CMD_NAMES[CMD_COUNT] = {"POWER", "VOL+", "VOL-", "MUTE", "CH+", "CH-", "INPUT"};

struct IrCode {
  IrProto  proto;    // IR_NONE = this brand has no such button
  uint8_t  bits;
  uint16_t addr;     // Panasonic / RC5 / RC6 address
  uint64_t data;     // the code (RC5/RC6: the command number; Sharp: the raw 15-bit frame)
};

struct TvBrand {
  const char* name;
  char        letter;           // shown on the matrix
  CRGB        color;
  IrCode      codes[CMD_COUNT];
  IrCode      alts[CMD_COUNT];  // second code for model families that differ
  uint8_t     verified;         // how many buttons are confirmed by 2+ sources
};

struct SweepEntry {
  IrCode  code;
  int8_t  brand;                // index into BRANDS, or -1 for power-only families
  const char* label;
};

#include "TvCodes.h"   // BRANDS[], BRAND_COUNT, POWER_SWEEP[], SWEEP_COUNT

// cfg.tvBrand == BRAND_ALL means "no TV picked yet" (only universal POWER works).
const uint8_t BRAND_ALL = BRAND_COUNT;
const char* tvBrandName(uint8_t b) { return b >= BRAND_COUNT ? "NO TV PICKED" : BRANDS[b].name; }

// Send one code. Frame counts follow what real remotes send (from the IR
// research): Sony 3+, Samsung/JVC/Sharp/Mitsubishi 2. RC5/RC6 flip a
// "toggle" bit on every new press so the TV can tell it from a held button.
void irSendCode(const IrCode& c, uint16_t repeat = 0) {
  static bool toggle = false;
  switch (c.proto) {
    case IR_NEC:       irsend.sendNEC(c.data, c.bits ? c.bits : 32, repeat); break;
    case IR_SAMSUNG:   irsend.sendSAMSUNG(c.data, c.bits ? c.bits : 32, max<uint16_t>(1, repeat)); break;
    case IR_SONY:      irsend.sendSony(c.data, c.bits ? c.bits : 12, max<uint16_t>(2, repeat)); break;   // Sony: 3+ frames
    case IR_PANASONIC: irsend.sendPanasonic(c.addr, (uint32_t)c.data, kPanasonicBits, repeat); break;
    case IR_JVC:       irsend.sendJVC(c.data, c.bits ? c.bits : 16, max<uint16_t>(1, repeat)); break;
    case IR_SHARP:     irsend.sendSharpRaw(c.data, kSharpBits, max<uint16_t>(1, repeat)); break;
    case IR_MITSUBISHI: irsend.sendMitsubishi(c.data, kMitsubishiBits, max<uint16_t>(1, repeat)); break;
    case IR_RC5: {
      uint64_t d = irsend.encodeRC5(c.addr, c.data);
      if (toggle) d = irsend.toggleRC5(d);
      irsend.sendRC5(d, kRC5Bits, repeat);
      toggle = !toggle;
      break;
    }
    case IR_RC6: {
      uint64_t d = irsend.encodeRC6(c.addr, c.data, kRC6Mode0Bits);
      if (toggle) d = irsend.toggleRC6(d, kRC6Mode0Bits);
      irsend.sendRC6(d, kRC6Mode0Bits, repeat);
      toggle = !toggle;
      break;
    }
    default: break;
  }
}

// Sends a button for one brand (main code + its alternate). False = no such button.
bool irSendBrandCmd(uint8_t brand, uint8_t cmd, uint16_t repeat = 0) {
  const TvBrand& b = BRANDS[brand];
  const IrCode& c = b.codes[cmd];
  if (c.proto == IR_NONE) return false;
  irSendCode(c, repeat);
  if (b.alts[cmd].proto != IR_NONE) {
    delay(50);                       // small gap between the two codes
    irSendCode(b.alts[cmd], repeat);
  }
  return true;
}

// The main entry point: a button for the chosen brand, or every brand.
bool irSendCmd(uint8_t brand, uint8_t cmd, uint16_t repeat = 0) {
  if (brand >= BRAND_COUNT) {        // ALL BRANDS
    int sent = 0;
    for (uint8_t b = 0; b < BRAND_COUNT; b++) {
      if (irSendBrandCmd(b, cmd, repeat)) { sent++; delay(40); }
    }
    Serial.printf("[IR] ALL BRANDS %s (%d codes)\n", CMD_NAMES[cmd], sent);
    return sent > 0;
  }
  if (!irSendBrandCmd(brand, cmd, repeat)) {
    Serial.printf("[IR] %s has no %s button\n", BRANDS[brand].name, CMD_NAMES[cmd]);
    return false;
  }
  const IrCode& c = BRANDS[brand].codes[cmd];
  Serial.printf("[IR] %s %s (%s 0x%llX%s)\n", BRANDS[brand].name, CMD_NAMES[cmd],
                PROTO_NAMES[c.proto], (unsigned long long)c.data,
                BRANDS[brand].alts[cmd].proto != IR_NONE ? " + alt" : "");
  return true;
}
