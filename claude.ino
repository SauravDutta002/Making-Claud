/* =============================================================
   Animated OLED Face — SCENE EDITION  (44 scenes)
   -------------------------------------------------------------
   Board   : ESP32-C3 Super Mini
   Display : 1.3" OLED 128x64, SH1106, I2C @ 0x3C
   Touch   : TTP223 capacitive module (active HIGH)

   Wiring  : OLED  VCC -> 3V3     OLED  GND -> GND
             OLED  SDA -> GPIO5   OLED  SCL -> GPIO4
             TTP223 VCC -> 3V3    TTP223 GND -> GND
             TTP223 OUT -> GPIO3
             BUZZER  +  -> GPIO6  BUZZER  -  -> GND   (passive buzzer)

   Libraries: Adafruit GFX + Adafruit SH110X

   TOUCH
     single tap  -> next face
     double tap  -> SETTINGS  (brightness, volume, sound mode)
     triple tap  -> bug-jump game  (tap = jump)
     4 taps      -> sound on / off  (quick mute)
     hold        -> pet Claude (while held)

   SETTINGS  (everything is saved; closes itself after 10 s)
     tap  = next item           hold = choose
     on Brightness / Volume:    tap = step up (live preview), hold = done
     on Sound:                  hold cycles  ALL -> VOICE -> OFF
       ALL   = voice + scene sound effects
       VOICE = only Claude's reactions (done, needs you, petting ...)
     in game over: tap = retry, hold = exit

   Serial @115200 (single keys):
     n/p = next/prev scene   r = random   c = auto-cycle
     +/- = brightness up/down   b = brightness menu
     ] / [ = volume up/down     g = start game   q = quit game
     s = sound on/off           m = cycle sound mode

   Serial @115200 (lines from claude_bridge.py on the laptop):
     E:<event>      think read type search web bash git test need
                    done fail merge wait bug plane compact pkg idle
                    key = you are typing (keyboard scene)
     U:<5h>,<wk>,<min>   usage % (5-hour, weekly), minutes to 5h reset
     X:<pct>        context window % used
     C:<hash>       new git commit (shown in the git graph)
     G:1 / G:0      start / stop the game
   ============================================================= */

/* The laptop bridge talks to this board over the C3's built-in USB port.
   Without "USB CDC On Boot" the Serial output goes to the UART pins instead
   and the bridge sees a silent, unresponsive port. */
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
#error "Arduino IDE: Tools > USB CDC On Boot > Enabled, then upload again."
#endif

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDR      0x3C
#define I2C_SDA        5
#define I2C_SCL        4

#define C_WHITE  SH110X_WHITE
#define C_BLACK  SH110X_BLACK

#define FRAME_MS    25       // ~40 fps
#define SCENE_MS    5000     // auto-cycle dwell time
#define EYE_RADIUS  0        // 0 = hard square corners, 6-9 = rounded

/* eye styles — plain defines so no custom type appears in any
   function signature (Arduino auto-prototypes would break on that) */
#define EY_NORMAL      0
#define EY_HAPPY       1
#define EY_ANGRY       2
#define EY_SAD         3
#define EY_SLEEPY      4
#define EY_SUSPICIOUS  5
#define EY_WIDE        6

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;

/* ============================================================
   GLOBAL ANIMATION STATE
   ============================================================ */
uint32_t tick       = 0;
uint32_t lastFrame  = 0;
float    gBlink     = 1.0f;       // 1 = open, 0 = closed

bool     blinking   = false;
uint32_t blinkStart = 0;
uint32_t nextBlink  = 0;
uint8_t  blinkQueue = 0;
const uint16_t BLINK_DUR = 170;

bool     autoCycle  = true;
uint8_t  scene      = 0;
uint32_t sceneSince = 0;

/* ============================================================
   LIVE DATA  (pushed from the laptop over USB serial)
   ============================================================ */
bool     live        = false;         // laptop is driving the scenes
uint32_t lastEvent   = 0;
#define  LIVE_TIMEOUT_MS  90000       // no events this long -> back to auto-cycle

int8_t   usage5h     = -1;            // -1 = unknown
int8_t   usageWk     = -1;
int16_t  resetMin    = -1;            // minutes to 5h reset, at resetStamp
uint32_t resetStamp  = 0;
int8_t   ctxPct      = -1;
char     commits[5][8] = { "1a49713", "5acfded", "0f2d768", "aaad437", "2e5f783" };

uint8_t  followStage = 0;             // after "done": trophy -> usage -> idle
uint32_t followAt    = 0;

/* ============================================================
   GAME STATE
   ============================================================ */
bool     gameMode    = false;
bool     gameOver    = false;
uint32_t gameOverAt  = 0;
uint32_t gameTick    = 0;
uint16_t gScore      = 0, gHi = 0;
bool     gNewHi      = false;
float    gY = 0, gVy = 0, gSpeed = 2.6f, gScroll = 0, gNextGap = 60;
struct Obst { float x; uint8_t w, h; bool on; };
Obst     obs[3];
#define  G_GROUND  56
#define  G_PX      14
#define  G_PW      12
#define  G_PH      14

/* ============================================================
   SETTINGS  (brightness, volume, sound mode)
   ============================================================ */
const uint8_t BRI_LEVELS[] = { 1, 10, 25, 50, 90, 150, 207, 255 };
const uint8_t BRI_COUNT    = sizeof(BRI_LEVELS) / sizeof(BRI_LEVELS[0]);
uint8_t  briIndex   = 5;          // default level, overwritten from flash

const uint16_t VOL_LEVELS[] = { 8, 16, 28, 48, 80, 130, 220 };   // PWM duty of 1023
const uint8_t  VOL_COUNT    = sizeof(VOL_LEVELS) / sizeof(VOL_LEVELS[0]);
uint8_t  volIndex   = 3;

#define  SND_OFF    0
#define  SND_VOICE  1
#define  SND_ALL    2
uint8_t  soundMode  = SND_ALL;
uint8_t  lastMode   = SND_ALL;    // restored by the 4-tap quick mute

#define  MI_BRIGHT  0
#define  MI_VOLUME  1
#define  MI_SOUND   2
#define  MI_EXIT    3
#define  MENU_ITEMS 4
bool     menuOpen   = false;
bool     menuAdjust = false;      // editing a level instead of browsing
uint8_t  menuCursor = 0;
uint32_t menuSince  = 0;
#define  MENU_TIMEOUT_MS  10000
#define  MENU_HOLD_MS     450     // a bit longer than a pet, so it's deliberate

/* ============================================================
   TOUCH SENSOR STATE
   ============================================================ */
#define TOUCH_PIN      3
#define PET_HOLD_MS    250
#define TAP_WINDOW_MS  320
#define DEBOUNCE_MS    30

bool     touchState   = false;    // debounced level
bool     touchRaw     = false;    // last raw level
uint32_t touchChange  = 0;        // when raw last changed
uint32_t touchDown    = 0;        // when the current press started
bool     petting      = false;    // showing the petted face
bool     holdConsumed = false;    // this press already did a hold action
uint8_t  tapCount     = 0;
uint32_t lastTap      = 0;
bool     touchPressed  = false;   // finger-down edge, consumed by the game
bool     touchReleased = false;   // finger-up edge, consumed by the game

/* ============================================================
   TINY HELPERS
   ============================================================ */
void txt(int x, int y, uint8_t s, const char* t) {
  display.setTextSize(s);
  display.setTextColor(C_WHITE);
  display.setCursor(x, y);
  display.print(t);
}
void txtC(int y, uint8_t s, const char* t) {
  txt((SCREEN_WIDTH - (int)strlen(t) * 6 * s) / 2, y, s, t);
}
float wobble(uint32_t f, float speed, float amp) {
  return sinf(f * speed) * amp;
}

void setBrightness(uint8_t level) {
  display.oled_command(0x81);     // SH1106 set-contrast register
  display.oled_command(level);
}
void applyBrightness() { setBrightness(BRI_LEVELS[briIndex]); }

void sparkle(int cx, int cy, uint32_t f, int base) {
  for (uint8_t i = 0; i < 4; i++) {
    float a  = (i * 45.0f + f * 6.0f) * PI / 180.0f;
    int   l  = base + ((i + f / 4) % 2);
    int   dx = (int)(cosf(a) * l), dy = (int)(sinf(a) * l);
    display.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, C_WHITE);
  }
  display.fillRect(cx - 1, cy - 1, 2, 2, C_WHITE);
}

void heart(int cx, int cy, int s) {
  display.fillCircle(cx - s / 2, cy - s / 3, s / 2 + 1, C_WHITE);
  display.fillCircle(cx + s / 2, cy - s / 3, s / 2 + 1, C_WHITE);
  display.fillTriangle(cx - s - 1, cy - s / 5, cx + s + 1, cy - s / 5,
                       cx, cy + s, C_WHITE);
}

void note(int x, int y, int h) {                 // music note glyph
  display.fillCircle(x, y + h, 2, C_WHITE);
  display.drawFastVLine(x + 2, y, h, C_WHITE);
  display.drawLine(x + 2, y, x + 6, y + 3, C_WHITE);
}

void winBox(int x, int y, int w, int h, const char* title) {
  display.drawRect(x, y, w, h, C_WHITE);
  display.fillRect(x + 1, y + 1, w - 2, 9, C_WHITE);
  display.setTextColor(C_BLACK);
  display.setTextSize(1);
  display.setCursor(x + 4, y + 2);
  display.print(title);
  display.setTextColor(C_WHITE);
}

void meter(int x, int y, int w, int h, uint8_t pct) {
  display.drawRect(x, y, w, h, C_WHITE);
  int fill = (int)((w - 4) * (pct / 100.0f));
  if (fill > 0) display.fillRect(x + 2, y + 2, fill, h - 4, C_WHITE);
}

void steam(int x, int y, uint32_t f, int cols) {
  for (int i = 0; i < cols; i++)
    for (int k = 0; k < 12; k++) {
      int xx = x + i * 6 + (int)(sinf(k * 0.55f + f * 0.15f + i * 1.7f) * 2.2f);
      display.drawPixel(xx, y - k, C_WHITE);
    }
}

/* ============================================================
   VOICE  — soft gliding babble on a passive buzzer (GPIO6)
   Each phrase is a queue of syllables; soundUpdate() plays them
   without blocking, with a gentle attack/release and vibrato.
   ============================================================ */
#define BUZZER_PIN  6
#define BUZZ_BITS   10        // loudness = VOL_LEVELS[volIndex], set in SETTINGS

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define BZ_ID BUZZER_PIN    // core 3.x addresses LEDC by pin
#else
  #define BZ_ID 0             // core 2.x addresses LEDC by channel
#endif

struct Syl { uint16_t f0, f1, dur, gap; uint8_t v; };   // Hz, Hz, ms, ms, volume %
Syl      sq[24];
uint8_t  sqHead = 0, sqLen = 0;
uint32_t sylStart = 0;
bool     soundOn = true;                     // soundMode != SND_OFF
uint16_t bzF = 0, bzD = 0;                   // what the PWM is set to now

uint32_t toastUntil = 0;                     // "sound on/off" popup
const char* MODE_NAMES[] = { "OFF", "VOICE", "ALL" };

const float SCALE[] = { 1.0f, 1.125f, 1.25f, 1.5f, 1.667f };   // major pentatonic

/* Sound runs in its own FreeRTOS task, updated every 1 ms, so glides
   stay smooth while the main loop is busy pushing frames over I2C. */
SemaphoreHandle_t sndLock = NULL;
void soundUpdate();

void soundTask(void* arg) {
  for (;;) { soundUpdate(); vTaskDelay(1); }
}

void buzzInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 1000, BUZZ_BITS);
#else
  ledcSetup(BZ_ID, 1000, BUZZ_BITS);
  ledcAttachPin(BUZZER_PIN, BZ_ID);
#endif
  ledcWrite(BZ_ID, 0);
  sndLock = xSemaphoreCreateMutex();
  xTaskCreate(soundTask, "sound", 3072, NULL, 2, NULL);
}

void buzzOut(uint16_t f, uint16_t d) {
  if (f == 0 || d == 0) {
    if (bzD) { ledcWrite(BZ_ID, 0); bzD = 0; }
    return;
  }
  if (f > bzF + 3 || f + 3 < bzF) { ledcChangeFrequency(BZ_ID, f, BUZZ_BITS); bzF = f; bzD = 0; }
  if (d != bzD) { ledcWrite(BZ_ID, d); bzD = d; }
}

void sndClear() {
  if (!sndLock) return;
  xSemaphoreTake(sndLock, portMAX_DELAY);
  sqHead = 0; sqLen = 0; sylStart = millis();
  xSemaphoreGive(sndLock);
}

void sndPushV(uint16_t f0, uint16_t f1, uint16_t dur, uint16_t gap, uint8_t v) {
  if (!sndLock) return;
  xSemaphoreTake(sndLock, portMAX_DELAY);
  if (sqHead + sqLen < 24) {
    if (sqLen == 0) sylStart = millis();
    sq[sqHead + sqLen] = { f0, f1, dur, gap, v };
    sqLen++;
  }
  xSemaphoreGive(sndLock);
}
void sndPush(uint16_t f0, uint16_t f1, uint16_t dur, uint16_t gap) {
  sndPushV(f0, f1, dur, gap, 100);
}

void soundUpdate() {
  xSemaphoreTake(sndLock, portMAX_DELAY);
  if (!sqLen) { buzzOut(0, 0); xSemaphoreGive(sndLock); return; }
  uint32_t now = millis(), t = now - sylStart;
  const Syl& s = sq[sqHead];

  if (t < s.dur) {
    float k = (float)t / s.dur;
    float f = s.f0 + ((float)s.f1 - s.f0) * k;
    f *= 1.0f + 0.012f * sinf(t * 0.07f);              // little vibrato
    // soft attack / release, shortened for tiny clicks
    uint16_t att = s.dur / 4 < 8  ? s.dur / 4 : 8;
    uint16_t rel = s.dur / 3 < 14 ? s.dur / 3 : 14;
    uint32_t rem = s.dur - t;
    float env = 1.0f;
    if (att && t < att) env = (float)t / att;
    if (rel && rem < rel && (float)rem / rel < env) env = (float)rem / rel;
    buzzOut((uint16_t)f, (uint16_t)(VOL_LEVELS[volIndex] * env * s.v / 100.0f) + 1);
  } else if (t < (uint32_t)s.dur + s.gap) {
    buzzOut(0, 0);
  } else {
    sqHead++; sqLen--;
    sylStart = now;
  }
  xSemaphoreGive(sndLock);
}

/* Claude "talking": n syllables around base Hz; the last one
   rises (+1, happy / question) or falls (-1, sad). */
void say(uint8_t n, uint16_t base, int8_t ending) {
  if (!soundOn) return;
  sndClear();
  for (uint8_t i = 0; i < n; i++) {
    uint16_t f0 = (uint16_t)(base * SCALE[random(5)]);
    int16_t  g  = random(-70, 71);
    if (i == n - 1) g = ending * (int16_t)(base * 0.4f);
    sndPush(f0, f0 + g, random(50, 95), i == n - 1 ? 0 : random(25, 60));
  }
}

void sndNotes(const uint16_t* notes, uint8_t n, uint16_t dur, uint16_t gap) {
  if (!soundOn) return;
  sndClear();
  for (uint8_t i = 0; i < n; i++) sndPush(notes[i], notes[i], dur, gap);
}

void sndHello()   { static const uint16_t n[] = { 660, 880, 1320 }; sndNotes(n, 3, 70, 30); }
void sndHm()      { if (soundOn) { sndClear(); sndPush(700, 840, 70, 0); } }
void sndNeed()    { say(3, 850, +1); }                  // a little question
void sndDone()    { say(4, 950, +1); }                  // happy chatter
void sndFail()    { if (soundOn) { sndClear(); sndPush(700, 600, 110, 50); sndPush(560, 400, 180, 0); } }
void sndCommit()  { if (soundOn) { sndClear(); sndPush(1320, 1320, 60, 25); sndPush(1760, 1760, 130, 0); } }
void sndGoal()    { static const uint16_t n[] = { 1047, 1319, 1568, 1319, 1568, 2093 }; sndNotes(n, 6, 55, 15); }
void sndYay()     { static const uint16_t n[] = { 1047, 1319, 1568 }; sndNotes(n, 3, 60, 20); }
void sndJump()    { if (soundOn) { sndClear(); sndPush(700, 1100, 40, 0); } }
void sndOver()    { static const uint16_t n[] = { 784, 659, 523 }; sndNotes(n, 3, 100, 30); }
void sndHiScore() { static const uint16_t n[] = { 523, 659, 784, 1047 }; sndNotes(n, 4, 80, 20); }

/* --- petting voice ------------------------------------------
   hold starts -> "oo-h!"   while held -> coos / purr-trills
   after 3 s   -> giggle    let go     -> contented "mm~"      */
uint32_t petStart = 0, nextCoo = 0;
bool     giggled  = false;

void sndPetStart() {
  petStart = millis(); nextCoo = petStart + 1100; giggled = false;
  if (!soundOn) return;
  sndClear();
  sndPushV(900, 1300, 90, 25, 90);                       // "oo-
  sndPushV(1300, 1550, 70, 0, 100);                      //    -h!"
}

void sndCoo() {                                          // two soft falling notes
  uint16_t f = (uint16_t)(1000 * SCALE[random(5)]);
  sndClear();
  sndPushV(f, (uint16_t)(f * 0.86f), 120, 70, 70);
  sndPushV((uint16_t)(f * 1.12f), (uint16_t)(f * 0.94f), 170, 0, 55);
}

void sndChirr() {                                        // happy purr-trill
  sndClear();
  for (uint8_t i = 0; i < 8; i++)
    sndPushV(i % 2 ? 1180 : 1080, i % 2 ? 1120 : 1140, 34, 0, 45 + i * 5);
}

void sndGiggle() {
  static const uint16_t n[] = { 1175, 1319, 1480, 1760 };
  sndClear();
  for (uint8_t i = 0; i < 4; i++) sndPushV(n[i], n[i] + 60, 45, 35, 80);
}

void sndPetEnd() {                                       // contented sigh
  if (!soundOn) return;
  sndClear();
  sndPushV(1250, 1350, 60, 0, 70);
  sndPushV(1350, 820, 260, 0, 50);
}

void petUpdate() {
  if (!petting || !soundOn || sqLen) return;
  uint32_t now = millis();
  if (now < nextCoo) return;
  if (!giggled && now - petStart > 3000) { sndGiggle(); giggled = true; }
  else if (random(3) == 0)               sndChirr();
  else                                   sndCoo();
  nextCoo = now + random(1300, 2100);
}

/* --- settings persistence + sound mode ---------------------- */
void setSoundMode(uint8_t m) {
  soundMode = m;
  soundOn   = (m != SND_OFF);
  if (m != SND_OFF) lastMode = m;
}

void saveSettings() {
  prefs.begin("face", false);
  prefs.putUChar("bri",  briIndex);
  prefs.putUChar("vol",  volIndex);
  prefs.putUChar("sndm", soundMode);
  prefs.end();
}

/* --- UI sounds (quiet, short) -------------------------------- */
void sndUiTick()   { if (soundOn) { sndClear(); sndPushV(1600, 1600, 12, 0, 45); } }
void sndUiSelect() { if (soundOn) { sndClear(); sndPushV(1200, 1800, 50, 0, 60); } }
void sndUiOpen()   { if (soundOn) { sndClear(); sndPushV(880, 880, 45, 20, 55); sndPushV(1320, 1320, 70, 0, 60); } }
void sndUiClose()  { if (soundOn) { sndClear(); sndPushV(1320, 1320, 45, 20, 55); sndPushV(880, 880, 70, 0, 50); } }
void sndPreview()  {          // always plays, so you hear the new volume
  sndClear(); sndPushV(1320, 1320, 50, 30, 100); sndPushV(1760, 1760, 90, 0, 100);
}

/* 4-tap quick mute: off <-> whatever mode you had before */
void toggleSound() {
  setSoundMode(soundOn ? SND_OFF : lastMode);
  sndClear();                                           // confirmation plays even when turning off
  if (soundOn) { sndPush(660, 660, 70, 30); sndPush(990, 990, 110, 0); }
  else         { sndPush(990, 990, 70, 30); sndPush(660, 660, 110, 0); }
  saveSettings();
  toastUntil = millis() + 1500;
  Serial.printf("sound %s\n", MODE_NAMES[soundMode]);
}

/* small speaker glyph, 12x11, drawn in colour c */
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

void drawToast() {
  if (millis() > toastUntil) return;
  display.fillRect(22, 18, 84, 26, C_BLACK);
  display.drawRect(22, 18, 84, 26, C_WHITE);
  iconSpeaker(30, 25, soundOn, C_WHITE);
  txt(50, 23, 1, "sound");
  txt(50, 33, 1, soundMode == SND_OFF ? "off" : soundMode == SND_VOICE ? "voice" : "on");
}

/* --- scene sound effects (only in ALL mode) ------------------
   Quiet, synced to each scene's animation, and they never cut
   into Claude's voice: they only play when the queue is empty. */
const uint16_t MELODY[] = { 1047, 1175, 1319, 1568, 1760 };

bool ambOk() { return soundMode == SND_ALL && sqLen == 0; }
void amb(uint16_t f0, uint16_t f1, uint16_t dur, uint8_t v) { if (ambOk()) sndPushV(f0, f1, dur, 0, v); }

void sceneSfx(uint8_t sc, uint32_t f) {
  if (soundMode != SND_ALL) return;
  switch (sc) {
    case 1: case 2:                                       // thinking: soft twinkle
      if (f % 120 == 0 && ambOk()) { sndPushV(1568, 1568, 40, 30, 30); sndPushV(2093, 2093, 70, 0, 22); }
      break;
    case 3:  if (f % 80 == 0) amb(2093, 2093, 120, 28); break;          // lightbulb ding
    case 4:  if (f % 160 == 0) amb(900, 1400, 30, 18); break;           // page turn
    case 5:  if (f % 3 == 0 && random(10) < 4) amb(2400, 2100, 8, 28); break;   // typing
    case 6:  if (f % 6 == 0) amb(2300, 2000, 8, 28); break;             // key peek
    case 44: if (f % 4 == 0 && random(10) < 7) amb(2400, 2000, 8, 28); break;   // typing along
    case 8: case 31:                                      // searching: sonar ping
      if (f % 80 == 0 && ambOk()) { sndPushV(1400, 1300, 60, 120, 26); sndPushV(1400, 1300, 40, 0, 12); }
      break;
    case 9:  if (f % 16 == 0) { uint16_t b = random(700, 1100); amb(b, b + 300, 25, 18); } break;   // bubbles
    case 13: case 14: case 15: case 16: case 17:          // music: soft melody
      if (f % 10 == 0 && random(4)) { uint16_t n = MELODY[random(5)]; amb(n, n, 120, 22); }
      break;
    case 18:                                              // love: heartbeat
      if (f % 40 == 0 && ambOk()) { sndPushV(520, 520, 25, 90, 40); sndPushV(480, 480, 35, 0, 30); }
      break;
    case 20: if (f % 36 == 0) amb(1200, 1500, 30, 28); break;           // juggling catch
    case 21: if (f % 50 == 0) { uint16_t n = 2093 + random(3) * 250; amb(n, n, 50, 18); } break;   // stars
    case 22: case 23: if (f % 84 == 0) amb(520, 430, 500, 16); break;   // snore
    case 30:                                              // terminal: test dots, then a bonk
      if (f % 7 == 0) {
        uint8_t n = (f / 7) % 9;
        if (n >= 1 && n < 8) amb(2000, 2000, 6, 20);
        else if (n == 8)     amb(420, 300, 120, 35);
      }
      break;
    case 32: if (f % 14 == 0) amb(1760, 1760, 15, 20); break;           // git commit hop
    case 36:                                              // old goal: kick + net
      if (f % 32 == 0)       amb(600, 350, 30, 50);
      else if (f % 32 == 28) amb(1800, 900, 80, 22);
      break;
    case 42: if (f % 75 == 0) amb(1500, 700, 300, 14); break;           // paper plane whoosh
    case 43: if (f % 60 == 31) amb(900, 200, 90, 40); break;            // squash
    case 45:                                              // GOAL! little cheer
      if (f % 40 == 0 && ambOk()) {
        sndPushV(1047, 1047, 50, 15, 32); sndPushV(1319, 1319, 50, 15, 32); sndPushV(1568, 1568, 80, 0, 32);
      }
      break;
    case 46: {                                            // bicycle kick, synced to the animation
      uint8_t t = f % 90;
      if      (t == 32) amb(700, 1400, 110, 22);          // flip
      else if (t == 37) amb(520, 260, 45, 60);            // kick
      else if (t == 66) amb(1800, 800, 120, 28);          // net swish
      else if (t == 71 && ambOk()) {
        sndPushV(1319, 1319, 45, 15, 30); sndPushV(1568, 1568, 45, 15, 30); sndPushV(2093, 2093, 90, 0, 30);
      }
      break;
    }
  }
}

/* ============================================================
   EYE RENDERING
   ============================================================ */
void eyeBody(int cx, int cy, int w, int h, int r) {
  if (h < 4) { display.fillRect(cx - w / 2, cy - 1, w, 3, C_WHITE); return; }

  if (r > EYE_RADIUS) r = EYE_RADIUS;      // global corner clamp
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;

  if (r <= 0) display.fillRect(cx - w / 2, cy - h / 2, w, h, C_WHITE);
  else        display.fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, C_WHITE);
}

void eyeMask(uint8_t s, int cx, int cy, int w, int h, int r, int side) {
  int x = cx - w / 2, y = cy - h / 2;
  if (h < 7) return;
  switch (s) {
    case EY_HAPPY:
      display.fillRoundRect(x - 3, y + (int)(h * 0.45f), w + 6, h, 6, C_BLACK);
      break;
    case EY_ANGRY:
      if (side < 0) display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + (int)(h * .55f), C_BLACK);
      else          display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1,     y + (int)(h * .55f), C_BLACK);
      break;
    case EY_SAD:
      if (side < 0) display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1,     y + (int)(h * .55f), C_BLACK);
      else          display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + (int)(h * .55f), C_BLACK);
      break;
    case EY_SLEEPY:
      display.fillRect(x - 2, y - 2, w + 4, (int)(h * .55f) + 2, C_BLACK);
      break;
    case EY_SUSPICIOUS:
      display.fillRect(x - 2, y - 2, w + 4, (int)(h * .32f) + 2, C_BLACK);
      display.fillRect(x - 2, y + (int)(h * .70f), w + 4, h, C_BLACK);
      break;
    default: break;
  }
}

/* main eye pair — blink is applied here */
void eyes(int cx, int cy, int w, int h, int gap, uint8_t s, int r, bool applyBlink) {
  int hh = applyBlink ? (int)lroundf(h * gBlink) : h;
  if (hh < 3) hh = 3;
  int lx = cx - (w + gap) / 2, rx = cx + (w + gap) / 2;
  eyeBody(lx, cy, w, hh, r); eyeMask(s, lx, cy, w, hh, r, -1);
  eyeBody(rx, cy, w, hh, r); eyeMask(s, rx, cy, w, hh, r, +1);
}

/* convenience overloads (blink on by default) */
void eyes(int cx, int cy, int w, int h, int gap, uint8_t s, int r) {
  eyes(cx, cy, w, h, gap, s, r, true);
}
void eyes(int cx, int cy, int w, int h, int gap, uint8_t s) {
  eyes(cx, cy, w, h, gap, s, 7, true);
}

/* alternate eye shapes */
void chevronEyes(int cx, int cy, int w, bool inward) {
  int lx = cx - 22, rx = cx + 22;
  for (int t = 0; t < 3; t++) {
    if (inward) {                                  // >  <
      display.drawLine(lx - w / 2, cy - w / 2 + t, lx + w / 2, cy + t, C_WHITE);
      display.drawLine(lx + w / 2, cy + t, lx - w / 2, cy + w / 2 + t, C_WHITE);
      display.drawLine(rx + w / 2, cy - w / 2 + t, rx - w / 2, cy + t, C_WHITE);
      display.drawLine(rx - w / 2, cy + t, rx + w / 2, cy + w / 2 + t, C_WHITE);
    } else {                                       // ^  ^
      display.drawLine(lx - w / 2, cy + w / 2 + t, lx, cy - w / 2 + t, C_WHITE);
      display.drawLine(lx, cy - w / 2 + t, lx + w / 2, cy + w / 2 + t, C_WHITE);
      display.drawLine(rx - w / 2, cy + w / 2 + t, rx, cy - w / 2 + t, C_WHITE);
      display.drawLine(rx, cy - w / 2 + t, rx + w / 2, cy + w / 2 + t, C_WHITE);
    }
  }
}

void xEyes(int cx, int cy, int s) {
  int lx = cx - 24, rx = cx + 24;
  for (int t = 0; t < 2; t++) {
    display.drawLine(lx - s, cy - s + t, lx + s, cy + s + t, C_WHITE);
    display.drawLine(lx + s, cy - s + t, lx - s, cy + s + t, C_WHITE);
    display.drawLine(rx - s, cy - s + t, rx + s, cy + s + t, C_WHITE);
    display.drawLine(rx + s, cy - s + t, rx - s, cy + s + t, C_WHITE);
  }
}

/* ============================================================
   SETTINGS SCREENS
   ============================================================ */
void txtCol(int x, int y, const char* t, uint16_t c) {
  display.setTextSize(1);
  display.setTextColor(c);
  display.setCursor(x, y);
  display.print(t);
  display.setTextColor(C_WHITE);
}

void iconSun(int x, int y, uint16_t c) {               // 11x11
  display.fillCircle(x + 5, y + 5, 2, c);
  display.drawFastHLine(x, y + 5, 2, c);  display.drawFastHLine(x + 9, y + 5, 2, c);
  display.drawFastVLine(x + 5, y, 2, c);  display.drawFastVLine(x + 5, y + 9, 2, c);
  display.drawPixel(x + 2, y + 2, c); display.drawPixel(x + 8, y + 2, c);
  display.drawPixel(x + 2, y + 8, c); display.drawPixel(x + 8, y + 8, c);
}

void iconNote(int x, int y, uint16_t c) {              // 9x11
  display.fillCircle(x + 2, y + 8, 2, c);
  display.drawFastVLine(x + 4, y + 1, 8, c);
  display.drawLine(x + 4, y + 1, x + 8, y + 3, c);
}

void iconBack(int x, int y, uint16_t c) {              // 9x9 arrow
  display.drawLine(x, y + 4, x + 4, y, c);
  display.drawLine(x, y + 4, x + 4, y + 8, c);
  display.drawFastHLine(x, y + 4, 9, c);
}

/* mini level bar used in the list */
void miniBar(int x, int y, uint8_t idx, uint8_t count, uint16_t c) {
  for (uint8_t i = 0; i < count; i++) {
    int h = 3 + i * 5 / count;
    if (i <= idx) display.fillRect(x + i * 5, y + 7 - h, 4, h, c);
    else          display.drawPixel(x + i * 5 + 1, y + 6, c);
  }
}

/* the list: SETTINGS title bar + 4 rows, selected row inverted */
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
      case MI_BRIGHT:
        iconSun(3, y, c);              txtCol(18, y + 2, "Brightness", c);
        miniBar(86, y + 1, briIndex, BRI_COUNT, c);
        break;
      case MI_VOLUME:
        iconSpeaker(2, y, soundOn, c); txtCol(18, y + 2, "Volume", c);
        miniBar(91, y + 1, volIndex, VOL_COUNT, c);
        break;
      case MI_SOUND: {
        iconNote(4, y, c);             txtCol(18, y + 2, "Sound", c);
        const char* m = MODE_NAMES[soundMode];
        txtCol(124 - (int)strlen(m) * 6, y + 2, m, c);
        break;
      }
      case MI_EXIT:
        iconBack(4, y + 1, c);         txtCol(18, y + 2, "Done", c);
        break;
    }
  }
  // gentle pulse on the cursor's right edge so the selection reads as "live"
  if ((f / 10) % 2) display.fillRect(126, 14 + menuCursor * 12 + 3, 2, 4, C_BLACK);
}

/* full-screen level editor for brightness / volume */
void drawLevelEdit(const char* title, uint8_t idx, uint8_t count, uint32_t f) {
  display.fillRect(0, 0, 128, 11, C_WHITE);
  txtCol(4, 2, title, C_BLACK);

  int gap = 2;
  int segW = (112 - (count - 1) * gap) / count;
  int total = count * segW + (count - 1) * gap;
  int x0 = (SCREEN_WIDTH - total) / 2;
  for (uint8_t i = 0; i < count; i++) {
    int x = x0 + i * (segW + gap);
    int h = 5 + i * 22 / count;                    // staircase
    int y = 40 - h;
    if (i <= idx) display.fillRect(x, y, segW, h, C_WHITE);
    else          display.drawRect(x, y, segW, h, C_WHITE);
  }

  char b[12];
  snprintf(b, sizeof(b), "%u / %u", idx + 1, count);
  txtC(44, 1, b);
  if ((f / 16) % 2) txtC(55, 1, "tap = +   hold = ok");
}

void drawSettings(uint32_t f) {
  if (!menuAdjust)                    drawSettingsList(f);
  else if (menuCursor == MI_BRIGHT)   drawLevelEdit("BRIGHTNESS", briIndex, BRI_COUNT, f);
  else                                drawLevelEdit("VOLUME", volIndex, VOL_COUNT, f);
}

/* ============================================================
   SCENES
   ============================================================ */

/* --- idle --------------------------------------------------- */
void sc_idle(uint32_t f) {
  int dx = (int)wobble(f, 0.035f, 5);
  int dy = (int)wobble(f, 0.011f, 2);
  eyes(64 + dx, 26 + dy, 28, 34, 22, EY_NORMAL, 9);
}

/* --- thinking words ----------------------------------------- */
void sc_thinkWords(uint32_t f) {
  eyes(64, 20, 16, 22, 34, EY_NORMAL, 5);
  sparkle(30, 46, f, 3);
  txt(42, 42, 1, "Ruminating");
  for (uint8_t i = 0; i < (f / 8) % 4; i++) txt(108 + i * 4, 42, 1, ".");
}

/* --- thinking (sparkle flies to box) ------------------------ */
void sc_thinking(uint32_t f) {
  eyes(34, 26, 16, 20, 20, EY_NORMAL, 5);
  uint8_t p = f % 40;
  for (uint8_t i = 0; i < 3; i++) {
    int q = (p + i * 6) % 40;
    int x = 58 + q, y = 34 - q / 2;
    if (x < 92) sparkle(x, y, f + i * 5, 2);
  }
  display.drawRoundRect(94, 6, 28, 26, 4, C_WHITE);
  sparkle(108, 19, f, 5);
}

/* --- lightbulb ---------------------------------------------- */
void sc_lightbulb(uint32_t f) {
  eyes(64, 42, 18, 24, 34, EY_NORMAL, 6);
  display.drawCircle(64, 14, 7, C_WHITE);
  display.drawRect(61, 20, 6, 4, C_WHITE);
  display.drawFastHLine(61, 24, 6, C_WHITE);
  int l = 3 + (f / 4) % 3;
  for (uint8_t i = 0; i < 8; i++) {
    float a = i * 45 * PI / 180.0f;
    display.drawLine(64 + cosf(a) * 10, 14 + sinf(a) * 10,
                     64 + cosf(a) * (10 + l), 14 + sinf(a) * (10 + l), C_WHITE);
  }
  txt(16, 10, 1, "+"); txt(108, 16, 1, "+");
}

/* --- reading ------------------------------------------------ */
void sc_reading(uint32_t f) {
  eyes(64, 14, 14, 18, 32, EY_NORMAL, 4);
  for (uint8_t r = 0; r < 4; r++) {
    int y = 34 + r * 7;
    int seg = 0, x = 22;
    while (x < 108) {
      int w = 3 + ((r * 7 + seg * 3 + f / 10) % 9);
      if (x + w > 108) w = 108 - x;
      display.drawFastHLine(x, y, w, C_WHITE);
      x += w + 3; seg++;
    }
  }
}

/* --- typing ------------------------------------------------- */
void sc_typing(uint32_t f) {
  eyes(64, 14, 14, 18, 30, EY_NORMAL, 4);
  txt(10, 32, 1, "return len;");
  txt(16, 42, 1, "if (ok = 8) {");
  txt(10, 52, 1, "// le");
  if ((f / 10) % 2) display.fillRect(46, 51, 5, 8, C_WHITE);
}

/* --- keyboard peek ------------------------------------------ */
void sc_keyboard(uint32_t f) {
  eyes(64, 18, 16, 22, 36, EY_NORMAL, 5);
  const char* k = "asdfjkl;";
  uint8_t hit = (f / 6) % 8;
  for (uint8_t i = 0; i < 8; i++) {
    int x = 10 + i * 14;
    if (i == hit) { display.fillRect(x, 36, 12, 11, C_WHITE);
                    display.setTextColor(C_BLACK); }
    else          { display.drawRect(x, 36, 12, 11, C_WHITE);
                    display.setTextColor(C_WHITE); }
    display.setTextSize(1); display.setCursor(x + 4, 38);
    display.print(k[i]);
    display.setTextColor(C_WHITE);
    display.drawRect(x, 50, 12, 9, C_WHITE);
  }
}


/* --- letters fly -------------------------------------------- */
void sc_letters(uint32_t f) {
  eyes(40, 24, 15, 20, 20, EY_NORMAL, 5);
  display.drawRoundRect(76, 6, 48, 24, 4, C_WHITE);
  display.fillTriangle(78, 30, 86, 30, 76, 36, C_WHITE);
  int n = (f / 8) % 6;
  for (int i = 0; i < n; i++) display.fillRect(82 + i * 7, 16, 4, 6, C_WHITE);
  for (uint8_t i = 0; i < 11; i++) {
    int x = 6 + i * 11 - (int)(f % 11);
    if (x > 2 && x < 118) display.drawRect(x, 48, 8, 8, C_WHITE);
  }
}

/* --- web search --------------------------------------------- */
void sc_webSearch(uint32_t f) {
  eyes(40, 28, 16, 22, 20, EY_NORMAL, 5);
  int cx = 96, cy = 30, r = 14;
  display.drawCircle(cx, cy, r, C_WHITE);
  for (int i = -1; i <= 1; i++) {
    int yy = i * r / 2;
    int w  = (int)sqrtf((float)(r * r - yy * yy));
    display.drawFastHLine(cx - w, cy + yy, 2 * w, C_WHITE);
  }
  float mw = fabsf(cosf(f * 0.07f)) * r;
  for (int y = -r; y <= r; y++) {
    float k = 1.0f - (float)(y * y) / (float)(r * r);
    if (k < 0) k = 0;
    int ww = (int)(mw * sqrtf(k));
    display.drawPixel(cx - ww, cy + y, C_WHITE);
    display.drawPixel(cx + ww, cy + y, C_WHITE);
  }
}

/* --- test tubes --------------------------------------------- */
void sc_testTubes(uint32_t f) {
  eyes(30, 28, 15, 22, 16, EY_NORMAL, 5);
  for (uint8_t i = 0; i < 4; i++) {
    int x = 62 + i * 16;
    display.drawRect(x, 20, 11, 36, C_WHITE);
    int lvl = 10 + (int)(10 * fabsf(sinf(f * 0.07f + i)));
    display.fillRect(x + 2, 54 - lvl, 7, lvl, C_WHITE);
    int by = 16 - (int)((f * 2 + i * 9) % 16);
    display.fillCircle(x + 5, by, 2, C_WHITE);
  }
}

/* --- needs you ---------------------------------------------- */
void sc_needsYou(uint32_t f) {
  eyes(64, 28, 26, 34, 22, EY_WIDE, 8);
  if ((f / 8) % 2) {
    for (int s = -1; s <= 1; s += 2) {
      int x = 64 + s * 56;
      display.fillRect(x - 1, 14, 3, 18, C_WHITE);
      display.fillRect(x - 1, 36, 3, 3, C_WHITE);
    }
  }
}

/* --- context filling up ------------------------------------- */
void sc_context(uint32_t f) {
  eyes(34, 22, 14, 20, 18, EY_NORMAL, 4);
  uint8_t pct = (ctxPct >= 0) ? ctxPct : 40 + (f / 3) % 60;
  char b[18]; snprintf(b, sizeof(b), "context %u%%", pct);
  txt(60, 24, 1, b);
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 14; c++) {
      bool on = (r * 14 + c) < (56 * pct / 100);
      if (on) display.fillRect(58 + c * 5, 36 + r * 6, 4, 5, C_WHITE);
      else    display.drawPixel(60 + c * 5, 38 + r * 6, C_WHITE);
    }
}

/* --- coffee ------------------------------------------------- */
void sc_coffee(uint32_t f) {
  eyes(64, 26, 16, 22, 56, EY_NORMAL, 5);
  display.fillTriangle(50, 34, 78, 34, 74, 50, C_WHITE);
  display.fillTriangle(50, 34, 74, 50, 54, 50, C_WHITE);
  display.drawCircle(80, 39, 5, C_WHITE);
  display.drawFastHLine(46, 52, 36, C_WHITE);
  steam(58, 30, f, 3);
}

/* --- music (^ ^) -------------------------------------------- */
void sc_music(uint32_t f) {
  chevronEyes(64, 26 + (int)wobble(f, 0.18f, 2), 20, false);
  note(22, 12 + (int)wobble(f, 0.12f, 3), 6);
  note(100, 34 + (int)wobble(f, 0.15f, 3), 6);
}

/* --- headbang ----------------------------------------------- */
void sc_headbang(uint32_t f) {
  int dy = (int)(fabsf(sinf(f * 0.22f)) * 7);
  eyes(64, 22 + dy, 22, 30, 26, EY_NORMAL, 7);
  note(106, 16 + (int)wobble(f, 0.2f, 2), 7);
}

/* --- dance -------------------------------------------------- */
void sc_dance(uint32_t f) {
  int dx = (int)wobble(f, 0.18f, 7);
  eyes(64 + dx, 28, 20, 30, 26, EY_NORMAL, 6);
  note(14, 14 + (int)wobble(f, 0.14f, 3), 6);
  note(108, 34 + (int)wobble(f, 0.16f, 3), 6);
}

/* --- equalizer ---------------------------------------------- */
void sc_equalizer(uint32_t f) {
  for (uint8_t i = 0; i < 14; i++) {
    float a = sinf(f * 0.12f + i * 0.9f) + sinf(f * 0.05f + i * 2.1f) * 0.6f;
    int h = 6 + (int)(fabsf(a) * 20);
    if (h > 46) h = 46;
    display.fillRect(4 + i * 9, 52 - h, 7, h, C_WHITE);
  }
  display.drawFastHLine(0, 53, 128, C_WHITE);
}

/* --- now playing -------------------------------------------- */
void sc_nowPlaying(uint32_t f) {
  note(44, 2, 5); txt(54, 4, 1, "music");
  eyes(64, 30, 16, 22, 30, EY_NORMAL, 5);
  for (uint8_t i = 0; i < 16; i++) {
    int h = 2 + (int)(fabsf(sinf(f * 0.15f + i)) * 7);
    display.fillRect(6 + i * 8, 58 - h, 5, h, C_WHITE);
  }
}

/* --- love --------------------------------------------------- */
void sc_love(uint32_t f) {
  int p = (int)wobble(f, 0.18f, 2);
  heart(40, 26, 10 + p); heart(84, 26, 10 + p);
  int fy = 56 - (int)((f * 2) % 40);
  heart(108, fy, 4);
}

/* --- petted (> <) ------------------------------------------- */
void sc_petted(uint32_t f) {
  int dy = (int)wobble(f, 0.15f, 2);
  chevronEyes(64, 26 + dy, 22, true);

  // blush
  for (uint8_t i = 0; i < 3; i++) {
    display.drawLine(30 + i * 4, 45 + dy, 32 + i * 4, 42 + dy, C_WHITE);
    display.drawLine(88 + i * 4, 45 + dy, 90 + i * 4, 42 + dy, C_WHITE);
  }
  // little "w" mouth
  display.drawLine(58, 42 + dy, 61, 46 + dy, C_WHITE);
  display.drawLine(61, 46 + dy, 64, 43 + dy, C_WHITE);
  display.drawLine(64, 43 + dy, 67, 46 + dy, C_WHITE);
  display.drawLine(67, 46 + dy, 70, 42 + dy, C_WHITE);

  // hearts drifting up both sides
  for (uint8_t i = 0; i < 2; i++) {
    int p = (int)((f + i * 22) % 44);
    int x = i ? 116 : 12;
    heart(x + (int)wobble(f + i * 9, 0.2f, 2), 52 - p, p < 30 ? 4 : 3);
  }
}

/* --- juggling ----------------------------------------------- */
void sc_juggling(uint32_t f) {
  eyes(64, 36, 20, 26, 24, EY_HAPPY, 6);
  uint8_t p = f % 36;
  float t = p / 36.0f;
  int x = 44 + (int)(t * 40);
  int y = 24 - (int)(sinf(t * PI) * 18);
  display.fillRect(x, y, 6, 6, C_WHITE);
}

/* --- stargazing --------------------------------------------- */
void sc_stargazing(uint32_t f) {
  for (uint8_t i = 0; i < 18; i++) {
    int x = (i * 37) % 124 + 2;
    int y = (i * 23) % 26 + 2;
    if (((f / 6) + i) % 5) display.drawPixel(x, y, C_WHITE);
  }
  display.fillCircle(110, 10, 6, C_WHITE);
  display.fillCircle(107, 8, 6, C_BLACK);
  eyes(60, 44, 18, 20, 20, EY_HAPPY, 6);
}

/* --- dreaming ----------------------------------------------- */
void sc_dreaming(uint32_t f) {
  eyes(64, 30, 24, 28, 24, EY_SLEEPY, 7);
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t p = (f + i * 14) % 42;
    display.setTextSize(i == 2 ? 2 : 1);
    display.setCursor(96 + i * 6, 26 - p / 3);
    display.print('z');
  }
  display.setTextSize(1);
  sparkle(18, 14, f, 3);
}

/* --- moods -------------------------------------------------- */
void sc_sleepy(uint32_t f) { eyes(64, 30, 28, 30, 22, EY_SLEEPY, 8); }

void sc_happy(uint32_t f) {
  eyes(64, 28 + (int)wobble(f, 0.1f, 2), 30, 32, 22, EY_HAPPY, 9);
}

void sc_angry(uint32_t f) { eyes(64, 28, 30, 32, 20, EY_ANGRY, 8); }

void sc_sad(uint32_t f) {
  eyes(64, 30, 28, 30, 24, EY_SAD, 8);
  int dy = (int)((f * 2) % 20);
  display.fillCircle(46, 44 + dy, 2, C_WHITE);
}

void sc_suspicious(uint32_t f) {
  int dx = ((f / 30) % 2) ? 5 : -5;
  eyes(64 + dx, 28, 30, 34, 20, EY_SUSPICIOUS, 8);
}

void sc_wink(uint32_t f) {
  eyeBody(39, 28, 28, (int)(34 * gBlink), 9);
  display.fillRect(75, 27, 28, 3, C_WHITE);
}

void sc_surprised(uint32_t f) {
  int s = (int)wobble(f, 0.25f, 2);
  eyes(64, 28, 32 + s, 40 + s, 18, EY_WIDE, 12);
}

/* --- terminal ----------------------------------------------- */
void sc_terminal(uint32_t f) {
  winBox(2, 2, 124, 58, "bash");
  txt(8, 16, 1, "$ pytest -q");
  uint8_t n = (f / 7) % 9;
  char dots[10] = {0};
  for (uint8_t i = 0; i < n; i++) dots[i] = (i == 2) ? 'F' : '.';
  txt(8, 28, 1, dots);
  if (n >= 8) txt(8, 40, 1, "1 failed");
  if ((f / 10) % 2) display.fillRect(8, 50, 5, 8, C_WHITE);
}

/* --- searching ---------------------------------------------- */
void sc_searching(uint32_t f) {
  winBox(2, 2, 124, 58, "searching");
  for (uint8_t r = 0; r < 4; r++)
    display.drawFastHLine(10, 18 + r * 8, 40 + ((r * 13 + f / 8) % 30), C_WHITE);
  int mx = 20 + (int)(fabsf(sinf(f * 0.06f)) * 70);
  display.drawCircle(mx, 46, 7, C_WHITE);
  display.drawLine(mx + 5, 51, mx + 11, 57, C_WHITE);
  display.drawRect(108, 20, 12, 18, C_WHITE);            // hourglass
  display.fillTriangle(109, 21, 119, 21, 114, 29, C_WHITE);
  display.fillTriangle(109, 37, 119, 37, 114, 29, C_WHITE);
}

/* --- git graph ---------------------------------------------- */
void sc_gitGraph(uint32_t f) {
  const char* h[5] = { commits[0], commits[1], commits[2], commits[3], commits[4] };
  for (uint8_t i = 0; i < 5; i++) {
    int y = 6 + i * 11;
    display.drawFastVLine(10, y, 11, C_WHITE);
    bool live = (i == (f / 14) % 5);
    if (live) display.fillCircle(10, y, 3, C_WHITE);
    else      display.drawCircle(10, y, 3, C_WHITE);
    txt(20, y - 3, 1, h[i]);
    display.drawFastHLine(70, y, 24, C_WHITE);
  }
  txt(96, 26, 1, "commit");
}

/* --- branch merge ------------------------------------------- */
void sc_branchMerge(uint32_t f) {
  txt(2, 28, 1, "main");
  display.drawFastHLine(30, 32, 86, C_WHITE);
  display.drawLine(40, 32, 52, 14, C_WHITE);
  display.drawFastHLine(52, 14, 30, C_WHITE);
  display.drawLine(82, 14, 96, 32, C_WHITE);
  txt(56, 2, 1, "wip");
  for (uint8_t i = 0; i < 3; i++) display.fillCircle(52 + i * 15, 14, 2, C_WHITE);
  int p = (int)((f * 2) % 86);
  display.fillCircle(30 + p, 32, 3, C_WHITE);
  txt(80, 44, 1, "merged");
}

/* --- packages ----------------------------------------------- */
void sc_packages(uint32_t f) {
  display.drawRect(44, 24, 48, 32, C_WHITE);
  display.drawLine(44, 24, 36, 12, C_WHITE);
  display.drawLine(92, 24, 100, 12, C_WHITE);
  int up = (int)((f * 2) % 18);
  for (uint8_t i = 0; i < 2; i++) {
    int x = 58 + i * 18, y = 48 - up - i * 4;
    display.drawFastVLine(x, y, 8, C_WHITE);
    display.drawLine(x - 3, y + 3, x, y, C_WHITE);
    display.drawLine(x + 3, y + 3, x, y, C_WHITE);
  }
  display.fillRect(100 + (int)wobble(f, 0.1f, 2), 4, 10, 7, C_WHITE);
}

/* --- trophy ------------------------------------------------- */
void sc_trophy(uint32_t f) {
  display.fillRect(52, 12, 24, 14, C_WHITE);
  display.fillTriangle(52, 26, 76, 26, 64, 34, C_WHITE);
  display.drawCircle(48, 18, 5, C_WHITE);
  display.drawCircle(80, 18, 5, C_WHITE);
  display.fillRect(61, 34, 6, 6, C_WHITE);
  display.fillRect(52, 40, 24, 4, C_WHITE);
  sparkle(30, 12, f, 3);
  sparkle(98, 22, f + 10, 3);
  eyes(64, 54, 10, 8, 18, EY_HAPPY, 3, false);
}

/* --- goal --------------------------------------------------- */
void sc_goal(uint32_t f) {
  for (uint8_t c = 0; c < 4; c++)
    for (uint8_t r = 0; r < 8; r++)
      display.drawPixel(100 + c * 6, 16 + r * 5, C_WHITE);
  display.drawRect(98, 12, 24, 40, C_WHITE);
  int bx = (int)((f * 3) % 96);
  display.fillCircle(14 + bx, 36 - (int)(sinf(bx * 0.05f) * 8), 4, C_WHITE);
  eyes(24, 30, 14, 18, 14, EY_HAPPY, 4);
}

/* --- usage -------------------------------------------------- */
/* "2h15" / "45m" / "--" from the minutes left on the 5h window */
void fmtReset(char* b, size_t n) {
  int m = resetMin - (int)((millis() - resetStamp) / 60000UL);
  if (resetMin < 0)  snprintf(b, n, "--");
  else if (m <= 0)   snprintf(b, n, "soon");
  else if (m < 60)   snprintf(b, n, "%dm", m);
  else               snprintf(b, n, "%dh%02d", m / 60, m % 60);
}

void sc_usage(uint32_t f) {
  char b[24], r[10];
  int a = usage5h < 0 ? 0 : usage5h, w = usageWk < 0 ? 0 : usageWk;
  txt(4, 6, 1, "5 hour");
  if (usage5h < 0) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d%%", a);
  txt(124 - (int)strlen(b) * 6, 6, 1, b);
  meter(4, 16, 120, 10, a);
  txt(4, 32, 1, "week");
  if (usageWk < 0) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d%%", w);
  txt(124 - (int)strlen(b) * 6, 32, 1, b);
  meter(4, 42, 120, 10, w);
  if ((f / 12) % 2) {
    fmtReset(r, sizeof(r));
    snprintf(b, sizeof(b), "resets in %s", r);
    txtC(56, 1, b);
  }
}

/* --- limit hit ---------------------------------------------- */
void sc_limitHit(uint32_t f) {
  char b[24], r[10];
  xEyes(64, 26, 11);
  sparkle(64, 6, f, 3);
  fmtReset(r, sizeof(r));
  snprintf(b, sizeof(b), "back in %s", r);
  txtC(48, 1, b);
}

/* --- test failed -------------------------------------------- */
void sc_testFailed(uint32_t f) {
  eyeBody(28, 24, 22, 28, 7);
  if ((f / 8) % 2) {
    for (int t = 0; t < 2; t++) {
      display.drawLine(18, 14 + t, 38, 34 + t, C_BLACK);
      display.drawLine(38, 14 + t, 18, 34 + t, C_BLACK);
    }
  }
  eyeBody(64, 24, 22, (int)(28 * gBlink), 7);
  eyeBody(100, 24, 22, (int)(28 * gBlink), 7);
  txtC(48, 1, "TEST FAILED");
}

/* --- var check ---------------------------------------------- */
void sc_varCheck(uint32_t f) {
  eyes(28, 24, 14, 20, 16, EY_NORMAL, 4);
  display.drawRect(58, 8, 62, 26, C_WHITE);
  txt(78, 16, 2, "VAR");
  display.fillRect(74 + (int)wobble(f, 0.12f, 12), 38, 12, 5, C_WHITE);
  txt(66, 48, 1, "checking");
  for (uint8_t i = 0; i < (f / 8) % 4; i++) txt(114 + i * 3, 48, 1, ".");
}

/* --- wait --------------------------------------------------- */
void sc_wait(uint32_t f) {
  display.drawRoundRect(60, 4, 58, 20, 4, C_WHITE);
  display.fillTriangle(64, 24, 74, 24, 62, 31, C_WHITE);
  txt(66, 10, 1, "wait");
  for (uint8_t i = 0; i < (f / 8) % 4; i++) txt(92 + i * 4, 10, 1, ".");
  eyes(46, 42, 18, 22, 20, EY_NORMAL, 6);
  display.drawFastHLine(0, 58, 128, C_WHITE);
}

/* --- paper plane -------------------------------------------- */
void sc_paperPlane(uint32_t f) {
  int p  = (int)((f * 2) % 150) - 20;
  int py = 30 - (int)(sinf(p * 0.04f) * 8);
  display.drawTriangle(p, py, p + 18, py + 5, p + 2, py + 12, C_WHITE);
  display.drawLine(p, py, p + 7, py + 7, C_WHITE);
  display.drawLine(p + 7, py + 7, p + 18, py + 5, C_WHITE);
  for (int i = 1; i < 7; i++)
    display.drawPixel(p - i * 5,
                      py + 6 + (int)(sinf((p - i * 5) * 0.04f) * 8)
                             - (int)(sinf(p * 0.04f) * 8), C_WHITE);
  display.drawFastHLine(0, 56, 128, C_WHITE);
}

/* --- bug squashed ------------------------------------------- */
void sc_bugSquashed(uint32_t f) {
  bool dead = (f % 60) > 30;
  int cx = 56, cy = 36;
  if (!dead) {
    display.fillRoundRect(cx - 12, cy - 8, 24, 16, 6, C_WHITE);
    for (int s = -1; s <= 1; s += 2)
      for (uint8_t i = 0; i < 3; i++)
        display.drawLine(cx + s * 11, cy - 4 + i * 5,
                         cx + s * 18, cy - 8 + i * 7 + (int)wobble(f, 0.4f, 2), C_WHITE);
    display.drawLine(cx - 4, cy - 8, cx - 8, cy - 15, C_WHITE);
    display.drawLine(cx + 4, cy - 8, cx + 8, cy - 15, C_WHITE);
  } else {
    display.fillRoundRect(cx - 14, cy + 2, 28, 6, 3, C_WHITE);
    for (uint8_t i = 0; i < 6; i++) {
      float a = i * 60 * PI / 180.0f;
      display.drawLine(cx, cy + 4, cx + cosf(a) * 20, cy + 4 + sinf(a) * 8, C_WHITE);
    }
  }
  eyes(104, 26, 10, 14, 10, dead ? EY_HAPPY : EY_NORMAL, 3);
}

/* --- typing along (shown while YOU type) -------------------- */
void sc_typingAlong(uint32_t f) {
  int bob = (int)wobble(f, 0.2f, 1);
  int step = (f / 4) % 2;                       // which paw is pressing

  // head + arms
  display.fillRect(46, 11 + bob, 39, 23, C_WHITE);
  display.fillRect(40, 22 + bob, 51, 7, C_WHITE);
  int eh = (int)(8 * gBlink); if (eh < 1) eh = 1;
  display.fillRect(53, 15 + bob, 3, eh, C_BLACK);
  display.fillRect(76, 15 + bob, 3, eh, C_BLACK);

  // paws (the pressing one drops onto the keys)
  display.fillRect(33, step == 0 ? 34 : 31, 6, 5, C_WHITE);
  display.fillRect(77, step == 1 ? 34 : 31, 6, 5, C_WHITE);

  // keyboard in perspective
  display.drawFastHLine(26, 37, 76, C_WHITE);
  display.drawLine(26, 37, 17, 50, C_WHITE);
  display.drawLine(101, 37, 110, 50, C_WHITE);
  display.drawFastHLine(17, 50, 94, C_WHITE);
  for (uint8_t r = 0; r < 2; r++) {
    int   y  = 40 + r * 5;
    float t  = (y - 37) / 13.0f;
    float L  = 26 - 9 * t + 3, R = 101 + 9 * t - 3;
    float kw = (R - L) / 9.0f;
    uint8_t hit = ((f / 4) * 7 + r * 4) % 9;
    for (uint8_t i = 0; i < 9; i++) {
      int x = (int)(L + i * kw);
      if (r == step && i == hit) display.fillRect(x, y, (int)kw - 1, 3, C_WHITE);
      else                       display.drawRect(x, y, (int)kw - 1, 3, C_WHITE);
    }
  }

  display.drawFastHLine(4, 53, 120, C_WHITE);

  char b[20];
  snprintf(b, sizeof(b), "typing along%.*s", (int)((f / 8) % 4), "...");
  txt(2, 56, 1, b);
}

/* --- football helpers --------------------------------------- */
/* Claude's body: x,y = top-left of the 26x12 body, ground at y+19.
   flip draws the legs on top (bicycle kick); arm lifts the arm band. */
void claudeFB(int x, int y, bool flip, int arm) {
  display.fillRect(x, y, 26, 12, C_WHITE);
  display.fillRect(x - 4, y + 4 + arm, 34, 5, C_WHITE);
  display.fillRect(x + 6,  y + 3, 2, 4, C_BLACK);
  display.fillRect(x + 18, y + 3, 2, 4, C_BLACK);
  int ly = flip ? y - 7 : y + 12;
  display.fillRect(x + 1,  ly, 2, 7, C_WHITE);
  display.fillRect(x + 5,  ly, 2, 7, C_WHITE);
  display.fillRect(x + 19, ly, 2, 7, C_WHITE);
  display.fillRect(x + 23, ly, 2, 7, C_WHITE);
}

void ballFB(int x, int y) {            // black fill so it shows over white
  display.fillCircle(x, y, 3, C_BLACK);
  display.drawCircle(x, y, 3, C_WHITE);
  display.drawPixel(x, y, C_WHITE);
}

void goalNet(int shake) {
  display.drawRect(104 + shake, 24, 24, 30, C_WHITE);
  for (int y = 27; y < 54; y += 4)
    for (int x = 107; x < 126; x += 4) display.drawPixel(x + shake, y, C_WHITE);
}

/* --- goal! -------------------------------------------------- */
void sc_goalCheer(uint32_t f) {
  int dy  = -(int)(fabsf(sinf(f * 0.25f)) * 5);        // jumping for joy
  int arm = ((f / 6) % 2) ? -3 : 0;
  goalNet((f / 3) % 2 ? 1 : 0);
  ballFB(118, 50 - (int)(fabsf(sinf(f * 0.3f)) * 1.5f));
  claudeFB(30, 34 + dy, false, arm);
  if ((f / 10) % 4 != 3) txt(34, 1, 2, "GOAL!");
  sparkle(14, 18, f, 3);
  sparkle(98, 14, f + 7, 3);
  display.drawFastHLine(4, 54, 120, C_WHITE);
}

/* --- bicycle kick ------------------------------------------- */
void sc_bicycleKick(uint32_t f) {
  uint8_t t = f % 90;
  int   dy = 0, bx = 58, by = 4;
  bool  flip = false;

  if (t >= 28 && t <= 46) {                              // the jump
    float u = (t - 28) / 18.0f;
    dy   = -(int)(sinf(u * PI) * 12);
    flip = (u > 0.2f && u < 0.8f);                       // legs go overhead
  }
  if (t < 37) {                                          // ball drops
    by = 4 + (int)(t * 10 / 37);
  } else if (t < 70) {                                   // ball flies to the net
    float k = (t - 37) / 33.0f;
    bx = 58 + (int)(k * 60);
    by = 14 + (int)(k * 36 - sinf(k * PI) * 10);
  } else {                                               // ball in the net
    bx = 118; by = 50;
  }

  goalNet(t >= 65 && t < 78 && (t / 2) % 2 ? 1 : 0);
  claudeFB(46, 34 + dy, flip, flip ? -2 : 0);
  ballFB(bx, by);
  display.drawFastHLine(4, 54, 120, C_WHITE);
}

/* ============================================================
   SCENE TABLE
   capStyle: 0 = none, 1 = sparkle + animated dots, 2 = plain centred
   ============================================================ */
struct Scene { const char* cap; uint8_t capStyle; void (*draw)(uint32_t); };

const Scene SCENES[] = {
  { "idling",          1, sc_idle        },   //  0
  { NULL,              0, sc_thinkWords  },   //  1
  { "thinking",        0, sc_thinking    },   //  2
  { "bright idea",     2, sc_lightbulb   },   //  3
  { "reading",         0, sc_reading     },   //  4
  { "typing",          0, sc_typing      },   //  5
  { "keyboard peek",   0, sc_keyboard    },   //  6
  { "letters fly",     0, sc_letters     },   //  7
  { "web search",      2, sc_webSearch   },   //  8
  { "test tubes",      0, sc_testTubes   },   //  9
  { "needs you",       2, sc_needsYou    },   // 10
  { NULL,              0, sc_context     },   // 11
  { "coffee",          2, sc_coffee      },   // 12
  { "music",           2, sc_music       },   // 13
  { "headbang",        2, sc_headbang    },   // 14
  { "dance",           2, sc_dance       },   // 15
  { "equalizer",       2, sc_equalizer   },   // 16
  { NULL,              0, sc_nowPlaying  },   // 17
  { "love",            2, sc_love        },   // 18
  { "petted",          2, sc_petted      },   // 19
  { "juggling",        0, sc_juggling    },   // 20
  { "stargazing",      0, sc_stargazing  },   // 21
  { "dreaming",        0, sc_dreaming    },   // 22
  { "low power",       2, sc_sleepy      },   // 23
  { "feeling great",   2, sc_happy       },   // 24
  { "not amused",      2, sc_angry       },   // 25
  { "oh no",           2, sc_sad         },   // 26
  { "hmmmm",           2, sc_suspicious  },   // 27
  { "got it",          2, sc_wink        },   // 28
  { "whoa",            2, sc_surprised   },   // 29
  { NULL,              0, sc_terminal    },   // 30
  { NULL,              0, sc_searching   },   // 31
  { "git graph",       0, sc_gitGraph    },   // 32
  { NULL,              0, sc_branchMerge },   // 33
  { "packages",        2, sc_packages    },   // 34
  { "trophy",          0, sc_trophy      },   // 35
  { "goal",            2, sc_goal        },   // 36
  { NULL,              0, sc_usage       },   // 37
  { NULL,              0, sc_limitHit    },   // 38
  { NULL,              0, sc_testFailed  },   // 39
  { NULL,              0, sc_varCheck    },   // 40
  { NULL,              0, sc_wait        },   // 41
  { "paper plane",     2, sc_paperPlane  },   // 42
  { "bug squashed",    2, sc_bugSquashed },   // 43
  { NULL,              0, sc_typingAlong },   // 44
  { "goal",            2, sc_goalCheer   },   // 45
  { "bicycle kick",    2, sc_bicycleKick },   // 46
};
const uint8_t SCENE_COUNT = sizeof(SCENES) / sizeof(SCENES[0]);

/* ============================================================
   CAPTION  (plain types only — no custom struct in signature)
   ============================================================ */
void drawCaption(const char* cap, uint8_t capStyle, uint32_t f) {
  if (capStyle == 0 || cap == NULL) return;
  if (capStyle == 2) { txtC(56, 1, cap); return; }

  char buf[26];
  uint8_t dots = (f / 8) % 4;
  snprintf(buf, sizeof(buf), "%s%.*s", cap, dots, "...");
  int blockW = (int)strlen(buf) * 6 + 14;
  int x0 = (SCREEN_WIDTH - blockW) / 2; if (x0 < 0) x0 = 0;
  sparkle(x0 + 5, 57, f, 4);
  txt(x0 + 14, 54, 1, buf);
}

/* ============================================================
   BLINK ENGINE
   ============================================================ */
void scheduleBlink() { nextBlink = millis() + random(2200, 6200); }

void updateBlink() {
  uint32_t now = millis();
  if (!blinking && now > nextBlink) {
    blinking = true; blinkStart = now;
    if (blinkQueue == 0 && random(100) < 25) blinkQueue = 1;   // double blink
  }
  if (!blinking) { gBlink = 1.0f; return; }

  uint32_t t = now - blinkStart;
  if (t >= BLINK_DUR) {
    blinking = false;
    if (blinkQueue) { blinkQueue--; blinking = true; blinkStart = now; }
    else scheduleBlink();
    gBlink = 1.0f;
    return;
  }
  gBlink = fabsf(cosf((float)t / BLINK_DUR * PI));
}

/* ============================================================
   SCENE CONTROL
   ============================================================ */
void setScene(uint8_t i) {
  gameMode = false;                    // any scene change leaves the game
  scene = i % SCENE_COUNT;
  sceneSince = millis();
  Serial.printf("scene %u\n", scene);
}

/* ============================================================
   GAME — the face jumps over bugs. Tap = jump.
   ============================================================ */
void startGame() {
  gameMode = true; gameOver = false; gNewHi = false;
  gameTick = 0; gScore = 0;
  gY = 0; gVy = 0; gSpeed = 2.6f; gScroll = 0; gNextGap = 60;
  for (uint8_t i = 0; i < 3; i++) obs[i].on = false;
  petting = false; menuOpen = false; tapCount = 0;
  touchPressed = touchReleased = false;
  Serial.println("game start");
}

void exitGame() {
  gameMode = false;
  sceneSince = millis();
  Serial.println("game exit");
}

void endGame() {
  gameOver = true;
  gameOverAt = millis();
  if (gScore > gHi) {
    gHi = gScore; gNewHi = true;
    prefs.begin("face", false);
    prefs.putUShort("hi", gHi);
    prefs.end();
  }
  if (gNewHi) sndHiScore(); else sndOver();
  Serial.printf("game over: %u\n", gScore);
}

void gameStep() {
  uint32_t now = millis();

  if (gameOver) {
    if (touchState && now - touchDown > 700) { exitGame(); return; }       // hold
    if (touchReleased && now - gameOverAt > 400 && now - touchDown < 700)  // tap
      startGame();
    return;
  }

  if (touchPressed && gY < 0.5f) { gVy = 6.4f; sndJump(); }   // jump
  gY += gVy;
  gVy -= 0.55f;
  if (gY <= 0) { gY = 0; gVy = 0; }

  // spawn
  float farX = -999;
  for (uint8_t i = 0; i < 3; i++) if (obs[i].on && obs[i].x + obs[i].w > farX) farX = obs[i].x + obs[i].w;
  if (farX < SCREEN_WIDTH - gNextGap) {
    for (uint8_t i = 0; i < 3; i++) if (!obs[i].on) {
      obs[i].on = true; obs[i].x = SCREEN_WIDTH;
      obs[i].w = random(6, 11); obs[i].h = random(8, 17);
      gNextGap = random(56, 100) + gSpeed * 10;
      break;
    }
  }

  // move + collide
  for (uint8_t i = 0; i < 3; i++) {
    if (!obs[i].on) continue;
    obs[i].x -= gSpeed;
    if (obs[i].x + obs[i].w < 0) { obs[i].on = false; continue; }
    bool hitX = obs[i].x < G_PX + G_PW - 2 && obs[i].x + obs[i].w > G_PX + 2;
    if (hitX && gY < obs[i].h - 2) { endGame(); return; }
  }

  gScroll += gSpeed;
  if (gSpeed < 5.5f) gSpeed += 0.0015f;
  gameTick++;
  gScore = gameTick / 4;
}

void gameDraw() {
  char b[16];
  display.drawFastHLine(0, G_GROUND + 1, SCREEN_WIDTH, C_WHITE);
  for (uint8_t i = 0; i < 6; i++)
    display.drawPixel(((int)(i * 29 + 128 - ((int)gScroll % 128))) % 128, G_GROUND + 4, C_WHITE);

  // obstacles = bugs
  for (uint8_t i = 0; i < 3; i++) {
    if (!obs[i].on) continue;
    int x = (int)obs[i].x, y = G_GROUND - obs[i].h;
    display.fillRect(x, y, obs[i].w, obs[i].h, C_WHITE);
    display.drawLine(x + 1, y, x - 2, y - 3, C_WHITE);
    display.drawLine(x + obs[i].w - 2, y, x + obs[i].w + 1, y - 3, C_WHITE);
  }

  // player
  int py = G_GROUND - (int)gY - G_PH;
  display.fillRect(G_PX, py, G_PW, G_PH, C_WHITE);
  if (gameOver) {
    display.drawLine(G_PX + 2, py + 3, G_PX + 5, py + 7, C_BLACK);
    display.drawLine(G_PX + 5, py + 3, G_PX + 2, py + 7, C_BLACK);
    display.drawLine(G_PX + 7, py + 3, G_PX + 10, py + 7, C_BLACK);
    display.drawLine(G_PX + 10, py + 3, G_PX + 7, py + 7, C_BLACK);
  } else {
    display.fillRect(G_PX + 3, py + 3, 2, 5, C_BLACK);
    display.fillRect(G_PX + 8, py + 3, 2, 5, C_BLACK);
  }

  snprintf(b, sizeof(b), "%05u", gScore);
  txt(SCREEN_WIDTH - 32, 2, 1, b);
  snprintf(b, sizeof(b), "HI %u", gHi);
  txt(2, 2, 1, b);

  if (gameOver) {
    display.fillRect(12, 14, 104, 38, C_BLACK);
    display.drawRect(12, 14, 104, 38, C_WHITE);
    txtC(18, 1, gNewHi ? "NEW HIGH SCORE" : "GAME OVER");
    snprintf(b, sizeof(b), "score %u", gScore);
    txtC(30, 1, b);
    if ((tick / 16) % 2) txtC(41, 1, "tap=retry hold=exit");
  }
}

/* ============================================================
   LIVE EVENTS  (E:<name> lines from the laptop)
   ============================================================ */
struct Ev { const char* name; uint8_t scene; };
const Ev EVENTS[] = {
  { "idle", 0 },  { "think", 2 },  { "read", 4 },   { "type", 5 },
  { "search", 31 }, { "web", 8 },  { "bash", 30 },  { "git", 32 },
  { "test", 9 },  { "need", 10 },  { "done", 35 },  { "fail", 39 },
  { "merge", 33 }, { "wait", 41 }, { "bug", 43 },   { "plane", 42 },
  { "compact", 11 }, { "pkg", 34 }, { "key", 44 },
  { "goal", 45 }, { "kick", 46 },
};
const uint8_t EVENT_COUNT = sizeof(EVENTS) / sizeof(EVENTS[0]);

void onEvent(const char* name) {
  uint32_t now = millis();
  static uint32_t lastNeed = 0, lastHm = 0, lastDone = 0;
  live = true; lastEvent = now; autoCycle = false; followStage = 0;

  if (!strcmp(name, "need")) {                     // Claude needs you: leave the game
    gameMode = false;
    if (now - lastNeed > 10000) { sndNeed(); lastNeed = now; }
  }
  if (gameMode) return;                            // don't interrupt a game otherwise

  // the voice only speaks up for the moments that matter
  if (!strcmp(name, "think") && now - lastHm > 8000) { sndHm(); lastHm = now; }
  else if (!strcmp(name, "fail"))                    sndFail();
  else if (!strcmp(name, "bug"))                     sndYay();
  else if (!strcmp(name, "goal"))                    sndGoal();

  if (!strcmp(name, "done")) {
    if (now - lastDone > 5000) { sndDone(); lastDone = now; }
    setScene(usage5h >= 100 ? 38 : 35);
    followStage = 1; followAt = now;
    return;
  }
  for (uint8_t i = 0; i < EVENT_COUNT; i++)
    if (!strcmp(name, EVENTS[i].name)) {
      if (scene != EVENTS[i].scene) setScene(EVENTS[i].scene);
      sceneSince = now;
      return;
    }
}

void pushCommit(const char* h) {
  for (uint8_t i = 4; i > 0; i--) memcpy(commits[i], commits[i - 1], 8);
  strncpy(commits[0], h, 7);
  commits[0][7] = 0;
}

void handleLine(char* line) {
  if (line[1] != ':') return;
  char* v = line + 2;
  switch (line[0]) {
    case 'E': onEvent(v); break;
    case 'U': {
      int a = -1, w = -1, m = -1;
      sscanf(v, "%d,%d,%d", &a, &w, &m);
      usage5h = constrain(a, -1, 100); usageWk = constrain(w, -1, 100);
      resetMin = constrain(m, -1, 32000); resetStamp = millis();
      if (a >= 100 && !gameMode && live) setScene(38);
      break;
    }
    case 'X': ctxPct = constrain(atoi(v), 0, 100); break;
    case 'C': pushCommit(v); onEvent("git"); if (!gameMode) sndCommit(); break;
    case 'G': if (atoi(v)) startGame(); else if (gameMode) exitGame(); break;
  }
}

/* ============================================================
   SETTINGS MENU  (one button: tap = next / step, hold = choose / done)
   ============================================================ */
void openMenu() {
  menuOpen = true; menuAdjust = false; menuCursor = 0;
  menuSince = millis();
  petting = false;
  sndUiOpen();
}

void closeMenu() {
  menuOpen = false; menuAdjust = false;
  saveSettings();
  sceneSince = millis();
  sndUiClose();
  Serial.printf("settings saved: bri %u/%u  vol %u/%u  sound %s\n",
                briIndex + 1, BRI_COUNT, volIndex + 1, VOL_COUNT, MODE_NAMES[soundMode]);
}

void menuTap() {
  menuSince = millis();
  if (!menuAdjust) {
    menuCursor = (menuCursor + 1) % MENU_ITEMS;
    sndUiTick();
  } else if (menuCursor == MI_BRIGHT) {
    briIndex = (briIndex + 1) % BRI_COUNT;
    applyBrightness();
    sndUiTick();
  } else {
    volIndex = (volIndex + 1) % VOL_COUNT;
    sndPreview();                                   // hear the new level
  }
}

void menuHold() {
  menuSince = millis();
  if (menuAdjust) { menuAdjust = false; sndUiSelect(); return; }
  switch (menuCursor) {
    case MI_BRIGHT:
    case MI_VOLUME:
      menuAdjust = true;
      sndUiSelect();
      break;
    case MI_SOUND:                                  // ALL -> VOICE -> OFF -> ALL
      setSoundMode(soundMode == SND_ALL ? SND_VOICE : soundMode == SND_VOICE ? SND_OFF : SND_ALL);
      sndClear();
      if (soundOn) sndPushV(1200, 1800, 50, 0, 60);
      break;
    case MI_EXIT:
      closeMenu();
      break;
  }
}

void drawFrame() {
  updateBlink();
  display.clearDisplay();

  if (menuOpen) {
    drawSettings(tick);
  } else if (gameMode) {
    gameStep();
    if (gameMode) gameDraw();          // gameStep may have exited
  } else if (petting) {                // hold overrides the active scene
    sc_petted(tick);
    txtC(56, 1, "petted");
  } else {
    SCENES[scene].draw(tick);
    drawCaption(SCENES[scene].cap, SCENES[scene].capStyle, tick);
    sceneSfx(scene, tick);
  }

  drawToast();
  touchPressed = touchReleased = false;
  display.display();
}

/* ============================================================
   TOUCH HANDLING
     tap        -> next face        (in settings: next item / step up)
     double tap -> open settings
     triple tap -> game     4 taps -> quick mute
     hold       -> petted face      (in settings: choose / done)
   Inside settings taps act instantly (no multi-tap wait).
   ============================================================ */
void handleTouch() {
  uint32_t now = millis();
  bool raw = digitalRead(TOUCH_PIN);

  if (raw != touchRaw) { touchRaw = raw; touchChange = now; }

  if (now - touchChange > DEBOUNCE_MS && touchState != touchRaw) {
    touchState = touchRaw;

    if (touchState) {                  // finger down
      touchDown = now;
      touchPressed = true;
    } else {                           // finger up
      touchReleased = true;
      if (gameMode)          { }       // the game reads the edges itself
      else if (petting)      { petting = false; sceneSince = now; sndPetEnd(); }
      else if (holdConsumed) { holdConsumed = false; }
      else if (menuOpen)     { menuTap(); }
      else                   { tapCount++; lastTap = now; }
    }
  }

  // hold threshold passed
  uint32_t holdMs = menuOpen ? MENU_HOLD_MS : PET_HOLD_MS;
  if (touchState && !gameMode && !petting && !holdConsumed && now - touchDown > holdMs) {
    if (menuOpen) { menuHold(); holdConsumed = true; }
    else          { petting = true; tapCount = 0; sndPetStart(); }
  }

  // resolve taps once the multi-tap window closes
  if (tapCount && now - lastTap > TAP_WINDOW_MS) {
    if (tapCount == 1) {
      setScene(scene + 1);
      autoCycle = false;
    } else if (tapCount == 2) {
      openMenu();                      // double tap opens settings
    } else if (tapCount == 3) {
      startGame();                     // triple tap
    } else {
      toggleSound();                   // 4 taps
    }
    tapCount = 0;
  }

  // menu auto-close
  if (menuOpen && now - menuSince > MENU_TIMEOUT_MS) closeMenu();
}

void handleKey(char c) {
  if      (c == 'n') { setScene(scene + 1);               autoCycle = false; live = false; }
  else if (c == 'p') { setScene(scene + SCENE_COUNT - 1); autoCycle = false; live = false; }
  else if (c == 'r') { setScene(random(SCENE_COUNT));     autoCycle = false; live = false; }
  else if (c == 'c') { autoCycle = !autoCycle; sceneSince = millis(); }
  else if (c == '+') { if (briIndex < BRI_COUNT - 1) briIndex++; applyBrightness(); saveSettings(); }
  else if (c == '-') { if (briIndex > 0)             briIndex--; applyBrightness(); saveSettings(); }
  else if (c == ']') { if (volIndex < VOL_COUNT - 1) volIndex++; sndPreview(); saveSettings(); }
  else if (c == '[') { if (volIndex > 0)             volIndex--; sndPreview(); saveSettings(); }
  else if (c == 'b') { if (!menuOpen) openMenu(); }
  else if (c == 'g') { startGame(); }
  else if (c == 'q') { if (gameMode) exitGame(); }
  else if (c == 's') { toggleSound(); }
  else if (c == 'm') {
    setSoundMode(soundMode == SND_ALL ? SND_VOICE : soundMode == SND_VOICE ? SND_OFF : SND_ALL);
    saveSettings(); toastUntil = millis() + 1500;
  }
}

/* Upper-case first char starts a protocol line ("E:think\n");
   anything else is a single-key command, as before. */
char    lineBuf[40];
uint8_t lineLen = 0;
bool    inLine  = false;

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (inLine) {
      if (c == '\n' || c == '\r') {
        lineBuf[lineLen] = 0;
        if (lineLen > 2) handleLine(lineBuf);
        inLine = false; lineLen = 0;
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    } else if (c >= 'A' && c <= 'Z') {
      inLine = true; lineLen = 0; lineBuf[lineLen++] = c;
    } else {
      handleKey(c);
    }
  }
}

/* live mode follow-ups and timeout */
void updateLive() {
  uint32_t now = millis();
  if (followStage == 1 && now - followAt > 5000 && !gameMode) {
    followStage = 2; followAt = now;
    setScene(usage5h >= 0 ? 37 : 0);
  } else if (followStage == 2 && now - followAt > 6000 && !gameMode) {
    followStage = 0;
    setScene(0);
  }
  if (live && now - lastEvent > LIVE_TIMEOUT_MS) {
    live = false; autoCycle = true; sceneSince = now;
  }
}

void bootSplash() {
  for (int h = 0; h <= 34; h += 2) {
    display.clearDisplay();
    eyes(64, 26, 28, h, 22, EY_NORMAL, 9, false);
    txtC(54, 1, "waking up");
    display.display();
    delay(18);
  }
  delay(300);
}

/* ============================================================
   SETUP / LOOP
   ============================================================ */
void setup() {
  Serial.begin(115200);

  pinMode(TOUCH_PIN, INPUT);              // TTP223 drives it HIGH on touch

  Wire.begin(I2C_SDA, I2C_SCL);           // SDA = GPIO5, SCL = GPIO4
  Wire.setClock(400000);
  delay(120);

  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED not found - check wiring/address");
    while (true) delay(1000);
  }
  display.setRotation(0);                 // use 2 if mounted upside down

  prefs.begin("face", true);              // read-only
  briIndex = prefs.getUChar("bri", 5);
  volIndex = prefs.getUChar("vol", 3);
  uint8_t m = prefs.getUChar("sndm", SND_ALL);
  gHi      = prefs.getUShort("hi", 0);
  prefs.end();
  if (briIndex >= BRI_COUNT) briIndex = 5;
  if (volIndex >= VOL_COUNT) volIndex = 3;
  setSoundMode(m > SND_ALL ? SND_ALL : m);
  applyBrightness();
  buzzInit();

  display.clearDisplay();
  display.display();

  randomSeed(esp_random());
  bootSplash();
  scheduleBlink();
  setScene(0);
  sndHello();
  Serial.printf("%u scenes. tap=next, 2x=settings, 3x=game, 4x=mute, hold=pet.\n",
                SCENE_COUNT);
}

void loop() {
  handleTouch();
  handleSerial();
  updateLive();
  petUpdate();                            // sound itself plays in soundTask

  if (autoCycle && !petting && !menuOpen && !gameMode && millis() - sceneSince > SCENE_MS)
    setScene(scene + 1);

  uint32_t now = millis();
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    tick++;
    drawFrame();
  }
}

/* =============================================================
   TUNING
     EYE_RADIUS      0 = square corners (current), 8 = rounded
     SCENE_MS        dwell time per scene when auto-cycling
     PET_HOLD_MS     how long a press must last to count as a hold
     TAP_WINDOW_MS   gap that separates a single tap from a double
     BRI_LEVELS[]    the brightness steps the menu walks through

   SSD1306 SWAP
     #include <Adafruit_SSD1306.h>
     Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
     #define C_WHITE SSD1306_WHITE
     #define C_BLACK SSD1306_BLACK
     display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
     // and replace setBrightness() with display.ssd1306_command(0x81) + level

   ADDING YOUR OWN SCENE
     1. void sc_myThing(uint32_t f) { ... }        // f = frame counter
     2. add  { "my caption", 2, sc_myThing },  to SCENES[]
     Cycling, blinking, touch and serial control are automatic.
   ============================================================= */