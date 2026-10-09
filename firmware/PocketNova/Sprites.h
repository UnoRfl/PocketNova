#pragma once
// =====================================================================
//  Sprites.h — every 5x5 picture on the device, drawn as text
// =====================================================================
//  Each sprite is 25 characters (5 rows of 5). Letters are colours:
//
//    R red   G green   B blue   C cyan   M magenta   Y yellow
//    O orange   P purple   W white   w dim white   g dim green
//    b dim blue   r dim red   .  off
//
//  The SPRITE() macro refuses to compile if you miscount a row — try
//  deleting a dot and see the error message.
// =====================================================================

#define SPRITE(name, pixels)                                   \
  const char name[] = pixels;                                  \
  static_assert(sizeof(name) == 26, #name " must be exactly 25 pixels");

#define FRAMES(arr) (sizeof(arr) / sizeof(arr[0]))

// ---------- general ----------
SPRITE(SPR_X,      "R...R" ".R.R." "..R.." ".R.R." "R...R")
SPRITE(SPR_CHECK,  "....." "....G" "...G." "G.G.." ".G...")
SPRITE(SPR_BT,     "..BB." "B.B.B" ".BBB." "B.B.B" "..BB.")
SPRITE(SPR_ARROW_R,"..G.." "...G." "GGGGG" "...G." "..G..")
SPRITE(SPR_ARROW_L,"..G.." ".G..." "GGGGG" ".G..." "..G..")
SPRITE(SPR_ARROW_U,"..Y.." ".YYY." "Y.Y.Y" "..Y.." "..Y..")
SPRITE(SPR_ARROW_RY,"..Y.." "...Y." "YYYYY" "...Y." "..Y..")

// ---------- menu icons (animated: each app has 2-4 frames) ----------
SPRITE(ICO_MEDIA_1, "..PP." "..P.P" "..P.." "PPP.." "PPP..")
SPRITE(ICO_MEDIA_2, "..M.M" "..M.." "MMM.." "MMM.." ".....")
const char* const ICON_MEDIA[] = {ICO_MEDIA_1, ICO_MEDIA_2};

SPRITE(ICO_TV_1, ".W.W." "..W.." "BBBBB" "BCCCB" "BBBBB")
SPRITE(ICO_TV_2, ".W.W." "..W.." "BBBBB" "BMMMB" "BBBBB")
SPRITE(ICO_TV_3, "W...W" ".W.W." "BBBBB" "BYYYB" "BBBBB")
const char* const ICON_TV[] = {ICO_TV_1, ICO_TV_2, ICO_TV_3};

SPRITE(ICO_SLIDES_1, "BBBBB" "BG..B" "BGG.B" "BG..B" "BBBBB")
SPRITE(ICO_SLIDES_2, "BBBBB" "B.G.B" "B.GGB" "B.G.B" "BBBBB")
const char* const ICON_SLIDES[] = {ICO_SLIDES_1, ICO_SLIDES_2};

SPRITE(ICO_KEYS_1, "....." "OWOWO" "....." "WOWOW" ".....")
SPRITE(ICO_KEYS_2, "....." "WOWOW" "....." "OWOWO" ".....")
const char* const ICON_KEYS[] = {ICO_KEYS_1, ICO_KEYS_2};

SPRITE(ICO_LEVEL_1, "....." "....." "wGwww" "....." ".....")
SPRITE(ICO_LEVEL_2, "....." "....." "wwGww" "....." ".....")
SPRITE(ICO_LEVEL_3, "....." "....." "wwwGw" "....." ".....")
const char* const ICON_LEVEL[] = {ICO_LEVEL_1, ICO_LEVEL_2, ICO_LEVEL_3, ICO_LEVEL_2};

SPRITE(ICO_DICE_1, "W...W" "....." "..W.." "....." "W...W")
SPRITE(ICO_DICE_2, "R...." "....." "..R.." "....." "....R")
SPRITE(ICO_DICE_3, "Y...Y" "....." "Y...Y" "....." "Y...Y")
const char* const ICON_DICE[] = {ICO_DICE_1, ICO_DICE_2, ICO_DICE_3};

SPRITE(ICO_TIMER_1, "YYYYY" ".YYY." "..Y.." ".w.w." "wwwww")
SPRITE(ICO_TIMER_2, "wwwww" ".YYY." "..Y.." ".wYw." "wwwww")
SPRITE(ICO_TIMER_3, "wwwww" ".w.w." "..Y.." ".YYY." "YYYYY")
const char* const ICON_TIMER[] = {ICO_TIMER_1, ICO_TIMER_2, ICO_TIMER_3};

SPRITE(ICO_LIGHT_1, "RYGCB" "YGCBM" "GCBMR" "CBMRY" "BMRYG")
SPRITE(ICO_LIGHT_2, "YGCBM" "GCBMR" "CBMRY" "BMRYG" "MRYGC")
SPRITE(ICO_LIGHT_3, "GCBMR" "CBMRY" "BMRYG" "MRYGC" "RYGCB")
const char* const ICON_LIGHT[] = {ICO_LIGHT_1, ICO_LIGHT_2, ICO_LIGHT_3};

SPRITE(ICO_TORCH_1, "Y.Y.Y" ".YYY." "..W.." "..w.." "..w..")
SPRITE(ICO_TORCH_2, "....." ".YYY." "..W.." "..w.." "..w..")
const char* const ICON_TORCH[] = {ICO_TORCH_1, ICO_TORCH_2};

SPRITE(ICO_SNAKE_1, "....." "gggG." "....." "...R." ".....")
SPRITE(ICO_SNAKE_2, "....." ".ggg." "...G." "...R." ".....")
SPRITE(ICO_SNAKE_3, "....." "..gg." "...g." "...G." ".....")
const char* const ICON_SNAKE[] = {ICO_SNAKE_1, ICO_SNAKE_2, ICO_SNAKE_3};

SPRITE(ICO_SET_1, "..w.." ".WWW." "wW.Ww" ".WWW." "..w..")
SPRITE(ICO_SET_2, "w...w" ".WWW." ".W.W." ".WWW." "w...w")
const char* const ICON_SET[] = {ICO_SET_1, ICO_SET_2};

// ---------- media remote ----------
SPRITE(SPR_PLAY,  ".G..." ".GG.." ".GGG." ".GG.." ".G...")
SPRITE(SPR_PREV,  "C...C" "C..CC" "C.CCC" "C..CC" "C...C")
SPRITE(SPR_NEXT,  "C...C" "CC..C" "CCC.C" "CC..C" "C...C")
SPRITE(SPR_VOLUP, "....." "..Y.." ".YYY." "..Y.." ".....")
SPRITE(SPR_VOLDN, "....." "....." ".YYY." "....." ".....")

// ---------- TV remote ----------
SPRITE(SPR_POWER, "..R.." "R.R.R" "R.R.R" "R...R" ".RRR.")
SPRITE(SPR_TVUP,  "....." "..G.." ".GGG." "..G.." ".....")
SPRITE(SPR_TVDN,  "....." "....." ".GGG." "....." ".....")
SPRITE(SPR_MUTE,  "O...O" ".O.O." "..O.." ".O.O." "O...O")
SPRITE(SPR_CHUP,  "..C.." ".CCC." "C.C.C" "..C.." "..C..")
SPRITE(SPR_CHDN,  "..C.." "..C.." "C.C.C" ".CCC." "..C..")
SPRITE(SPR_INPUT, "..Y.." "...Y." "YYYYY" "...Y." "..Y..")
SPRITE(SPR_FIND,  "WWW.." "W.W.." "WWW.." "...W." "....W")
SPRITE(SPR_ALL,   "R.Y.G" "....."  "C.B.M" "....."  "W.O.P")   // ALL BRANDS

// ---------- slides ----------
SPRITE(SPR_START, ".Y..." ".YY.." ".YYY." ".YY.." ".Y...")
SPRITE(SPR_STOP,  "RRRRR" "R...R" "R...R" "R...R" "RRRRR")

// ---------- PC shortcuts ----------
SPRITE(SPR_LOCK, ".YYY." ".Y.Y." "YYYYY" "YY.YY" "YYYYY")
SPRITE(SPR_SNIP, "C...C" ".C.C." "..C.." "CC.CC" "CC.CC")
SPRITE(SPR_DESK, "BBBBB" "BbbbB" "BBBBB" "..B.." ".BBB.")
SPRITE(SPR_TASK, "....G" "..G.G" "..G.G" "G.G.G" "G.G.G")
SPRITE(SPR_CALC, "OOOOO" "O.O.O" "OOOOO" "O.O.O" "OOOOO")

// ---------- dice faces ----------
SPRITE(DICE_1, "....." "....." "..W.." "....." ".....")
SPRITE(DICE_2, "W...." "....." "....." "....." "....W")
SPRITE(DICE_3, "W...." "....." "..W.." "....." "....W")
SPRITE(DICE_4, "W...W" "....." "....." "....." "W...W")
SPRITE(DICE_5, "W...W" "....." "..W.." "....." "W...W")
SPRITE(DICE_6, "W...W" "....." "W...W" "....." "W...W")
const char* const DICE_FACES[] = {DICE_1, DICE_2, DICE_3, DICE_4, DICE_5, DICE_6};

// ---------- settings ----------
SPRITE(SPR_SUN,   "Y.Y.Y" ".YYY." "YYYYY" ".YYY." "Y.Y.Y")
SPRITE(SPR_ROT,   "..C.." ".CCC." "C.C.C" "..C.." "..C..")
SPRITE(SPR_CALIB, ".GGG." "G...G" "G.G.G" "G...G" ".GGG.")
SPRITE(SPR_INFO,  "..B.." "....." "..B.." "..B.." "..B..")
SPRITE(SPR_AUTOROT, ".CCC." "C...C" "C...C" "C..CC" ".C.CC")
SPRITE(SPR_WIFI,  ".CCC." "C...C" ".CCC." "....." "..C..")
SPRITE(SPR_PAIR,  "..B.O" "B.BOO" ".BB.O" "B.B.." "..B..")
SPRITE(SPR_TUTOR, ".YYY." "Y...Y" "...Y." "..Y.." "..Y..")

// ---------- tutorial ----------
SPRITE(SPR_TAP_DOT,  "....." "....." "..W.." "....." ".....")
SPRITE(SPR_HOLD_RING, ".WWW." "W...W" "W...W" "W...W" ".WWW.")
