/* =============================================================
   Keychain — a tiny screen that plays your own animations
   -------------------------------------------------------------
   Same board and wiring as Claude Buddy:
   Board   : ESP32-C3 Super Mini
   Display : 1.3" OLED 128x64, SH1106, I2C @ 0x3C
   Touch   : TTP223 capacitive module (active HIGH)

   Wiring  : OLED  VCC -> 3V3     OLED  GND -> GND
             OLED  SDA -> GPIO5   OLED  SCL -> GPIO4
             TTP223 VCC -> 3V3    TTP223 GND -> GND
             TTP223 OUT -> GPIO3
             BUZZER  +  -> GPIO6  BUZZER  -  -> GND   (passive buzzer, optional)

   Libraries: Adafruit GFX + Adafruit SH110X
   Arduino IDE: board "ESP32C3 Dev Module", USB CDC On Boot: Enabled

   YOUR PICTURES
     python tools/make_frames.py your.gif [more.gif ...]   -> frames.h
     then upload this sketch. Each file becomes one animation.
     The built-in "night ride" plays after your own ones.

   TOUCH
     tap          -> next animation
     double tap   -> SETTINGS (brightness, volume, sound) - tap = next / +, hold = choose / done
     hold 3 s     -> sleep now (tap to wake)
     Nobody touches it for SLEEP_MIN minutes -> it sleeps by itself
     (screen off, almost no battery). A tap wakes it up.
   ============================================================= */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Update.h>
#include "frames_z.h"                // made from frames.h by tools/pack_frames.py

/* ---------------- make it yours ---------------- */
#define FW_VERSION   "2.1.1"   // bump it for each release: the app offers the update (tools/release_fw.py)
#define CAPTION      ""      // a little name in the corner, e.g. "NNAN" ("" = none)
#define SLEEP_MIN    5       // sleep after this many minutes untouched (0 = never)
#define FLIP         0       // 1 = screen mounted upside down
#define BUILTIN_RIDE 0       // 0 = only your own animations
#define BUILTIN_PLAYER 1     // the record player (like the Claude face's), after your animations
#define BUILTIN_PICTURE 1    // "My picture": what you draw or pick in the app, after the record player

/* ---------------- hardware ---------------- */
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define I2C_SDA       5
#define I2C_SCL       4
#define TOUCH_PIN     3
#define BUZZER_PIN    6
#define C_WHITE SH110X_WHITE
#define C_BLACK SH110X_BLACK

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences prefs;

const uint8_t BRI[] = { 1, 10, 25, 50, 90, 150, 207, 255 };      // the same steps as the Claude face
const uint8_t BRI_COUNT = sizeof(BRI);
uint8_t briIndex = 5;
const uint16_t VOL_LEVELS[] = { 30, 60, 110, 180, 270, 380, 512 }; // buzzer loudness (PWM duty of 1023)
const uint8_t  VOL_COUNT = sizeof(VOL_LEVELS) / sizeof(VOL_LEVELS[0]);
uint8_t volIndex = 3;
bool    soundOn  = true;
uint8_t screen   = 0;
const uint8_t SCREENS = (KC_ANIM_COUNT + BUILTIN_RIDE + BUILTIN_PLAYER + BUILTIN_PICTURE)
                        ? KC_ANIM_COUNT + BUILTIN_RIDE + BUILTIN_PLAYER + BUILTIN_PICTURE : 1;
const uint8_t PLAYER_SCREEN = KC_ANIM_COUNT + BUILTIN_RIDE;                     // (only if BUILTIN_PLAYER)
const uint8_t PICTURE_SCREEN = KC_ANIM_COUNT + BUILTIN_RIDE + BUILTIN_PLAYER;   // (only if BUILTIN_PICTURE)

/* MY PICTURE - drawn or picked in the app, shown the moment it arrives.
   The app sends only the bytes that changed: [offset hi, offset lo, bytes...]
   to the picture characteristic; "P:save" keeps it (it survives sleep). */
uint8_t  picBuf[1024];
bool     picSet = false;
volatile bool picNew = false;

uint32_t lastTouch = 0;
uint32_t toastUntil = 0;
char     toastText[24] = "";
int8_t   toastBar = -1;            // >= 0: draw a brightness bar in the toast

/* ============================================================
   SOUND — tiny chirps, never blocking
   ============================================================ */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define BZ_ID BUZZER_PIN
#else
  #define BZ_ID 0
#endif
struct Note { uint16_t f, ms; };
Note     notes[6];
uint8_t  noteLen = 0, noteAt = 0;
uint32_t noteStart = 0;

void buzzInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 10);
#else
  ledcSetup(BZ_ID, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BZ_ID);
#endif
  ledcWrite(BZ_ID, 0);
}
void buzz(uint16_t f) {
  if (!f) { ledcWrite(BZ_ID, 0); return; }
  ledcChangeFrequency(BZ_ID, f, 10);
  ledcWrite(BZ_ID, VOL_LEVELS[volIndex]);
}
void play(std::initializer_list<Note> list, bool force = false) {
  if (!soundOn && !force) return;
  noteLen = 0;
  for (const Note& n : list) if (noteLen < 6) notes[noteLen++] = n;
  noteAt = 0; noteStart = millis();
  buzz(notes[0].f);
}
void soundUpdate() {
  if (noteAt >= noteLen) return;
  if (millis() - noteStart < notes[noteAt].ms) return;
  noteAt++; noteStart = millis();
  buzz(noteAt < noteLen ? notes[noteAt].f : 0);
}
void sndTap()   { play({ { 2600, 18 } }); }
void sndHello() { play({ { 1900, 60 }, { 0, 20 }, { 2500, 60 }, { 0, 20 }, { 3200, 90 } }); }
void sndBye()   { play({ { 3000, 70 }, { 0, 20 }, { 2300, 70 }, { 0, 20 }, { 1700, 110 } }); }

/* ============================================================
   BACKGROUND MUSIC — little original chiptunes, one per mood.
   Quieter than the chirps; a chirp (tap, hello) briefly takes over
   and the tune carries on after. Double tap mutes everything.
   ============================================================ */
#define MUSIC     0                      // 1 = background tunes on, 0 = off (just the chirps)
#define MUSIC_VOL 70                     // loudness of the tunes (0..1023; chirps use 200)
#define G4 392
#define A4 440
#define B4 494
#define C5 523
#define D5 587
#define E5 659
#define F5 698
#define G5 784
#define A5 880
#define B5 988
#define C6 1047
#define E6 1319
struct Tone { uint16_t f, ms; };         // f = 0: a rest

// 1. lullaby: slow and soft (night scenes)
const Tone T_LULLABY[] = {
  {E5,420},{G5,210},{E5,210},{D5,420},{C5,420},{D5,210},{E5,210},{G5,630},{0,210},
  {A5,420},{G5,210},{E5,210},{D5,420},{E5,420},{D5,210},{C5,210},{C5,630},{0,630},
  {E5,420},{G5,210},{A5,210},{C6,420},{A5,420},{G5,210},{E5,210},{G5,630},{0,210},
  {A5,420},{G5,210},{E5,210},{D5,420},{C5,420},{D5,210},{E5,210},{C5,840},{0,840} };
// 2. bouncy: cheerful (sunny, cosy scenes)
const Tone T_BOUNCY[] = {
  {C5,170},{E5,170},{G5,170},{E5,170},{A5,340},{G5,340},{E5,170},{G5,170},{C6,340},{A5,170},{G5,170},{E5,340},{0,170},
  {D5,170},{E5,170},{G5,170},{E5,170},{D5,340},{C5,340},{E5,170},{D5,170},{C5,510},{0,340},
  {G5,170},{A5,170},{G5,170},{E5,170},{G5,340},{C6,340},{A5,170},{G5,170},{E5,340},{D5,170},{E5,170},{G5,340},{0,170},
  {E5,170},{D5,170},{C5,170},{D5,170},{E5,340},{G5,340},{D5,170},{E5,170},{C5,510},{0,340} };
// 3. road trip: upbeat, a bass note under each step
const Tone T_ROAD[] = {
  {G4,140},{D5,140},{G5,140},{D5,140},{A4,140},{E5,140},{A5,140},{E5,140},
  {C5,140},{G5,140},{C6,140},{G5,140},{B4,140},{D5,140},{G5,280},{0,140},
  {G4,140},{D5,140},{G5,140},{A5,140},{C6,280},{A5,140},{G5,140},
  {E5,140},{G5,140},{E5,140},{D5,140},{C5,280},{D5,140},{G4,280},{0,140} };
// 4. space: a slow, dreamy arpeggio
const Tone T_SPACE[] = {
  {C5,320},{G5,320},{E6,320},{G5,320},{A4,320},{E5,320},{C6,320},{E5,320},
  {F5,320},{C6,320},{A5,320},{C6,320},{G4,320},{D5,320},{B5,320},{D5,320},{0,640} };

struct Tune { const Tone* t; uint8_t n; };
const Tune TUNES[] = {
  { T_LULLABY, sizeof(T_LULLABY) / sizeof(Tone) }, { T_BOUNCY, sizeof(T_BOUNCY) / sizeof(Tone) },
  { T_ROAD,    sizeof(T_ROAD)    / sizeof(Tone) }, { T_SPACE,  sizeof(T_SPACE)  / sizeof(Tone) } };

uint8_t  tuneIdx = 0, tuneAt = 0;
uint32_t toneStart = 0;
bool     toneOn = false;

uint8_t tuneFor(uint8_t scr) {           // pick the mood from the screen's name
  if (scr >= KC_ANIM_COUNT) return 0;
  const char* n = KC_ANIMS[scr].name;
  if (strstr(n, "Black") || strstr(n, "Saturn")) return 3;
  if (strstr(n, "drive") || strstr(n, "ace"))    return 2;
  if (strstr(n, "bus") || strstr(n, "roof") || strstr(n, "bed") || strstr(n, "snow")) return 0;
  return 1;
}

void musicStart(uint8_t scr) {
  tuneIdx = tuneFor(scr); tuneAt = 0; toneStart = millis(); toneOn = false;
}

void musicUpdate() {
  if (!MUSIC) return;
  if (noteAt < noteLen) { toneOn = false; return; }   // a chirp is playing: let it
  if (!soundOn) { if (toneOn) { ledcWrite(BZ_ID, 0); toneOn = false; } return; }
  const Tune& tu = TUNES[tuneIdx];
  const Tone& t = tu.t[tuneAt];
  uint32_t el = millis() - toneStart;
  if (el >= t.ms) {                                    // next note
    tuneAt = (tuneAt + 1) % tu.n;
    toneStart = millis(); toneOn = false; el = 0;
  }
  const Tone& cur = tu.t[tuneAt];
  bool want = cur.f && el < cur.ms * 82 / 100;         // a short gap after each note, so they sound separate
  if (want && !toneOn) {
    ledcChangeFrequency(BZ_ID, cur.f, 10);
    ledcWrite(BZ_ID, MUSIC_VOL);
    toneOn = true;
  } else if (!want && toneOn) {
    ledcWrite(BZ_ID, 0);
    toneOn = false;
  }
}

/* ============================================================
   HELPERS
   ============================================================ */
// black out everything outside a circle: an iris opening / closing
void iris(int cx, int cy, int r) {
  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    int dy = y - cy, dx = (r > abs(dy)) ? (int)sqrtf((float)(r * r - dy * dy)) : -1;
    if (dx < 0) { display.drawFastHLine(0, y, SCREEN_WIDTH, C_BLACK); continue; }
    if (cx - dx > 0) display.drawFastHLine(0, y, cx - dx, C_BLACK);
    if (cx + dx < SCREEN_WIDTH) display.drawFastHLine(cx + dx, y, SCREEN_WIDTH - cx - dx, C_BLACK);
  }
}

void toast(const char* t, int8_t bar = -1, uint16_t ms = 1200) {
  strncpy(toastText, t, sizeof(toastText) - 1);
  toastBar = bar; toastUntil = millis() + ms;
}

void drawToast() {
  if (millis() > toastUntil) return;
  int w = strlen(toastText) * 6 + 16;
  if (toastBar >= 0 && w < 70) w = 70;
  int h = toastBar >= 0 ? 24 : 15, x = (SCREEN_WIDTH - w) / 2, y = SCREEN_HEIGHT - h - 3;
  display.fillRoundRect(x, y, w, h, 4, C_BLACK);
  display.drawRoundRect(x, y, w, h, 4, C_WHITE);
  display.setTextSize(1); display.setTextColor(C_WHITE);
  display.setCursor(x + (w - (int)strlen(toastText) * 6) / 2, y + 4);
  display.print(toastText);
  if (toastBar >= 0) {
    for (uint8_t i = 0; i < BRI_COUNT; i++) {
      int bx = x + 10 + i * ((w - 20) / BRI_COUNT);
      if (i <= toastBar) display.fillRect(bx, y + 15, (w - 20) / BRI_COUNT - 3, 5, C_WHITE);
      else display.drawRect(bx, y + 15, (w - 20) / BRI_COUNT - 3, 5, C_WHITE);
    }
  }
}

void drawCaption() {
  if (!CAPTION[0]) return;
  int w = strlen(CAPTION) * 6 + 4;
  display.fillRect(0, SCREEN_HEIGHT - 10, w, 10, C_BLACK);
  display.setTextSize(1); display.setTextColor(C_WHITE);
  display.setCursor(2, SCREEN_HEIGHT - 8);
  display.print(CAPTION);
}

/* ============================================================
   YOUR ANIMATIONS (frames.h)
   ============================================================ */
uint16_t frameIdx = 0;
uint32_t frameAt = 0;

/* frames_z.h keeps only what changes from one frame to the next (packed):
   the picture on screen lives here, and each step applies the next changes */
uint8_t  kcBuf[1024];
int16_t  bufAnim = -1, bufFrame = -1;

void applyFrame(uint16_t g) {                  // XOR frame g's packed changes into kcBuf
  uint32_t i = pgm_read_dword(&KC_ZOFF[g]);
  uint16_t o = 0;
  while (o < 1024) {
    uint8_t c = pgm_read_byte(&KC_Z[i++]);
    if (c < 128) { for (uint16_t k = 0; k <= c && o < 1024; k++) kcBuf[o++] ^= pgm_read_byte(&KC_Z[i++]); }
    else { uint8_t v = pgm_read_byte(&KC_Z[i++]); for (uint16_t k = 0; k < (uint16_t)(c - 126) && o < 1024; k++) kcBuf[o++] ^= v; }
  }
}

const uint8_t* frameBits(uint8_t a, uint16_t f) {
  const KcAnim& an = KC_ANIMS[a];
  if (bufAnim != a || (int16_t)f < bufFrame) { memset(kcBuf, 0, sizeof(kcBuf)); bufAnim = a; bufFrame = -1; }   // start over from frame 0
  while (bufFrame < (int16_t)f) applyFrame(an.first + ++bufFrame);
  return kcBuf;
}

void drawCustom(uint8_t a) {
  const KcAnim& an = KC_ANIMS[a];
  if (an.count == 0) return;
  uint32_t now = millis();
  uint16_t ms = pgm_read_word(&KC_DELAY[an.first + frameIdx % an.count]);
  if (now - frameAt >= ms) { frameIdx = (frameIdx + 1) % an.count; frameAt = now; }
  display.drawBitmap(0, 0, frameBits(a, frameIdx % an.count), SCREEN_WIDTH, SCREEN_HEIGHT, C_WHITE);
}

/* ============================================================
   BUILT-IN: NIGHT RIDE — the view from a bus window at night
   ============================================================ */
// the window glass: everything outside is drawn only inside it
const int WX0 = 8, WY0 = 5, WX1 = 119, WY1 = 41;

inline void px(int x, int y) {
  if (x > WX0 && x < WX1 && y > WY0 && y < WY1) display.drawPixel(x, y, C_WHITE);
}
void boxOut(int x, int y, int w, int h, bool fill) {        // a rectangle, clipped to the glass
  int x0 = max(x, WX0 + 1), x1 = min(x + w - 1, WX1 - 1);
  int y0 = max(y, WY0 + 1), y1 = min(y + h - 1, WY1 - 1);
  if (x0 > x1 || y0 > y1) return;
  if (fill) { display.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, C_WHITE); return; }
  if (y == y0) display.drawFastHLine(x0, y0, x1 - x0 + 1, C_WHITE);
  if (x == x0) display.drawFastVLine(x0, y0, y1 - y0 + 1, C_WHITE);
  if (x + w - 1 == x1) display.drawFastVLine(x1, y0, y1 - y0 + 1, C_WHITE);
}

uint8_t rnd(uint32_t n) {                                     // steady "random" per index
  n = (n << 13) ^ n; n = n * (n * n * 15731 + 789221) + 1376312589;
  return (n >> 16) & 0xFF;
}

void drawRide(uint32_t f) {
  float sway = sinf(f * 0.09f) * 0.6f + (rnd(f / 9) > 236 ? 1 : 0);  // the bus rocks, bumps now and then
  int sy = (int)roundf(sway);

  // stars, twinkling
  for (int i = 0; i < 26; i++) {
    int x = WX0 + 2 + rnd(i * 3) % (WX1 - WX0 - 4), y = WY0 + 2 + rnd(i * 3 + 1) % 16;
    if ((f / 6 + i * 7) % 23) px(x, y + sy);
  }
  // the moon: far away, so it stays put
  int mx = 96, my = 14 + sy;
  for (int dy = -6; dy <= 6; dy++) for (int dx = -6; dx <= 6; dx++) {
    bool in = dx * dx + dy * dy <= 36, bite = (dx - 3) * (dx - 3) + (dy + 2) * (dy + 2) <= 26;
    if (in && !bite) px(mx + dx, my + dy);
  }
  // far city: slow, with lit windows that come and go
  float far = f * 0.35f;
  for (int i = (int)(far / 14); i < (int)(far / 14) + 10; i++) {
    int x = i * 14 - (int)far + WX0, h = 8 + rnd(i) % 12, w = 9 + rnd(i + 99) % 5;
    int top = WY1 - 2 - h + sy;
    boxOut(x, top, w, h + 3, false);
    for (int r = top + 3; r < WY1 - 3 + sy; r += 3)
      for (int c = x + 2; c < x + w - 2; c += 3)
        if (rnd(i * 31 + r * 7 + c + (f / 90)) > 170) px(c, r);
  }
  // street lamps rushing past
  float near = f * 3.2f;
  for (int i = (int)(near / 64); i < (int)(near / 64) + 3; i++) {
    int x = i * 64 - (int)near + WX0 + 20;
    for (int y = WY0 + 9; y < WY1; y++) px(x, y + sy);             // pole
    for (int k = 0; k < 6; k++) px(x - k, WY0 + 9 + sy);           // arm
    boxOut(x - 8, WY0 + 9 + sy, 4, 2, true);                       // lamp
    px(x - 7, WY0 + 12 + sy); px(x - 6, WY0 + 13 + sy);            // a little glow under it
  }
  // the road edge, rushing by
  int road = WY1 - 2 + sy;
  for (int x = WX0 + 1; x < WX1; x++)
    if (((x + (int)(f * 4.0f)) % 12) < 7) px(x, road);
  // the bus itself: window frame, the middle pillar, a grab pole, seat backs
  display.drawRoundRect(WX0, WY0, WX1 - WX0 + 1, WY1 - WY0 + 1, 3, C_WHITE);
  display.drawRoundRect(WX0 - 2, WY0 - 2, WX1 - WX0 + 5, WY1 - WY0 + 5, 4, C_WHITE);
  display.fillRect(63, WY0, 3, WY1 - WY0, C_WHITE);
  display.drawFastVLine(30, 0, SCREEN_HEIGHT, C_WHITE);
  display.drawFastVLine(31, 0, SCREEN_HEIGHT, C_WHITE);
  for (int s = 0; s < 2; s++) {
    int x = s ? 78 : 6, w = s ? 44 : 40, y = 47 + sy;
    display.fillRoundRect(x, y, w, 20, 5, C_BLACK);
    display.drawRoundRect(x, y, w, 20, 5, C_WHITE);
    display.drawRoundRect(x + 3, y + 3, w - 6, 20, 3, C_WHITE);
    for (int k = x + 6; k < x + w - 6; k += 2) display.drawPixel(k, y + 9, C_WHITE);   // stitching
  }
  // reflections sliding over the glass
  int g = (f / 2) % 160;
  for (int k = 0; k < 4; k++) px(WX0 + g - k + 4, WY0 + 6 + k * 2);
}

/* ============================================================
   SCREENS, TOUCH, SLEEP
   ============================================================ */
uint32_t sceneFrame = 0;


/* ============================================================
   RECORD PLAYER — the same "now playing" as the Claude face: a vinyl
   record whose grooves turn, a tonearm that lowers while it plays, the
   song gliding along, a progress line and three buttons.
   It speaks the Claude bridge's lines over USB, ready for later:
     M:<0 none|1 playing|2 paused>,<Song - Artist>
     Q:<position s>,<length s>
   Until a bridge talks to it, it plays a little demo.
   ============================================================ */
char     musicTitle[44] = "Lo-fi Nights - Snoopy";
bool     musicOn = true;                  // playing (the record turns)
bool     bridgeMusic = false;             // a bridge has sent something
uint8_t  musicState  = 0;                 // from the app: 0 nothing, 1 playing, 2 paused
uint32_t musicAt     = 0;                 // when the app last told us
uint32_t manualAt    = 0;                 // you changed screen yourself: the music backs off for a while
bool     newSong     = false;             // a different song started since then
bool     autoPlayer  = false;             // the music brought the record player up
uint8_t  beforeMusic = 0;                 // ...and this is where we were
uint32_t trackPos = 0, trackDur = 180, trackAt = 0;
float    discAngle = 0;

/* the beat, like the Claude face: the phone sends A:<0-99> (how loud the song
   is right now) ~8x a second. No A: lines (the phone doesn't share its sound)?
   A gentle beat of its own, so it still moves with the music. */
uint8_t  musicLevel  = 0;                 // loudness right now (0-99)
float    musicAvg    = 0;                 // ...on average (a jump above it = a beat)
float    musicSmooth = 0;                 // eased, for smooth movement
float    beatGlow    = 0;                 // eased beat pulse
uint32_t levelAt = 0, beatAt = 0;
bool     eqView = false, viewFlip = false;   // the player screen: record <-> equalizer, every 12 s
uint32_t viewAt = 0;

float beatPulse() {                       // 1 right on a beat, fading to 0 over 250 ms
  uint32_t d = millis() - beatAt;
  return d < 250 ? 1 - d / 250.0f : 0;
}

void beatUpdate() {                       // once a frame
  uint32_t now = millis();
  bool real = now - levelAt < 1500;
  float target = 0;
  if (real) target = musicLevel;
  else if (musicOn) {                     // no sound from the phone: about 115 beats a minute
    static uint32_t nextBeat = 0;
    if ((int32_t)(now - nextBeat) >= 0) { beatAt = now; nextBeat = now + 520; }
    target = 34 + 26 * beatPulse() + 8 * sinf(now * 0.004f);
  }
  musicSmooth += (target - musicSmooth) * 0.2f;
  beatGlow    += (beatPulse() - beatGlow) * 0.15f;
}

void txt(int x, int y, const char* t) {
  display.setTextSize(1); display.setTextColor(C_WHITE);
  display.setCursor(x, y); display.print(t);
}

void btnIcon(uint8_t b, int cx, int cy, uint16_t c) {
  if (b == 1) {                                   // |<<
    display.drawFastVLine(cx - 5, cy - 3, 7, c);
    display.fillTriangle(cx, cy - 3, cx, cy + 3, cx - 4, cy, c);
    display.fillTriangle(cx + 4, cy - 3, cx + 4, cy + 3, cx, cy, c);
  } else if (b == 3) {                            // >>|
    display.fillTriangle(cx - 4, cy - 3, cx - 4, cy + 3, cx, cy, c);
    display.fillTriangle(cx, cy - 3, cx, cy + 3, cx + 4, cy, c);
    display.drawFastVLine(cx + 5, cy - 3, 7, c);
  } else if (musicOn) {                           // pause ||
    display.fillRect(cx - 3, cy - 3, 2, 7, c);
    display.fillRect(cx + 1, cy - 3, 2, 7, c);
  } else {                                        // play >
    display.fillTriangle(cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy, c);
  }
}

void grooveRing(int cx, int cy, int r, float ang) {   // dashes travelling round: reads as spinning
  int n = (int)(2 * PI * r);
  int shift = (int)(ang * r);
  for (int k = 0; k < n; k++) {
    if (((k + shift) / 3) % 3 == 0) continue;
    float a = k * 2 * PI / n;
    display.drawPixel(cx + (int)lroundf(cosf(a) * r), cy + (int)lroundf(sinf(a) * r), C_WHITE);
  }
}

void drawPlayer(uint32_t f) {
  const int RX = 64;
  if (musicOn) discAngle += 0.07f;
  char song[44], artist[44] = "";
  strncpy(song, musicTitle[0] ? musicTitle : "music", sizeof(song) - 1);
  song[sizeof(song) - 1] = 0;
  char* dash = strstr(song, " - ");
  if (dash) { strncpy(artist, dash + 3, sizeof(artist) - 1); artist[sizeof(artist) - 1] = 0; *dash = 0; }
  int w = (int)strlen(song) * 6;
  if (w <= 128 - RX) txt(RX, 10, song);
  else {
    int span = w + 30, off = (int)((f / 3) % span);
    txt(RX - off, 10, song);
    txt(RX - off + span, 10, song);
  }
  if (strlen(artist) > 10) { artist[8] = '.'; artist[9] = '.'; artist[10] = 0; }
  txt(RX, 21, artist);
  display.fillRect(0, 8, RX - 1, 12, C_BLACK);            // the gliding song stays in its column

  if (trackDur) {                                         // progress line with a playhead
    uint32_t pos = trackPos + (musicOn ? (millis() - trackAt) / 1000 : 0);
    if (!bridgeMusic) pos %= trackDur;                    // the demo just loops
    if (pos > trackDur) pos = trackDur;
    int len = 126 - RX, fw = (int)(len * pos / trackDur);
    for (int x = RX + fw; x < RX + len; x += 2) display.drawPixel(x, 35, C_WHITE);
    display.drawFastHLine(RX, 35, fw, C_WHITE);
    display.fillRect(RX + fw - 1, 34, 3, 3, C_WHITE);
  }
  static const int BX[3] = { 72, 95, 117 };
  for (uint8_t b = 1; b <= 3; b++) btnIcon(b, BX[b - 1], 50, C_WHITE);

  const int cx = 30, cy = 32;                             // the record
  display.drawCircle(cx, cy, 28, C_WHITE);
  display.drawCircle(cx, cy, 27, C_WHITE);
  grooveRing(cx, cy, 24, discAngle);
  grooveRing(cx, cy, 20, discAngle);
  grooveRing(cx, cy, 16, discAngle);
  for (int r = 13; r <= 25; r++) {                        // glossy sheen (the light doesn't turn)
    display.drawPixel(cx + (int)(r * 0.70f), cy - (int)(r * 0.70f), C_WHITE);
    display.drawPixel(cx - (int)(r * 0.70f), cy + (int)(r * 0.70f), C_WHITE);
  }
  display.fillCircle(cx, cy, 10, C_WHITE);                // the label, its star turning
  for (uint8_t k = 0; k < 3; k++) {
    float a = discAngle + k * PI / 3;
    int dx = (int)lroundf(cosf(a) * 7), dy = (int)lroundf(sinf(a) * 7);
    display.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, C_BLACK);
    display.drawLine(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, C_BLACK);
  }
  display.fillCircle(cx, cy, 1, C_WHITE);
  if (!musicOn && bridgeMusic) {                          // paused: || on the label
    display.fillCircle(cx, cy, 10, C_WHITE);
    display.fillRect(cx - 4, cy - 5, 3, 11, C_BLACK);
    display.fillRect(cx + 1, cy - 5, 3, 11, C_BLACK);
  }
  display.fillCircle(60, 6, 3, C_WHITE);                  // tonearm: needle down while playing
  display.fillCircle(60, 6, 1, C_BLACK);
  int ex = musicOn ? 55 : 62, ey = musicOn ? 24 : 27;
  int tx = musicOn ? 47 : 60, ty = musicOn ? 22 : 34;
  display.drawLine(60, 8, ex, ey, C_WHITE);
  display.drawLine(61, 8, ex + 1, ey, C_WHITE);
  display.drawLine(ex, ey, tx, ty, C_WHITE);
  display.fillRect(tx - 2, ty - 1, 4, 3, C_WHITE);
}

/* the equalizer - the Claude face's: 14 bars that snap to the song's real
   loudness, the bass kicking on each beat, a soft fall and a peak cap */
void drawEqualizer(uint32_t f) {
  static float bar[14];
  float lv = levelAt && millis() - levelAt < 1500 ? musicLevel : musicSmooth;
  for (uint8_t i = 0; i < 14; i++) {
    float target;
    if (musicOn) {                                  // the loudness, spread over the bands
      float shape = 0.45f + 0.55f * (sinf(f * 0.31f + i * 1.7f) * 0.5f + 0.5f);
      float bass  = i < 4 ? beatPulse() * 0.5f : 0;
      target = lv * 0.46f * (shape + bass);
    } else {                                        // paused: a slow idle ripple
      float a = sinf(f * 0.12f + i * 0.9f) + sinf(f * 0.05f + i * 2.1f) * 0.6f;
      target = fabsf(a) * 4;
    }
    bar[i] = target > bar[i] ? target : bar[i] * 0.82f;   // fast attack, smooth fall
    int h = 4 + (int)bar[i];
    if (h > 52) h = 52;
    display.fillRect(4 + i * 9, 59 - h, 7, h, C_WHITE);
    display.drawFastHLine(4 + i * 9, 59 - h - 2, 7, C_WHITE);   // peak cap
  }
  display.drawFastHLine(0, 60, 128, C_WHITE);
}

/* ============================================================
   BRIDGE — lines over USB or Bluetooth (the phone app), one per line:
     M:<0 none|1 playing|2 paused>,<Song - Artist>   what's playing
     Q:<position s>,<length s>                        where in the song
     S:<n>  show screen n      S:+  next screen
     B:<0-7> brightness        V:<0-6> volume        O:<0|1> sound
     I:     tell me everything  ->  K:<screen>,<screens>,<bri>,<vol>,<sound>
                                    N:<i>,<name>   (one per screen)
   Bluetooth: a "wireless serial cable" (Nordic UART service), like the
   Claude face; it advertises as BLE_NAME.
   ============================================================ */
#define BLE_NAME     "Snoopy Keychain"
#define NUS_SERVICE  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_OTA      "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"   // firmware updates
#define NUS_PIC      "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"   // My picture, live
QueueHandle_t bleQ = NULL;
volatile bool bleConnected = false;
BLECharacteristic* bleTx = NULL;
uint32_t lastLinkAt = 0;                  // the app was busy with us: stay awake

class BleRx : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    auto v = ch->getValue();
    for (size_t i = 0; i < v.length(); i++) { char c = v[i]; xQueueSend(bleQ, &c, 0); }
  }
};
/* ============================================================
   UPDATE OVER BLUETOOTH - the app sends the new firmware:
     U:<size>,<md5>    -> U:ready,<piece size>   (or U:err,<why>)
     then the firmware itself, in pieces, written to the OTA characteristic
     U:end             -> U:ok, and it restarts on the new firmware
     U:abort           -> stops (the old firmware stays)
   The new copy goes next to the running one; a bad or cut-off copy is never
   started (its md5 is checked), so the keychain can't be broken this way.
   ============================================================ */
volatile bool otaOn = false, otaFail = false, otaDrop = false;
volatile uint32_t otaGot = 0;
uint32_t otaSize = 0, otaRebootAt = 0;
BLEServer* bleSrv = NULL;

void bleSpeed(bool fast) {                    // fast while updating; gentle on the battery otherwise
  if (!bleSrv || !bleConnected) return;
  bleSrv->updateConnParams(bleSrv->getConnId(), fast ? 6 : 24, fast ? 12 : 40, 0, 400);   // 1.25 ms steps
}

class OtaRx : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    if (!otaOn || otaFail) return;
    size_t n = ch->getLength();
    if (Update.write(ch->getData(), n) != n) otaFail = true;
    else otaGot += n;
  }
};

class PicRx : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    size_t n = ch->getLength();
    const uint8_t* d = ch->getData();
    if (n < 3) return;
    uint16_t off = (d[0] << 8) | d[1];
    if (off >= sizeof(picBuf)) return;
    size_t len = n - 2;
    if (off + len > sizeof(picBuf)) len = sizeof(picBuf) - off;
    memcpy(picBuf + off, d + 2, len);
    picSet = true; picNew = true;
  }
};

class BleConn : public BLEServerCallbacks {
  void onConnect(BLEServer* srv) override    { bleConnected = true; }
  void onDisconnect(BLEServer* srv) override { bleConnected = false; if (otaOn) otaDrop = true; BLEDevice::startAdvertising(); }
};

void bleInit() {
  bleQ = xQueueCreate(1024, 1);
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(517);                         // big pieces: a quicker update
  BLEServer* srv = BLEDevice::createServer();
  bleSrv = srv;
  srv->setCallbacks(new BleConn());
  BLEService* svc = srv->createService(NUS_SERVICE);
  BLECharacteristic* rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRx());
  bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());
  BLECharacteristic* ota = svc->createCharacteristic(NUS_OTA, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ota->setCallbacks(new OtaRx());
  BLECharacteristic* pic = svc->createCharacteristic(NUS_PIC, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pic->setCallbacks(new PicRx());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void sendLine(const char* line) {                 // back to the app / bridge
  Serial.println(line);
  if (bleConnected && bleTx) {
    String t = String(line) + "\n";
    bleTx->setValue((uint8_t*)t.c_str(), t.length());
    bleTx->notify();
    delay(4);
  }
}

void sendInfo() {
  char b[64];
  // which firmware, that it can update over Bluetooth, and the link's packet size (MTU)
  snprintf(b, sizeof(b), "F:" FW_VERSION ",ota,%u", (unsigned)(bleConnected && bleSrv ? bleSrv->getPeerMTU(bleSrv->getConnId()) : 0));
  sendLine(b);
  snprintf(b, sizeof(b), "K:%u,%u,%u,%u,%u", screen, SCREENS, briIndex, volIndex, soundOn ? 1 : 0);
  sendLine(b);
  for (uint8_t i = 0; i < SCREENS; i++) {
    const char* n = i < KC_ANIM_COUNT ? KC_ANIMS[i].name : (BUILTIN_PLAYER && i == PLAYER_SCREEN) ? "Record player"
                  : (BUILTIN_PICTURE && i == PICTURE_SCREEN) ? "My picture" : "Night ride";
    snprintf(b, sizeof(b), "N:%u,%s", i, n);
    sendLine(b);
  }
}

void goToScreen(uint8_t n);
extern bool menuOpen;
void closeMenu();
void handleLine(char* line) {
  if (strlen(line) < 2 || line[1] != ':') return;
  char* v = line + 2;
  lastLinkAt = millis();
  switch (line[0]) {
    case 'M': {
      bridgeMusic = true;
      bool play = v[0] == '1';
      if (musicOn && !play) { trackPos += (millis() - trackAt) / 1000; trackAt = millis(); }   // paused: freeze the bar here
      else if (!musicOn && play) trackAt = millis();                                           // playing again: carry on
      musicOn = play;
      musicState = v[0] - '0'; musicAt = millis();
      char* p = strchr(v, ',');
      if (p && strncmp(musicTitle, p + 1, sizeof(musicTitle) - 1) != 0) newSong = true;
      if (p) { strncpy(musicTitle, p + 1, sizeof(musicTitle) - 1); musicTitle[sizeof(musicTitle) - 1] = 0; }
      if (v[0] == '0') musicTitle[0] = 0;
      break;
    }
    case 'A': {                                       // A:<level> ~8x a second
      musicLevel = constrain(atoi(v), 0, 99); levelAt = millis();
      if (musicLevel > musicAvg + 14 && millis() - beatAt > 180) beatAt = millis();
      musicAvg = musicAvg * 0.9f + musicLevel * 0.1f;
      break;
    }
    case 'U': {
      char b[64];
      if (!strcmp(v, "abort")) { if (otaOn) Update.abort(); otaOn = false; bleSpeed(false); break; }
      if (!strcmp(v, "end")) {
        if (!otaOn) { sendLine("U:err,not started"); break; }
        otaOn = false;
        if (otaFail || otaGot != otaSize) {
          Update.abort(); bleSpeed(false);
          snprintf(b, sizeof(b), "U:err,got %lu of %lu", (unsigned long)otaGot, (unsigned long)otaSize); sendLine(b); break;
        }
        if (!Update.end(true)) { bleSpeed(false); snprintf(b, sizeof(b), "U:err,%s", Update.errorString()); sendLine(b); break; }
        sendLine("U:ok");
        otaRebootAt = millis() + 800;                     // restart on the new firmware
        break;
      }
      unsigned long size = 0; char md5[40] = "";
      sscanf(v, "%lu,%39s", &size, md5);
      if (Update.isRunning()) Update.abort();
      otaOn = false; otaFail = false; otaGot = 0; otaSize = size;
      if (!size || !Update.begin(size)) { snprintf(b, sizeof(b), "U:err,%s", size ? Update.errorString() : "no size"); sendLine(b); break; }
      if (strlen(md5) == 32) Update.setMD5(md5);
      if (menuOpen) closeMenu();
      otaOn = true;
      bleSpeed(true);
      uint16_t mtu = bleSrv ? bleSrv->getPeerMTU(bleSrv->getConnId()) : 23;
      snprintf(b, sizeof(b), "U:ready,%u", (unsigned)constrain((int)mtu - 3, 20, 500));
      sendLine(b);
      break;
    }
    case 'P':                                         // My picture: keep it / wipe it
      if (!strcmp(v, "save")) { prefs.begin("keychain", false); prefs.putBytes("pic", picBuf, sizeof(picBuf)); prefs.end(); }
      else if (!strcmp(v, "clear")) { memset(picBuf, 0, sizeof(picBuf)); picSet = true; prefs.begin("keychain", false); prefs.putBytes("pic", picBuf, sizeof(picBuf)); prefs.end(); }
      break;
    case 'Q': {
      int ps = 0, d = 0;
      sscanf(v, "%d,%d", &ps, &d);
      trackPos = ps > 0 ? ps : 0; trackDur = d > 0 ? d : 0; trackAt = millis();
      break;
    }
    case 'S':
      manualAt = millis(); newSong = false;
      if (v[0] == '+') nextScreen();
      else goToScreen((uint8_t)atoi(v));
      break;
    case 'B': briIndex = constrain(atoi(v), 0, BRI_COUNT - 1); display.setContrast(BRI[briIndex]); save(); break;
    case 'V': volIndex = constrain(atoi(v), 0, VOL_COUNT - 1); save(); play({ { 2000, 50 } }, true); break;
    case 'O': soundOn = v[0] == '1'; save(); break;
    case 'I': sendInfo(); break;
  }
}

void serialUpdate() {
  static char usb[96], ble[96];
  static uint8_t nu = 0, nb = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { usb[nu] = 0; handleLine(usb); nu = 0; }
    else if (nu < sizeof(usb) - 1) usb[nu++] = c;
  }
  char c;
  while (bleQ && xQueueReceive(bleQ, &c, 0)) {
    if (c == '\n' || c == '\r') {
      ble[nb] = 0;
      if (nb && ble[0] != 'A') { Serial.print("< "); Serial.println(ble); }   // what the app sent, on USB (not the beat)
      handleLine(ble); nb = 0;
    }
    else if (nb < sizeof(ble) - 1) ble[nb++] = c;
  }
}

void drawScreen() {
  display.clearDisplay();
  if (screen < KC_ANIM_COUNT) drawCustom(screen);
  else if (BUILTIN_PICTURE && screen == PICTURE_SCREEN) {
    if (picSet) display.drawBitmap(0, 0, picBuf, SCREEN_WIDTH, SCREEN_HEIGHT, C_WHITE);
    else { txtC(22, "My picture"); txtC(36, "draw it in the app"); }
  }
  else if (BUILTIN_PLAYER && screen == PLAYER_SCREEN) { if (eqView) drawEqualizer(sceneFrame); else drawPlayer(sceneFrame); }
  else drawRide(sceneFrame);
  drawCaption();
}

void save() {
  prefs.begin("keychain", false);
  prefs.putUChar("scr", screen); prefs.putUChar("bri", briIndex); prefs.putBool("snd", soundOn);
  prefs.putUChar("vol", volIndex);
  prefs.end();
}

/* the change between screens, like the Claude face's old-TV shutter: black
   shutters close from the top and bottom with a bright edge, a glowing line
   flashes in the middle, then they open onto the next screen. It runs inside
   the normal 40 fps loop (nothing waits), so both screens keep moving and it
   stays smooth. */
#define T_CLOSE 150
#define T_LINE   40
#define T_OPEN  190
uint32_t transAt = 0;
uint8_t  transTo = 0;
bool     transSaved = true;

void nextScreen() {
  if (SCREENS <= 1) { toast("only one"); return; }
  if (transAt) transTo = (transTo + 1) % SCREENS;     // tapped again mid-way: go one further
  else { transTo = (screen + 1) % SCREENS; transAt = millis(); }
}

void goToScreen(uint8_t n) {
  if (n >= SCREENS || n == screen) return;
  transTo = n;
  if (!transAt) transAt = millis();
}

void shutters(int h) {                                // h = how far each shutter has closed (0..32)
  if (h <= 0) return;
  display.fillRect(0, 0, SCREEN_WIDTH, h, C_BLACK);
  display.fillRect(0, SCREEN_HEIGHT - h, SCREEN_WIDTH, h, C_BLACK);
  if (h < 31) {
    display.drawFastHLine(0, h, SCREEN_WIDTH, C_WHITE);
    display.drawFastHLine(0, SCREEN_HEIGHT - 1 - h, SCREEN_WIDTH, C_WHITE);
  }
}

void drawTransition() {
  if (!transAt) return;
  uint32_t t = millis() - transAt;
  if (t >= T_CLOSE && screen != transTo) {            // shut: swap now (on whichever frame comes first -
    screen = transTo;                                 // a slow screen can skip the short line moment)
    musicStart(screen);
    frameIdx = 0; frameAt = millis(); sceneFrame = 0;
    transSaved = false;
    eqView = viewFlip = false; viewAt = millis();     // a new screen: the player starts on its record
  }
  if (t >= T_CLOSE && viewFlip) { eqView = !eqView; viewFlip = false; viewAt = millis(); }
  if (t < T_CLOSE) {                                  // closing over the old screen
    float p = t / (float)T_CLOSE;
    shutters((int)(32 * p * p));
  } else if (t < T_CLOSE + T_LINE) {                  // shut: a glowing line in the middle
    display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C_BLACK);
    float p = (t - T_CLOSE) / (float)T_LINE;
    int w = (int)(SCREEN_WIDTH * (1 - p * 0.6f));
    display.drawFastHLine(64 - w / 2, 31, w, C_WHITE);
    display.drawFastHLine(64 - w / 4, 32, w / 2, C_WHITE);
  } else {                                            // opening onto the new screen
    float p = (t - T_CLOSE - T_LINE) / (float)T_OPEN;
    if (p >= 1) {
      transAt = 0;
      if (!transSaved) { save(); transSaved = true; } // write the setting once it's done
      return;
    }
    p = 1 - (1 - p) * (1 - p);
    shutters((int)(32 * (1 - p)));
  }
}

void goToSleep() {
  sndBye();
  for (int r = 75; r >= 0; r -= 5) {                      // iris closes
    drawScreen(); iris(64, 32, r); display.display();
    soundUpdate(); delay(16);
  }
  uint32_t t = millis();
  while (millis() - t < 400) soundUpdate();
  buzz(0);
  display.oled_command(0xAE);                             // screen off
  while (digitalRead(TOUCH_PIN) == HIGH) delay(10);       // let go first, or it wakes right away
  delay(150);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}


/* ============================================================
   SETTINGS — the same page as the Claude face.
   Double tap opens it. Tap = next row / one step up,
   hold = choose / done. It closes by itself after 10 s.
   ============================================================ */
#define MI_BRIGHT  0
#define MI_VOLUME  1
#define MI_SOUND   2
#define MI_EXIT    3
#define MENU_ITEMS 4
#define MENU_TIMEOUT_MS 10000
#define MENU_HOLD_MS    450
bool     menuOpen = false, menuAdjust = false;
uint8_t  menuCursor = 0;
uint32_t menuSince = 0;

void sndUiTick()   { play({ { 1600, 12 } }); }
void sndUiSelect() { play({ { 1200, 25 }, { 1500, 25 }, { 1800, 25 } }); }
void sndUiOpen()   { play({ { 880, 45 }, { 0, 20 }, { 1320, 70 } }); }
void sndUiClose()  { play({ { 1320, 45 }, { 0, 20 }, { 880, 70 } }); }

void txtCol(int x, int y, const char* t, uint16_t c) {
  display.setTextSize(1); display.setTextColor(c);
  display.setCursor(x, y); display.print(t);
  display.setTextColor(C_WHITE);
}
void txtC(int y, const char* t) { txtCol((SCREEN_WIDTH - (int)strlen(t) * 6) / 2, y, t, C_WHITE); }

void iconSun(int x, int y, uint16_t c) {
  display.fillCircle(x + 5, y + 5, 2, c);
  display.drawFastHLine(x, y + 5, 2, c);  display.drawFastHLine(x + 9, y + 5, 2, c);
  display.drawFastVLine(x + 5, y, 2, c);  display.drawFastVLine(x + 5, y + 9, 2, c);
  display.drawPixel(x + 2, y + 2, c); display.drawPixel(x + 8, y + 2, c);
  display.drawPixel(x + 2, y + 8, c); display.drawPixel(x + 8, y + 8, c);
}
void iconSpeaker(int x, int y, bool on, uint16_t c) {
  display.fillRect(x, y + 3, 3, 5, c);
  display.fillTriangle(x + 2, y + 5, x + 6, y + 1, x + 6, y + 9, c);
  if (on) {
    display.drawLine(x + 8, y + 3, x + 9, y + 5, c);  display.drawLine(x + 9, y + 5, x + 8, y + 7, c);
    display.drawLine(x + 10, y + 1, x + 11, y + 5, c); display.drawLine(x + 11, y + 5, x + 10, y + 9, c);
  } else {
    display.drawLine(x + 8, y + 3, x + 11, y + 7, c); display.drawLine(x + 11, y + 3, x + 8, y + 7, c);
  }
}
void iconNote(int x, int y, uint16_t c) {
  display.fillCircle(x + 2, y + 8, 2, c);
  display.drawFastVLine(x + 4, y + 1, 8, c);
  display.drawLine(x + 4, y + 1, x + 8, y + 3, c);
}
void iconBack(int x, int y, uint16_t c) {
  display.drawLine(x, y + 4, x + 4, y, c);
  display.drawLine(x, y + 4, x + 4, y + 8, c);
  display.drawFastHLine(x, y + 4, 9, c);
}
void miniBar(int x, int y, uint8_t idx, uint8_t count, uint16_t c) {
  for (uint8_t i = 0; i < count; i++) {
    int h = 3 + i * 5 / count;
    if (i <= idx) display.fillRect(x + i * 5, y + 7 - h, 4, h, c);
    else          display.drawPixel(x + i * 5 + 1, y + 6, c);
  }
}

void drawSettingsList(uint32_t f) {
  display.fillRect(0, 0, 128, 11, C_WHITE);
  txtCol(4, 2, "SETTINGS", C_BLACK);
  iconSpeaker(112, 0, soundOn, C_BLACK);
  for (uint8_t i = 0; i < MENU_ITEMS; i++) {
    int y = 14 + i * 12;
    bool sel = (i == menuCursor);
    uint16_t c = sel ? C_BLACK : C_WHITE;
    if (sel) display.fillRoundRect(0, y - 1, 128, 12, 2, C_WHITE);
    switch (i) {
      case MI_BRIGHT: iconSun(3, y, c);              txtCol(18, y + 2, "Brightness", c); miniBar(86, y + 1, briIndex, BRI_COUNT, c); break;
      case MI_VOLUME: iconSpeaker(2, y, soundOn, c); txtCol(18, y + 2, "Volume", c);     miniBar(91, y + 1, volIndex, VOL_COUNT, c); break;
      case MI_SOUND: {
        iconNote(4, y, c); txtCol(18, y + 2, "Sound", c);
        const char* m = soundOn ? "ON" : "OFF";
        txtCol(124 - (int)strlen(m) * 6, y + 2, m, c);
        break;
      }
      case MI_EXIT:   iconBack(4, y + 1, c);         txtCol(18, y + 2, "Done", c); break;
    }
  }
  if ((f / 10) % 2) display.fillRect(126, 14 + menuCursor * 12 + 3, 2, 4, C_BLACK);   // the cursor "breathes"
}

void drawLevelEdit(const char* title, uint8_t idx, uint8_t count, uint32_t f) {
  display.fillRect(0, 0, 128, 11, C_WHITE);
  txtCol(4, 2, title, C_BLACK);
  int gap = 2, segW = (112 - (count - 1) * gap) / count;
  int total = count * segW + (count - 1) * gap, x0 = (SCREEN_WIDTH - total) / 2;
  for (uint8_t i = 0; i < count; i++) {
    int x = x0 + i * (segW + gap), h = 5 + i * 22 / count, y = 40 - h;
    if (i <= idx) display.fillRect(x, y, segW, h, C_WHITE);
    else          display.drawRect(x, y, segW, h, C_WHITE);
  }
  char b[12];
  snprintf(b, sizeof(b), "%u / %u", idx + 1, count);
  txtC(44, b);
  if ((f / 16) % 2) txtC(55, "tap = +   hold = ok");
}

void drawSettings(uint32_t f) {
  display.clearDisplay();
  if (!menuAdjust)                  drawSettingsList(f);
  else if (menuCursor == MI_BRIGHT) drawLevelEdit("BRIGHTNESS", briIndex, BRI_COUNT, f);
  else                              drawLevelEdit("VOLUME", volIndex, VOL_COUNT, f);
}

void openMenu()  { menuOpen = true; menuAdjust = false; menuCursor = 0; menuSince = millis(); sndUiOpen(); }
void closeMenu() { menuOpen = false; menuAdjust = false; save(); sndUiClose(); }

void menuTap() {
  menuSince = millis();
  if (!menuAdjust) { menuCursor = (menuCursor + 1) % MENU_ITEMS; sndUiTick(); }
  else if (menuCursor == MI_BRIGHT) { briIndex = (briIndex + 1) % BRI_COUNT; display.setContrast(BRI[briIndex]); sndUiTick(); }
  else { volIndex = (volIndex + 1) % VOL_COUNT; play({ { 2000, 60 }, { 0, 20 }, { 2600, 60 } }, true); }   // hear it
}

void menuHold() {
  menuSince = millis();
  if (menuAdjust) { menuAdjust = false; sndUiSelect(); return; }
  switch (menuCursor) {
    case MI_BRIGHT: case MI_VOLUME: menuAdjust = true; sndUiSelect(); break;
    case MI_SOUND:  soundOn = !soundOn; if (soundOn) sndUiSelect(); break;
    case MI_EXIT:   closeMenu(); break;
  }
}

// taps and holds
bool     wasDown = false;
uint32_t downAt = 0, upAt = 0, lastStep = 0;
uint8_t  taps = 0;
bool     held = false;

void touchUpdate() {
  uint32_t now = millis();
  bool down = digitalRead(TOUCH_PIN) == HIGH;
  if (down) lastTouch = now;
  if (down && !wasDown) { downAt = now; held = false; }

  if (menuOpen) {                                       // in SETTINGS: tap = next / step, hold = choose / done
    if (down && !held && now - downAt > MENU_HOLD_MS) { held = true; menuHold(); }
    if (!down && wasDown && !held) menuTap();
    if (menuOpen && now - menuSince > MENU_TIMEOUT_MS) closeMenu();
    wasDown = down;
    return;
  }

  if (down && now - downAt > 3000) goToSleep();         // hold 3 s: sleep
  if (down && now - downAt > 650) held = true;          // a long press isn't a tap
  if (!down && wasDown && !held) { taps++; upAt = now; }
  if (!down && taps && now - upAt > 320) {
    if (taps == 1) { sndTap(); nextScreen(); manualAt = now; newSong = false; }   // tap: next screen
    else openMenu();                                    // double tap: SETTINGS
    taps = 0;
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);                                  // for a bridge, later (song titles)
  pinMode(TOUCH_PIN, INPUT);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  delay(100);
  if (!display.begin(OLED_ADDR, true)) { while (true) delay(1000); }
  display.setRotation(FLIP ? 2 : 0);
  display.setTextWrap(false);

  prefs.begin("keychain", true);
  screen   = prefs.getUChar("scr", 0);
  briIndex = prefs.getUChar("bri", 5);
  volIndex = prefs.getUChar("vol", 3);
  if (volIndex >= VOL_COUNT) volIndex = 3;
  soundOn  = prefs.getBool("snd", true);
  picSet   = BUILTIN_PICTURE && prefs.getBytes("pic", picBuf, sizeof(picBuf)) == sizeof(picBuf);
  prefs.end();
  if (screen >= SCREENS) screen = 0;
  if (briIndex >= BRI_COUNT) briIndex = 5;
  display.setContrast(BRI[briIndex]);

  buzzInit();
  bleInit();
  sndHello();
  musicStart(screen);                              // its tune starts after the hello chirp
  frameAt = millis();
  for (int r = 0; r <= 75; r += 4) {                     // iris opens on the picture
    drawScreen(); iris(64, 32, r); display.display();
    soundUpdate(); sceneFrame++;
  }
  lastTouch = millis();
}

/* music from the phone, like the Claude face: a song starts -> the record
   player comes up (shutters), paused -> it stays, stopped -> back to where
   you were. Change screen yourself and it leaves you alone for 45 s (or
   until the next song). */
void autoMusic() {
  if (!BUILTIN_PLAYER || menuOpen || transAt) return;
  uint32_t now = millis();
  if (screen == PLAYER_SCREEN) newSong = false;
  bool on = (musicState == 1 && now - musicAt < 60000UL)          // the app re-sends every 15 s while playing
         || (musicState == 2 && now - musicAt < 300000UL);        // paused a long time: the music's over
  if (screen == PLAYER_SCREEN) {                 // record <-> equalizer while it plays; the record when it stops
    bool playing = on && musicState == 1;
    if ((eqView && !playing) || (playing && now - viewAt > 12000UL)) { viewFlip = true; transTo = screen; transAt = now; return; }
  }
  if (on) {
    bool backOff = manualAt && now - manualAt < 45000UL && !newSong;
    if (musicState == 1 && !backOff && screen != PLAYER_SCREEN) {
      if (!autoPlayer) beforeMusic = screen;
      autoPlayer = true; newSong = false;
      goToScreen(PLAYER_SCREEN);
    }
  } else if (autoPlayer) {
    autoPlayer = false;
    if (screen == PLAYER_SCREEN) goToScreen(beforeMusic);
  }
}

/* the update screen: a ring that fills as the firmware comes in, the
   percentage inside it, and the phone sending little packets to the chip.
   Done: a full ring with a tick, then it restarts. All smooth, no blinking. */
void otaRing(int cx, int cy, int r, float p) {
  for (int a = 0; a < 360; a += 3) {                     // the track: a dotted circle
    float t = (a - 90) * DEG_TO_RAD;
    display.drawPixel(cx + (int)lroundf(cosf(t) * r), cy + (int)lroundf(sinf(t) * r), C_WHITE);
  }
  int end = (int)(360 * p);
  for (int a = 0; a <= end; a++) {                        // the filled part: 3 px thick
    float t = (a - 90) * DEG_TO_RAD, c = cosf(t), sn = sinf(t);
    for (int k = -1; k <= 1; k++) display.drawPixel(cx + (int)lroundf(c * (r + k)), cy + (int)lroundf(sn * (r + k)), C_WHITE);
  }
  if (p > 0 && p < 1) {                                  // a round head on the end
    float t = (end - 90) * DEG_TO_RAD;
    display.fillCircle(cx + (int)lroundf(cosf(t) * r), cy + (int)lroundf(sinf(t) * r), 2, C_WHITE);
  }
}

void drawOta() {
  static uint32_t f = 0;
  f++;
  display.clearDisplay();
  bool done = otaRebootAt != 0;
  float p = done ? 1 : (otaSize ? (float)otaGot / otaSize : 0);
  const int cx = 31, cy = 32;
  otaRing(cx, cy, 24, p);
  if (done) {                                           // a tick
    display.drawLine(cx - 9, cy, cx - 3, cy + 6, C_WHITE);  display.drawLine(cx - 9, cy + 1, cx - 3, cy + 7, C_WHITE);
    display.drawLine(cx - 3, cy + 6, cx + 9, cy - 6, C_WHITE); display.drawLine(cx - 3, cy + 7, cx + 9, cy - 5, C_WHITE);
  } else {
    char b[6]; snprintf(b, sizeof(b), "%d", (int)(p * 100));
    display.setTextSize(2); display.setTextColor(C_WHITE);
    int w = (int)strlen(b) * 12;
    display.setCursor(cx - w / 2 - 2, cy - 7); display.print(b);
    display.setTextSize(1);
    display.setCursor(cx + w / 2 - 1, cy - 7); display.print("%");
  }

  const int X = 64;                                       // the right side
  txt(X, 6, done ? "UPDATED" : "UPDATING");
  display.drawFastHLine(X, 16, done ? 42 : 48, C_WHITE);
  // phone -> packets -> chip
  display.drawRoundRect(X, 25, 9, 15, 2, C_WHITE);        // the phone
  display.drawFastHLine(X + 3, 37, 3, C_WHITE);
  display.drawRect(X + 46, 27, 11, 11, C_WHITE);          // the chip, with its pins
  display.fillRect(X + 49, 30, 5, 5, C_WHITE);
  for (int k = 0; k < 3; k++) {
    display.drawFastHLine(X + 43, 29 + k * 3, 3, C_WHITE);
    display.drawFastHLine(X + 57, 29 + k * 3, 3, C_WHITE);
  }
  if (!done) {
    for (int x = X + 12; x < X + 42; x += 3) display.drawPixel(x, 32, C_WHITE);   // the line between them
    for (int k = 0; k < 3; k++) {                         // packets gliding along it
      int px = X + 12 + (int)((f * 1.2f + k * 10) ) % 30;
      display.fillRect(px, 31, 3, 3, C_WHITE);
    }
  } else {
    display.drawFastHLine(X + 12, 32, 30, C_WHITE);
  }
  txt(X, 48, done ? "restarting" : "stay close");
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (otaDrop) { otaDrop = false; if (otaOn) { Update.abort(); otaOn = false; toast("update stopped"); } }
  if (otaRebootAt && (int32_t)(now - otaRebootAt) >= 0) ESP.restart();
  if (otaOn || otaRebootAt) {                             // updating: just the progress
    serialUpdate();
    lastTouch = now;
    if (now - last >= 40) { last = now; drawOta(); display.display(); }
    return;
  }
  touchUpdate();
  serialUpdate();
  soundUpdate();
  musicUpdate();
  if (BUILTIN_PICTURE && picNew) {
    manualAt = now; newSong = false;
    if (screen == PICTURE_SCREEN && !transAt) picNew = false;
    else if (!transAt) { goToScreen(PICTURE_SCREEN); picNew = false; }
  }
  autoMusic();
  if (bleConnected) lastTouch = now;                     // the phone app is connected: stay awake
  if (SLEEP_MIN > 0 && now - lastTouch > SLEEP_MIN * 60000UL) goToSleep();
  if (now - last < 25) return;                            // about 40 fps
  last = now;
  beatUpdate();
  sceneFrame++;
  if (menuOpen) drawSettings(sceneFrame);
  else { drawScreen(); drawTransition(); }
  drawToast();
  display.display();
}
