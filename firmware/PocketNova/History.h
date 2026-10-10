#pragma once
// =====================================================================
//  History.h - a log of who joined, who left and what was typed where
// =====================================================================
//  Security people call this an AUDIT LOG: you can't stop every bad
//  guess, but you can see them, spot a pattern (one device joining 30
//  times a minute, 50 wrong codes in a row) and block whoever's doing it.
//
//  Pocket Nova keeps the last 48 events in memory. The PC panel copies
//  them into a file (history.json next to the panel), so the full history
//  survives restarts. Each event has a number (seq) that only goes up, so
//  the panel just asks "anything after #17?" and never gets one twice.
//
//  PASSWORDS: off unless you switch "log typed passwords" on in the panel.
//  Then the password typed with each Connect is kept too, so you can spot
//  guessing (123, 1234, password...). A try that WORKS has its password
//  wiped right away, so a real Wi-Fi password never stays in the log.
// =====================================================================

enum HistKind : uint8_t {
  H_SETUP_OPEN, H_SETUP_CLOSE,   // the setup network opened / closed
  H_JOIN, H_LEAVE,               // a device joined / left the setup network
  H_TRY,                         // someone pressed Connect on the setup page
  H_JOINED, H_FAILED,            // ...and Pocket Nova joined that Wi-Fi / couldn't
  H_LIMITED,                     // too many tries: told to wait
  H_BLOCKED,                     // a blocked device tried to join and was kicked
  H_FLOOD,                       // joined too often too fast: blocked for 10 minutes
  H_KICK,                        // kicked from the panel
  H_CODE_BAD, H_CODE_OK,         // phone remote: wrong / right pairing code
  H_RESTART,                     // Pocket Nova restarted after a freeze or crash
};
const char* const HIST_NAMES[] = {"setup_open", "setup_close", "join", "leave", "try", "joined", "failed",
                                  "limited", "blocked", "flood", "kick", "code_bad", "code_ok", "restart"};

struct HistEntry {
  uint32_t seq;
  uint32_t at;          // millis() when it happened
  uint8_t  kind;
  uint8_t  mac[6];      // all zero = unknown
  uint32_t ip;          // lwIP order, 0 = unknown
  char     note[34];    // network name tried, a reason...
  char     pass[64];    // password typed with a Connect (only when logging them is on)
};

const int  HIST_LEN = 48;
HistEntry  hist[HIST_LEN];
uint32_t   histSeq = 0;                 // number of the newest event
uint32_t   histBoot = 0;                // random per start, so the PC can tell restarts apart
portMUX_TYPE histMux = portMUX_INITIALIZER_UNLOCKED;

// Safe to call from the Wi-Fi task (its events) as well as the main loop.
void histAdd(HistKind kind, const uint8_t* mac = nullptr, uint32_t ip = 0, const char* note = "", const char* pass = nullptr) {
  portENTER_CRITICAL(&histMux);
  HistEntry& e = hist[histSeq % HIST_LEN];
  e.seq = ++histSeq;
  e.at = millis();
  e.kind = kind;
  if (mac) memcpy(e.mac, mac, 6); else memset(e.mac, 0, 6);
  e.ip = ip;
  strncpy(e.note, note ? note : "", sizeof(e.note) - 1);
  e.note[sizeof(e.note) - 1] = 0;
  strncpy(e.pass, pass ? pass : "", sizeof(e.pass) - 1);
  e.pass[sizeof(e.pass) - 1] = 0;
  portEXIT_CRITICAL(&histMux);
}

// Wipes the password kept with event #seq (if it's still in memory).
void histForgetPass(uint32_t seq) {
  portENTER_CRITICAL(&histMux);
  HistEntry& e = hist[(seq - 1) % HIST_LEN];
  if (seq && e.seq == seq) memset(e.pass, 0, sizeof(e.pass));
  portEXIT_CRITICAL(&histMux);
}
