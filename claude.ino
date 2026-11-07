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

   ON ITS OWN (no laptop, or nobody touching it for 45 s)
     A "life director" picks what Claude does: moods, work scenes,
     naps, and self-playing games (build crane, flappy clawd,
     permission panic, whack a bug). After a game it reacts — happy
     or a trophy if it did well, sad or grumpy if it lost.
     Every scene has its own quiet sound (Sound mode ALL).

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
     T:<text>       what Claude is doing now ("editing app.py")
     R:<branch>,<repo>          L:<0-4>,<hash>,<subject>
                    your repo's branch and last 5 commits (git graph)
     S:<commits>,<tasks>,<tests>,<cents>,<added>,<removed>,<minutes>
                    today's stats + this session's cost/lines/time
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
#define RESUME_MS   45000    // no input this long -> Claude goes back to living on its own
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

bool     autoCycle  = true;       // the "life" director picks scenes on its own
uint8_t  scene      = 0;
uint32_t sceneSince = 0;
uint32_t sceneTick0 = 0;          // tick when the current scene started
uint32_t sceneDur   = 5000;       // how long the director keeps this scene
uint32_t lastInput  = 0;          // last touch / serial key, to resume autonomy

/* self-playing games report how they're doing to the director */
int8_t   playLives  = -1;
uint16_t playScore  = 0;
bool     playOver   = false;

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
/* your repo's real history (L: / R: lines from the bridge), newest first */
bool     gitKnown      = false;
char     gHash[5][8]   = { "", "", "", "", "" };
char     gSubj[5][20]  = { "", "", "", "", "" };
char     gitBranch[12] = "";
char     gitRepo[20]   = "";
uint32_t commitAt      = 0;           // when the newest commit arrived (row flashes)

uint8_t  followStage = 0;             // after "done": trophy -> usage -> idle
uint32_t followAt    = 0;

char     ticker[44]  = "";            // what Claude is doing right now (T:)
uint32_t tickerAt    = 0;

/* a Claude turn in progress: from your prompt until Claude finishes */
bool     busy        = false;
uint32_t busyStart   = 0;
uint32_t busyDur     = 0;             // how long the last turn took
uint32_t lastToolAt  = 0;             // last real tool event in this turn
uint8_t  verbIdx     = 0;
const char* VERBS[] = { "Enchanting", "Pondering", "Brewing", "Cogitating", "Conjuring",
                        "Musing", "Noodling", "Percolating", "Ruminating", "Tinkering",
                        "Crafting", "Mulling", "Simmering", "Wrangling", "Manifesting",
                        "Synthesizing", "Spelunking", "Vibing", "Baking", "Forging" };
const uint8_t VERB_COUNT = sizeof(VERBS) / sizeof(VERBS[0]);
#define  SPIN_AFTER_MS  3500          // no tool this long -> back to the spinner

bool     statsKnown  = false;         // today's stats (S:) from the bridge
uint16_t stCommits = 0, stTasks = 0, stTests = 0, stAdded = 0, stRemoved = 0, stMin = 0;
uint32_t stCostC   = 0;               // session cost in cents

/* ============================================================
   XP & LEVELS — Claude grows when you work together
   commit +10, tests pass +6, task done +4, petting +1 (once a minute)
   ============================================================ */
uint32_t xp           = 0;
uint8_t  level        = 0;
bool     levelPending = false;        // show the level-up scene when possible
uint32_t lastPetXp    = 0;
const char* TITLES[] = { "hatchling", "intern", "junior dev", "developer", "senior dev",
                         "staff eng", "principal", "architect", "wizard", "legend" };

uint32_t xpNeed(uint8_t L) { return 15UL * L * (L + 1); }     // total xp to reach level L
const char* levelTitle() { return TITLES[level < 9 ? level : 9]; }

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

const uint16_t VOL_LEVELS[] = { 30, 60, 110, 180, 270, 380, 512 };   // PWM duty of 1023 (512 = max)
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

/* LOUDER, no wiring: small piezos are loudest around 2-4 kHz, so every
   sound is lifted by PITCH_LIFT (1.0 = original, softer pitch).        */
#define PITCH_LIFT  1.6f

/* LOUDER, one wire: put the buzzer between GPIO6 and BUZZER_PIN2 (instead
   of GND). That pin plays the inverted signal, doubling the voltage across
   the buzzer (~4x power). -1 = off (buzzer's other leg on GND).          */
#define BUZZER_PIN2 -1

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
  #if BUZZER_PIN2 >= 0
    ledcAttachChannel(BUZZER_PIN,  1000, BUZZ_BITS, 0);   // both pins on one channel,
    ledcAttachChannel(BUZZER_PIN2, 1000, BUZZ_BITS, 0);   // the second one inverted
    ledcOutputInvert(BUZZER_PIN2, true);
  #else
    ledcAttach(BUZZER_PIN, 1000, BUZZ_BITS);
  #endif
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
  if (sqLen == 0) { sqHead = 0; sylStart = millis(); }   // reuse the buffer from the start
  if (sqHead + sqLen < 24) {
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
    f *= (1.0f + 0.012f * sinf(t * 0.07f)) * PITCH_LIFT;   // vibrato + loudness lift
    // soft attack / release, shortened for tiny clicks
    uint16_t att = s.dur / 4 < 8  ? s.dur / 4 : 8;
    uint16_t rel = s.dur / 3 < 14 ? s.dur / 3 : 14;
    uint32_t rem = s.dur - t;
    float env = 1.0f;
    if (att && t < att) env = (float)t / att;
    if (rel && rem < rel && (float)rem / rel < env) env = (float)rem / rel;
    // quiet effects (v 12-45 %) are lifted so they're still clearly audible
    float vol = 0.45f + 0.55f * s.v / 100.0f;
    buzzOut((uint16_t)f, (uint16_t)(VOL_LEVELS[volIndex] * env * vol) + 1);
  } else if (t < (uint32_t)s.dur + s.gap) {
    buzzOut(0, 0);
  } else {
    sqHead++; sqLen--;
    if (sqLen == 0) sqHead = 0;
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
void sndLevelUp() {
  if (!soundOn) return;
  static const uint16_t n[] = { 784, 988, 1175, 1568 };
  sndClear();
  for (uint8_t i = 0; i < 4; i++) sndPushV(n[i], n[i], 70, 20, 90);
  sndPushV(1568, 2093, 220, 0, 100);
}

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

/* game effects: these matter, so they cut in instead of waiting */
void fx(uint16_t f0, uint16_t f1, uint16_t dur, uint8_t v) {
  if (soundMode != SND_ALL) return;
  sndClear(); sndPushV(f0, f1, dur, 0, v);
}
void fx2(uint16_t a, uint16_t b, uint16_t dur, uint8_t v) {
  if (soundMode != SND_ALL) return;
  sndClear(); sndPushV(a, a, dur, dur / 2, v); sndPushV(b, b, dur + 20, 0, v);
}

/* tap to change face: a little two-note "pip-pip", pitched per scene */
void sndTap() {
  if (!soundOn) return;
  uint16_t b = (uint16_t)(988 * SCALE[scene % 5]);
  sndClear();
  sndPushV(b, b, 30, 15, 45);
  sndPushV((uint16_t)(b * 1.5f), (uint16_t)(b * 1.5f), 45, 0, 40);
}

/* the director moving to a new scene on its own: a softer chime */
void sndEnter() {
  if (!ambOk()) return;
  uint16_t b = (uint16_t)(880 * SCALE[random(5)]);
  sndPushV(b, (uint16_t)(b * 1.25f), 40, 0, 22);
}

void sceneSfx(uint8_t sc, uint32_t f) {
  if (soundMode != SND_ALL) return;
  switch (sc) {
    case 0:                                               // idle: a little murmur now and then
      if (f % 160 == 0 && ambOk()) {
        uint16_t b = (uint16_t)(760 * SCALE[random(5)]);
        sndPushV(b, b + 60, 60, 50, 22); sndPushV(b + 40, b - 40, 80, 0, 18);
      }
      break;
    case 7:  if (f % 8 == 0 && (f / 8) % 6) amb(1500, 1500, 10, 22); break;    // letters pop in
    case 10: if (f % 16 == 0) amb(1760, 1568, 40, 30); break;          // needs you: alert
    case 11: if (f % 15 == 0) amb(1200, 1200, 8, 15); break;           // context ticking up
    case 12:                                              // coffee: sip-sip
      if (f % 70 == 0 && ambOk()) { sndPushV(600, 500, 60, 40, 20); sndPushV(620, 480, 60, 0, 18); }
      break;
    case 19: if (f % 80 == 0) amb(1100, 950, 120, 30); break;          // petted face: coo
    case 24: if (f % 100 == 0) amb(1319, 1568, 60, 30); break;         // feeling great
    case 25:                                              // not amused: grumble
      if (f % 90 == 0 && ambOk()) { sndPushV(500, 430, 120, 30, 28); sndPushV(470, 400, 120, 0, 24); }
      break;
    case 26: if (f % 20 == 0) amb(1800, 1400, 20, 15); break;          // tears: plink
    case 27: if (f % 30 == 0) amb(600, 560, 150, 22); break;           // hmmmm
    case 28: if (f % 100 == 0) amb(2093, 2093, 60, 25); break;         // wink: ting
    case 29: if (f % 120 == 0) amb(600, 1400, 200, 28); break;         // whoa!
    case 33: if (f % 43 == 42 && ambOk()) { sndPushV(1319, 1319, 40, 20, 28); sndPushV(1760, 1760, 70, 0, 28); } break;   // merged
    case 34: if (f % 18 == 0) amb(1000, 1300, 20, 18); break;          // packages pop
    case 35:                                              // trophy fanfare
      if (f % 80 == 0 && ambOk()) {
        sndPushV(1047, 1047, 50, 15, 30); sndPushV(1319, 1319, 50, 15, 30); sndPushV(1568, 1568, 90, 0, 30);
      }
      break;
    case 37: if (f % 96 == 0) amb(1600, 1600, 10, 15); break;          // usage meter tick
    case 38: if (f % 120 == 0) amb(800, 500, 200, 25); break;          // limit hit
    case 39: if (f % 16 == 0) amb(500, 420, 80, 28); break;            // test failed
    case 40: case 41: if (f % 8 == 0) amb(1300, 1300, 8, 14); break;   // checking / waiting dots
    case 1: case 2: case 54:                              // thinking: soft twinkle
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
    case 32: if (f % 40 == 0) amb(1760, 1760, 15, 18); break;           // git HEAD pulse
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
/* like the editor's Graph panel: branch + repo, then the last 5 commits */
void sc_gitGraph(uint32_t f) {
  if (!gitKnown) {
    for (uint8_t i = 0; i < 4; i++) {                    // empty, gently pulsing graph
      int y = 8 + i * 12;
      if (i < 3) display.drawFastVLine(6, y, 12, C_WHITE);
      if (i == (f / 20) % 4) display.fillCircle(6, y, 2, C_WHITE);
      else                   display.drawCircle(6, y, 2, C_WHITE);
    }
    txt(18, 14, 1, "no git repo yet");
    txt(18, 28, 1, "start the bridge");
    txt(18, 40, 1, "& commit away!");
    return;
  }

  // branch pill + repo name
  const char* br = gitBranch[0] ? gitBranch : "HEAD";
  int bw = (int)strlen(br) * 6 + 13;
  display.fillRoundRect(0, 0, bw, 10, 4, C_WHITE);
  display.drawCircle(5, 4, 2, C_BLACK);
  txtCol(10, 1, br, C_BLACK);
  txt(bw + 4, 1, 1, gitRepo);

  bool fresh = millis() - commitAt < 4000;
  for (uint8_t i = 0; i < 5; i++) {
    if (!gHash[i][0]) continue;
    int y = 17 + i * 10;
    if (i < 4 && gHash[i + 1][0]) display.drawFastVLine(4, y, 10, C_WHITE);
    if (i == 0) {                                        // HEAD: hollow ring, pulsing
      display.fillCircle(4, y, 3, C_BLACK);
      display.drawCircle(4, y, 3, C_WHITE);
      if ((f / 20) % 2) display.fillCircle(4, y, 1, C_WHITE);
    } else {
      display.fillCircle(4, y, 2, C_WHITE);
    }
    const char* s = gSubj[i][0] ? gSubj[i] : gHash[i];
    if (i == 0 && fresh && (f / 4) % 2) {                // brand-new commit flashes
      display.fillRect(10, y - 4, (int)strlen(s) * 6 + 3, 9, C_WHITE);
      txtCol(12, y - 3, s, C_BLACK);
    } else {
      txt(12, y - 3, 1, s);
    }
  }
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
  if (!flip) hatFor(x, y);
}

/* hats unlock as Claude levels up: party hat (2), crown (5), wizard (8) */
void hatFor(int x, int y) {
  int cx = x + 13;
  if (level >= 8) {
    display.fillTriangle(cx - 9, y, cx + 9, y, cx + 3, y - 14, C_WHITE);
    display.drawPixel(cx, y - 5, C_BLACK);
    display.drawPixel(cx + 2, y - 8, C_BLACK);
    display.drawPixel(cx - 3, y - 2, C_BLACK);
  } else if (level >= 5) {
    display.fillRect(cx - 7, y - 4, 15, 4, C_WHITE);
    for (int i = 0; i < 3; i++)
      display.fillTriangle(cx - 7 + i * 5, y - 4, cx - 3 + i * 5, y - 4, cx - 5 + i * 5, y - 8, C_WHITE);
  } else if (level >= 2) {
    display.fillTriangle(cx - 5, y, cx + 5, y, cx, y - 9, C_WHITE);
    display.fillCircle(cx, y - 10, 1, C_WHITE);
  }
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
   SELF-PLAYING GAMES  (Claude plays these on its own)
   Each keeps its state in statics and restarts when the scene is
   entered again; it reports lives/score to the director.
   ============================================================ */
void miniClaude(int x, int y, bool armsUp) {      // 18x10 sprite, x,y = top-left
  display.fillRect(x + 2, y, 14, 7, C_WHITE);
  display.fillRect(x, y + (armsUp ? 1 : 3), 18, 3, C_WHITE);
  display.fillRect(x + 5, y + 2, 1, 2, C_BLACK);
  display.fillRect(x + 12, y + 2, 1, 2, C_BLACK);
  display.drawFastVLine(x + 3,  y + 7, 3, C_WHITE);
  display.drawFastVLine(x + 6,  y + 7, 3, C_WHITE);
  display.drawFastVLine(x + 11, y + 7, 3, C_WHITE);
  display.drawFastVLine(x + 14, y + 7, 3, C_WHITE);
}

void heartS(int x, int y, bool full) {             // 5x5 life heart
  static const uint8_t F[5] = { 0x0A, 0x1F, 0x1F, 0x0E, 0x04 };
  static const uint8_t E[5] = { 0x0A, 0x15, 0x11, 0x0A, 0x04 };
  const uint8_t* p = full ? F : E;
  for (uint8_t r = 0; r < 5; r++)
    for (uint8_t c = 0; c < 5; c++)
      if (p[r] & (0x10 >> c)) display.drawPixel(x + c, y + r, C_WHITE);
}

void livesRow(uint8_t lives, uint8_t maxLives) {
  for (uint8_t i = 0; i < maxLives; i++) heartS(1 + i * 7, 1, i < lives);
}

void scoreTR(uint16_t s) {
  char b[8];
  snprintf(b, sizeof(b), "%u", s);
  txt(128 - (int)strlen(b) * 6, 0, 1, b);
}

void dottedRect(int x, int y, int w, int h) {
  for (int i = 0; i < w; i += 2) { display.drawPixel(x + i, y, C_WHITE); display.drawPixel(x + i, y + h - 1, C_WHITE); }
  for (int j = 0; j < h; j += 2) { display.drawPixel(x, y + j, C_WHITE); display.drawPixel(x + w - 1, y + j, C_WHITE); }
}

void gameOverBox(uint16_t score) {
  char b[16];
  display.fillRect(22, 20, 84, 24, C_BLACK);
  display.drawRect(22, 20, 84, 24, C_WHITE);
  txtC(24, 1, "GAME OVER");
  snprintf(b, sizeof(b), "score %u", score);
  txtC(34, 1, b);
}

/* after game over: the director moves on, or (manual) it restarts */
bool gameOverTick(uint16_t& overT) {
  if (++overT < 100) return false;
  if (autoCycle) { playOver = true; return false; }
  return true;
}

/* --- build crane -------------------------------------------- */
void brick(int x, int y) {
  display.fillRect(x, y, 24, 5, C_WHITE);
  display.drawFastHLine(x + 2,  y + 2, 5, C_BLACK);
  display.drawFastHLine(x + 9,  y + 2, 6, C_BLACK);
  display.drawFastHLine(x + 17, y + 2, 5, C_BLACK);
}

void sc_crane(uint32_t f) {
  const uint16_t CY = 165;                         // frames per block
  uint32_t lt = f - sceneTick0;
  uint8_t  n  = (lt / CY) % 7;                     // blocks on the stack (6 = finished)
  uint16_t ph = lt % CY;
  int stackTop = 59 - n * 5;
  int dropHy   = stackTop - 10;
  int tx = 40, hy = 16;                            // trolley x, hook y

  if (n < 6) {
    if (ph < 60)       tx = 40 + 72 * ph / 60;                                  // carry right
    else if (ph < 85)  { tx = 112; hy = 16 + (dropHy - 16) * (ph - 60) / 25; }   // lower
    else if (ph < 95)  { tx = 112; hy = dropHy; }                               // release
    else if (ph < 115) { tx = 112; hy = dropHy - (dropHy - 16) * (ph - 95) / 20; } // raise
    else               tx = 112 - 72 * (ph - 115) / 50;                         // back
  }
  bool    carrying = n < 6 && ph < 85;
  uint8_t shown    = n + ((n < 6 && ph >= 85) ? 1 : 0);

  // jib, apex and mast
  display.drawFastHLine(10, 8, 117, C_WHITE);
  display.drawFastHLine(10, 11, 117, C_WHITE);
  for (int x = 10; x < 124; x += 6) {
    display.drawLine(x, 11, x + 3, 8, C_WHITE);
    display.drawLine(x + 3, 8, x + 6, 11, C_WHITE);
  }
  display.drawLine(18, 8, 21, 2, C_WHITE);
  display.drawLine(24, 8, 21, 2, C_WHITE);
  display.drawLine(21, 2, 70, 8, C_WHITE);
  display.drawFastVLine(18, 11, 48, C_WHITE);
  display.drawFastVLine(24, 11, 48, C_WHITE);
  for (int y = 11; y < 56; y += 6) {
    display.drawLine(18, y, 24, y + 3, C_WHITE);
    display.drawLine(24, y + 3, 18, y + 6, C_WHITE);
  }
  display.fillRect(10, 12, 7, 6, C_WHITE);         // counterweight
  display.drawRect(25, 12, 8, 7, C_WHITE);         // cab
  display.drawPixel(28, 14, C_WHITE);

  // trolley, cable, hook / load
  display.fillRect(tx - 3, 11, 7, 3, C_WHITE);
  display.drawFastVLine(tx, 14, hy - 14, C_WHITE);
  if (carrying) {
    display.drawLine(tx, hy, tx - 9, hy + 5, C_WHITE);
    display.drawLine(tx, hy, tx + 9, hy + 5, C_WHITE);
    brick(tx - 12, hy + 5);
  } else {
    display.drawLine(tx, hy, tx + 2, hy + 3, C_WHITE);
    display.drawLine(tx + 2, hy + 3, tx, hy + 4, C_WHITE);
  }

  for (uint8_t i = 0; i < shown; i++) brick(100, 54 - i * 5);
  display.drawFastHLine(0, 59, 128, C_WHITE);

  // Claude supervising with a little flag, cheering after each drop
  bool cheer = (n < 6 && ph >= 85 && ph < 120) || n == 6;
  int dy = cheer ? -(int)(fabsf(sinf(f * 0.3f)) * 3) : 0;
  claudeFB(44, 40 + dy, false, cheer ? -3 : 0);
  display.drawFastVLine(77, 26 + dy, 18, C_WHITE);
  display.fillRect(78, 26 + dy + (cheer && (f / 4) % 2 ? 1 : 0), 6, 4, C_WHITE);
  if (n == 6) { sparkle(112, 20, f, 3); sparkle(92, 26, f + 5, 2); }

  // sounds
  if (n < 6) {
    if ((ph < 60 || ph >= 115) && ph % 8 == 0) amb(700, 700, 5, 12);  // trolley motor
    else if (ph == 60)  amb(900, 650, 450, 14);                       // lowering
    else if (ph == 85)  fx(500, 280, 45, 45);                         // clunk
    else if (ph == 95)  amb(650, 900, 400, 12);                       // hoist up
  } else if (ph == 0 && soundMode == SND_ALL) {
    static const uint16_t done[] = { 1047, 1319, 1568, 2093 };
    sndClear();
    for (uint8_t i = 0; i < 4; i++) sndPushV(done[i], done[i], 70, 20, 35);
  }
}

/* --- G3 flappy clawd ---------------------------------------- */
void sc_flappy(uint32_t f) {
  static uint32_t enter = 0xFFFFFFFF;
  static bool     restart = false;
  static float    y, vy, px[2];
  static int8_t   gy[2];
  static bool     scored[2];
  static uint8_t  lives, hurt, lazy;
  static uint16_t score, overT;
  const int BX = 16, PW = 10, GAP = 24;

  if (enter != sceneTick0 || restart) {
    enter = sceneTick0; restart = false;
    y = 28; vy = 0; lives = 3; score = 0; hurt = 0; lazy = 0; overT = 0;
    for (uint8_t i = 0; i < 2; i++) { px[i] = 110 + i * 70; gy[i] = random(12, 34); scored[i] = false; }
  }

  if (lives > 0) {
    // aim for the nearest pipe that's still ahead
    uint8_t nx = (px[0] + PW < BX || (px[1] < px[0] && px[1] + PW >= BX)) ? 1 : 0;
    if (lazy) lazy--;
    else if (random(1000) < 5) lazy = 22;          // daydreams now and then...
    if (!lazy && y > gy[nx] + 9 && vy > -0.5f) { vy = -2.3f; amb(900, 1300, 18, 18); }
    vy += 0.2f; if (vy > 2.8f) vy = 2.8f;
    y += vy;    if (y < 9) { y = 9; vy = 0; }

    for (uint8_t i = 0; i < 2; i++) {
      px[i] -= 1.3f;
      if (px[i] < -PW - 2) { px[i] += 140; gy[i] = random(12, 34); scored[i] = false; }
      if (!scored[i] && px[i] + PW < BX) { scored[i] = true; score++; fx2(1760, 2093, 40, 30); }
      bool overX = px[i] < BX + 16 && px[i] + PW > BX + 2;
      if (!hurt && overX && (y + 1 < gy[i] || y + 9 > gy[i] + GAP)) {
        lives--; hurt = 45; fx(700, 180, 220, 45);
        if (!lives) sndOver();
      }
    }
    if (!hurt && y + 9 >= 60) {
      lives--; hurt = 45; y = 28; vy = -1.5f; fx(700, 180, 220, 45);
      if (!lives) sndOver();
    }
    if (hurt) hurt--;
  } else if (gameOverTick(overT)) {
    restart = true;
  }

  livesRow(lives, 3);
  scoreTR(score);
  for (int x = -(int)(f % 4); x < 128; x += 4) display.drawPixel(x, 61, C_WHITE);
  for (uint8_t i = 0; i < 2; i++) {
    int x = (int)px[i], by = gy[i] + GAP;
    display.drawRect(x, -1, PW, gy[i] + 1, C_WHITE);
    display.drawRect(x - 2, gy[i] - 4, PW + 4, 4, C_WHITE);
    display.drawRect(x, by, PW, 61 - by, C_WHITE);
    display.drawRect(x - 2, by, PW + 4, 4, C_WHITE);
    for (int yy = by + 7; yy < 59; yy += 4) display.drawPixel(x + 4, yy, C_WHITE);
  }
  if (!hurt || (hurt / 4) % 2) miniClaude(BX, (int)y, vy < 0);
  if (!lives) gameOverBox(score);
  playLives = lives; playScore = score;
}

/* --- G2 permission panic ------------------------------------ */
struct Perm { const char* title; const char* cmd; bool safe; };
const Perm PERMS[] = {
  { "Allow Bash?",  "$ pwd",            true  },
  { "Allow Bash?",  "$ ls -la",         true  },
  { "Allow Bash?",  "$ git status",     true  },
  { "Allow Bash?",  "$ npm test",       true  },
  { "Allow Edit?",  "src/app.py",       true  },
  { "Allow Fetch?", "docs.python.org",  true  },
  { "Allow Bash?",  "$ rm -rf /",       false },
  { "Allow Bash?",  "$ curl x.sh | sh", false },
  { "Allow Bash?",  "$ git push -f",    false },
  { "Allow Edit?",  "~/.ssh/id_rsa",    false },
};
const uint8_t PERM_COUNT = sizeof(PERMS) / sizeof(PERMS[0]);

void sc_permPanic(uint32_t f) {
  static uint32_t enter = 0xFFFFFFFF;
  static bool     restart = false, allow;
  static uint8_t  lives, cur, st, res;             // st: 0 asking, 1 result, 2 game over
  static uint16_t score, t, T, decideAt, overT;

  bool fresh = (enter != sceneTick0 || restart);
  if (fresh) { enter = sceneTick0; restart = false; lives = 5; score = 0; overT = 0; }
  if (fresh || (st == 1 && t > 40 && lives)) {
    // next prompt; the clock gets shorter as the score climbs
    cur = random(PERM_COUNT); st = 0; t = 0;
    T = 130 - (score * 5 > 80 ? 80 : score * 5);
    if (PERMS[cur].safe) { allow = true;  decideAt = random(15, T + 20); }     // may be too slow
    else if (random(100) < 7) { allow = true; decideAt = random(10, 40); }      // panics and allows!
    else { allow = false; decideAt = random(10, 45); }
    fx2(1568, 1319, 60, 35);
  }

  if (st == 0) {
    t++;
    if (T > t && T - t < T * 3 / 10 && t % 8 == 0) amb(2000, 2000, 6, 20);   // clock ticking
    if (t == decideAt || t >= T) {
      bool safe = PERMS[cur].safe;
      if (t >= T && t != decideAt) res = safe ? 4 : 2;      // time ran out
      else res = allow ? (safe ? 1 : 3) : 2;
      if (res == 1 || res == 2) score++; else lives--;
      if (res == 1)      fx(1047, 1568, 60, 35);
      else if (res == 2) fx2(880, 660, 50, 35);
      else               fx(500, 250, 220, 45);
      if (!lives) sndOver();
      st = 1; t = 0;
    }
  } else if (st == 1) {
    t++;
    if (!lives && t > 40) st = 2;
  } else if (gameOverTick(overT)) {
    restart = true;
  }

  livesRow(lives, 5);
  scoreTR(score);
  dottedRect(2, 10, 124, 36);
  txt(6, 14, 1, PERMS[cur].title);
  txt(6, 25, 1, PERMS[cur].cmd);
  display.drawRect(6, 36, 116, 6, C_WHITE);
  if (st == 0) {
    int w = 112 * (T - t) / T;
    bool urgent = T - t < T * 3 / 10;
    if (!urgent || (f / 4) % 2) display.fillRect(8, 38, w, 2, C_WHITE);
    char b[24];
    snprintf(b, sizeof(b), "deciding%.*s", (int)((f / 8) % 4), "...");
    txtC(53, 1, b);
  } else if (st == 1) {
    static const char* RES[]  = { "", "ALLOWED", "DENIED", "OOPS!", "TOO SLOW" };
    static const char* HINT[] = { "", "+1 safe command", "+1 blocked danger", "-1 that was risky", "-1 too slow" };
    display.fillRect(34, 18, 60, 14, C_WHITE);
    txtCol(64 - (int)strlen(RES[res]) * 3, 21, RES[res], C_BLACK);
    txtC(53, 1, HINT[res]);
  } else {
    gameOverBox(score);
  }
  playLives = lives; playScore = score;
}

/* --- G7 whack a bug ----------------------------------------- */
void bugSprite(int x, int y) {                     // x = centre, y = top
  display.fillRoundRect(x - 6, y + 2, 12, 6, 2, C_WHITE);
  display.fillRect(x - 3, y, 6, 3, C_WHITE);
  display.drawPixel(x - 2, y + 1, C_BLACK);
  display.drawPixel(x + 1, y + 1, C_BLACK);
  display.drawLine(x - 2, y, x - 4, y - 3, C_WHITE);
  display.drawLine(x + 2, y, x + 4, y - 3, C_WHITE);
  for (int s = -1; s <= 1; s += 2) {
    display.drawLine(x + s * 6, y + 3, x + s * 9, y + 1, C_WHITE);
    display.drawLine(x + s * 6, y + 6, x + s * 9, y + 8, C_WHITE);
  }
}

void sc_whack(uint32_t f) {
  static uint32_t enter = 0xFFFFFFFF;
  static bool     restart = false;
  static float    cx;
  static uint8_t  lives, st, hole, react, ham;     // st: 0 empty 1 rising 2 up 3 hiding 4 squashed
  static uint16_t score, t, upFor, overT;
  const int HX[3] = { 22, 64, 106 };

  if (enter != sceneTick0 || restart) {
    enter = sceneTick0; restart = false;
    cx = 64; lives = 3; score = 0; st = 0; t = 0; upFor = 20; ham = 0; overT = 0; hole = 1;
  }

  if (lives > 0) {
    t++;
    switch (st) {
      case 0: if (t > upFor) { st = 1; t = 0; hole = random(3); react = random(4, 24);
                               upFor = random(26, 44); ham = 0; amb(500, 900, 40, 30); } break;
      case 1: if (t >= 8) { st = 2; t = 0; } break;
      case 2: if (t > upFor) { st = 3; t = 0; } break;
      case 3: if (t >= 8) {                           // got away
                st = 0; t = 0; upFor = random(10, 30); lives--;
                fx(300, 200, 160, 40);
                if (!lives) sndOver();
              } break;
      case 4: if (t >= 18) { st = 0; t = 0; upFor = random(8, 26); } break;
    }
    // Claude scoots over the hole and drops the hammer
    bool bugOut = (st == 1 || st == 2);
    if (bugOut && !ham) {
      if (react) react--;
      else {
        float d = HX[hole] - cx;
        if (d > 2.2f)       cx += 2.2f;
        else if (d < -2.2f) cx -= 2.2f;
        else { cx = HX[hole]; ham = 1; }
      }
    }
    if (ham) {
      ham++;
      if (ham == 6 && bugOut) { st = 4; t = 0; score++; fx(900, 150, 70, 50); }
      if (ham > 10) ham = 0;
    }
  } else if (gameOverTick(overT)) {
    restart = true;
  }

  livesRow(lives, 3);
  scoreTR(score);
  int c = (int)cx;
  miniClaude(c - 9, 9, ham > 0);
  if (ham) {
    int len = (ham < 6 ? ham : 6) * 5;
    for (int yy = 20; yy < 20 + len; yy += 2) display.drawPixel(c, yy, C_WHITE);
    display.fillRect(c - 3, 20 + len, 7, 3, C_WHITE);
  }
  if (st >= 1 && st <= 3) {
    int rise = st == 1 ? t * 10 / 8 : st == 2 ? 10 : 10 - t * 10 / 8;
    bugSprite(HX[hole], 54 - rise);
  } else if (st == 4) {
    display.fillRect(HX[hole] - 8, 52, 16, 2, C_WHITE);
    sparkle(HX[hole], 44, f, 3);
  }
  for (uint8_t i = 0; i < 3; i++) {                  // holes hide the bug's lower half
    display.fillRect(HX[i] - 14, 54, 28, 6, C_BLACK);
    display.drawRect(HX[i] - 14, 54, 28, 6, C_WHITE);
    for (int xx = 2; xx < 26; xx += 2) display.drawPixel(HX[i] - 14 + xx, 56, C_WHITE);
  }
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 62, C_WHITE);
  if (!lives) gameOverBox(score);
  playLives = lives; playScore = score;
}

/* ============================================================
   STATS, LEVEL UP, PROFILE
   ============================================================ */
void titleBar(const char* left, const char* right) {
  display.fillRect(0, 0, 128, 11, C_WHITE);
  txtCol(3, 2, left, C_BLACK);
  txtCol(125 - (int)strlen(right) * 6, 2, right, C_BLACK);
}

void sc_stats(uint32_t f) {
  char b[24], lv[8];
  snprintf(lv, sizeof(lv), "Lv %u", level);
  titleBar("TODAY", lv);
  if (!statsKnown) {
    txtC(22, 1, "no stats yet");
    txtC(34, 1, "start the bridge");
    miniClaude(55, 48 + (int)wobble(f, 0.15f, 1), false);
    return;
  }
  snprintf(b, sizeof(b), "commits %u", stCommits);           txt(3, 15, 1, b);
  snprintf(b, sizeof(b), "tasks %u", stTasks);               txt(72, 15, 1, b);
  snprintf(b, sizeof(b), "tests %u", stTests);               txt(3, 26, 1, b);
  snprintf(b, sizeof(b), "%uh%02u", stMin / 60, stMin % 60); txt(72, 26, 1, b);
  snprintf(b, sizeof(b), "+%u -%u", stAdded, stRemoved);     txt(3, 37, 1, b);
  snprintf(b, sizeof(b), "$%lu.%02lu", (unsigned long)(stCostC / 100), (unsigned long)(stCostC % 100));
  txt(72, 37, 1, b);
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 47, C_WHITE);

  const char* cheer = stCommits == 0 ? "let's ship!" : stCommits < 3 ? "nice start!"
                    : stCommits < 6 ? "on a roll!" : "unstoppable!";
  miniClaude(4, 51 + (int)wobble(f, 0.2f, 1), (f / 10) % 2);
  txt(28, 53, 1, cheer);
}

void sc_levelUp(uint32_t f) {
  uint32_t lt = f - sceneTick0;
  for (uint8_t i = 0; i < 16; i++) {                      // confetti
    int x = (i * 37 + 11) % 128;
    int y = (int)((lt * (1 + i % 3) + i * 13) % 64);
    display.drawPixel(x, y, C_WHITE);
    if (i % 2) display.drawPixel(x + 1, y + 1, C_WHITE);
  }
  if ((lt / 6) % 5 != 4) txtC(1, 2, "LEVEL UP!");
  int dy = -(int)(fabsf(sinf(f * 0.3f)) * 4);
  claudeFB(51, 32 + dy, false, (f / 5) % 2 ? -3 : 0);
  display.drawFastHLine(20, 51, 88, C_WHITE);
  sparkle(16, 32, f, 3);
  sparkle(112, 32, f + 6, 3);
  char b[24];
  snprintf(b, sizeof(b), "Lv %u  %s", level, levelTitle());
  txtC(55, 1, b);
}

void sc_profile(uint32_t f) {
  char b[24], lv[8];
  snprintf(lv, sizeof(lv), "Lv %u", level);
  titleBar(lv, levelTitle());
  claudeFB(10, 32 + (int)wobble(f, 0.12f, 1), false, 0);
  display.drawFastHLine(0, 51, 46, C_WHITE);

  uint32_t lo = xpNeed(level), hi = xpNeed(level + 1);
  uint8_t pct = (uint8_t)((xp - lo) * 100 / (hi - lo));
  txt(50, 16, 1, "xp");
  meter(66, 15, 58, 9, pct);
  snprintf(b, sizeof(b), "%lu/%lu", (unsigned long)xp, (unsigned long)hi);
  txt(50, 28, 1, b);
  snprintf(b, sizeof(b), "next: Lv %u", level + 1);
  txt(50, 39, 1, b);
  static const char* HOW[] = { "+10 commit", "+6 tests ok", "+4 task done", "+1 pet me" };
  txt(50, 54, 1, HOW[(f / 60) % 4]);
}

/* the live "what Claude is doing" strip at the bottom of work scenes */
bool tickerScene(uint8_t s) {
  static const uint8_t L[] = { 1, 2, 4, 5, 7, 8, 9, 10, 33, 34, 41, 42, 54 };
  for (uint8_t i = 0; i < sizeof(L); i++) if (L[i] == s) return true;
  return false;
}

void drawTicker() {
  if (!live || !ticker[0] || millis() - tickerAt > 20000 || !tickerScene(scene)) return;
  int w = (int)strlen(ticker) * 6;
  display.fillRect(0, 54, 128, 10, C_BLACK);
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 54, C_WHITE);
  if (w <= 124) {
    txt((128 - w) / 2, 56, 1, ticker);
  } else {                                                // marquee
    int span = w + 40, off = (int)((tick * 2) % span);
    txt(2 - off, 56, 1, ticker);
    txt(2 - off + span, 56, 1, ticker);
  }
}

/* ============================================================
   WORKING / FINISHED  — synced to a real Claude turn
   ============================================================ */
/* Claude's ✻ : six thick arms, rotating and breathing */
void claudeStar(int cx, int cy, float r, float rot) {
  for (uint8_t k = 0; k < 3; k++) {
    float a = rot + k * PI / 3;
    int dx = (int)(cosf(a) * r), dy = (int)(sinf(a) * r);
    for (int o = -1; o <= 1; o++) {
      display.drawLine(cx - dx + o, cy - dy, cx + dx + o, cy + dy, C_WHITE);
      display.drawLine(cx - dx, cy - dy + o, cx + dx, cy + dy + o, C_WHITE);
    }
  }
  display.fillCircle(cx, cy, 2, C_WHITE);
}

void fmtDur(char* b, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60) snprintf(b, n, "%lus", (unsigned long)s);
  else        snprintf(b, n, "%lum %02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void sc_working(uint32_t f) {
  claudeStar(18, 22, 9 + sinf(f * 0.15f) * 3, f * 0.06f);

  char b[24], d[12];
  snprintf(b, sizeof(b), "%s%.*s", VERBS[verbIdx], (int)((f / 8) % 4), "...");
  txt(36, 10, 1, b);
  fmtDur(d, sizeof(d), busy ? millis() - busyStart : 0);
  snprintf(b, sizeof(b), "(%s)", d);
  txt(36, 22, 1, b);

  // eyes glance around while it works, with the usual blinks
  int dx = (int)wobble(f, 0.04f, 6);
  eyes(84 + dx, 42, 12, 16, 12, EY_NORMAL, 3);
}

void sc_finished(uint32_t f) {
  claudeStar(18, 22, 10, 0.3f);
  char d[12];
  fmtDur(d, sizeof(d), busyDur);
  txt(36, 10, 1, "Worked for");
  txt(36, 22, 2, d);
  eyes(64, 48, 14, 14, 14, EY_HAPPY, 3);
  sparkle(14, 50, f, 3);
  sparkle(114, 46, f + 6, 3);
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
  { NULL,              0, sc_crane       },   // 47  build crane
  { NULL,              0, sc_flappy      },   // 48  G3 flappy clawd
  { NULL,              0, sc_permPanic   },   // 49  G2 permission panic
  { NULL,              0, sc_whack       },   // 50  G7 whack a bug
  { NULL,              0, sc_stats       },   // 51  today's stats
  { NULL,              0, sc_levelUp     },   // 52  level up!
  { NULL,              0, sc_profile     },   // 53  level / xp / hat
  { NULL,              0, sc_working     },   // 54  ✻ Enchanting... (27s)
  { NULL,              0, sc_finished    },   // 55  Worked for 1m 23s
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
  sceneTick0 = tick;
  playLives = -1; playOver = false;
  Serial.printf("scene %u\n", scene);
}

/* ============================================================
   LIFE DIRECTOR — when nobody is driving, Claude lives on its own:
   moods, a bit of work, games (and reacts to how they went), naps.
   ============================================================ */
const uint8_t MOODS[] = { 0, 24, 27, 28, 29, 12, 18, 21, 3, 19, 53 };
const uint8_t WORKS[] = { 1, 2, 4, 5, 6, 7, 8, 9, 11, 30, 31, 32, 33, 34, 40, 41, 44, 51, 37 };
const uint8_t PLAYS[] = { 47, 48, 49, 50, 48, 49, 50, 45, 46, 36, 20, 13, 14, 15, 16, 17 };
const uint8_t RESTS[] = { 22, 23, 21 };
const uint8_t WINS[]  = { 35, 24, 45, 28 };
const uint8_t LOSES[] = { 26, 25 };

bool isGameScene(uint8_t s) { return s >= 48 && s <= 50; }
bool isLongScene(uint8_t s) { return s >= 47 && s <= 50; }

void director() {
  uint32_t now = millis();
  if (now - sceneSince < sceneDur && !playOver) return;

  if (isGameScene(scene)) {                          // react to how the game went
    bool won = playLives > 0;
    setScene(won ? WINS[random(sizeof(WINS))] : LOSES[random(sizeof(LOSES))]);
    if (won) sndYay(); else sndFail();
    sceneDur = 3500;
    return;
  }

  uint8_t r = random(100), next;
  do {
    if (r < 30)      next = MOODS[random(sizeof(MOODS))];
    else if (r < 58) next = WORKS[random(sizeof(WORKS))];
    else if (r < 90) next = PLAYS[random(sizeof(PLAYS))];
    else             next = RESTS[random(sizeof(RESTS))];
  } while (next == scene);

  if (isLongScene(next))  sceneDur = random(16000, 24000);
  else if (r < 30)        sceneDur = random(4000, 6500);
  else if (r < 58)        sceneDur = random(5000, 8000);
  else if (r < 90)        sceneDur = random(6000, 9000);
  else                    sceneDur = random(6000, 10000);
  setScene(next);
  sndEnter();
}

void addXp(uint16_t n) {
  xp += n;
  while (level < 250 && xp >= xpNeed(level + 1)) { level++; levelPending = true; }
  prefs.begin("face", false);
  prefs.putULong("xp", xp);
  prefs.end();
  Serial.printf("xp %lu (Lv %u)\n", (unsigned long)xp, level);
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
  if (gameTick % 400 == 0 && soundOn) {             // every 100 points
    sndClear(); sndPushV(1319, 1319, 50, 20, 50); sndPushV(1760, 1760, 80, 0, 50);
  }
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
  { "crane", 47 }, { "flappy", 48 }, { "panic", 49 }, { "whack", 50 },
  { "stats", 51 }, { "profile", 53 },
};
const uint8_t EVENT_COUNT = sizeof(EVENTS) / sizeof(EVENTS[0]);

void onEvent(const char* name) {
  uint32_t now = millis();
  static uint32_t lastNeed = 0, lastHm = 0, lastDone = 0;
  live = true; lastEvent = now; autoCycle = false; followStage = 0;
  if (!strcmp(name, "done"))     addXp(4);
  else if (!strcmp(name, "bug")) addXp(6);

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

  if (!strcmp(name, "done")) {                       // turn finished: "Worked for 1m 23s"
    if (now - lastDone > 5000) { sndDone(); lastDone = now; }
    busyDur = busy ? now - busyStart : 0;
    busy = false;
    setScene(usage5h >= 100 ? 38 : busyDur ? 55 : 35);
    followStage = 1; followAt = now;
    return;
  }
  if (!strcmp(name, "idle")) busy = false;            // session started / ended

  if (!strcmp(name, "think")) {                      // you sent a prompt: Claude is working
    if (!busy) { busy = true; busyStart = now; verbIdx = random(VERB_COUNT); }
    lastToolAt = 0;
    if (scene != 54) setScene(54);
    return;
  }
  if (busy && strcmp(name, "need") && strcmp(name, "key")) lastToolAt = now;   // a real tool step
  for (uint8_t i = 0; i < EVENT_COUNT; i++)
    if (!strcmp(name, EVENTS[i].name)) {
      if (scene != EVENTS[i].scene) setScene(EVENTS[i].scene);
      sceneSince = now;
      return;
    }
}

/* a new commit: shift it in now; the bridge follows up with the real list */
void pushCommit(const char* h) {
  for (uint8_t i = 4; i > 0; i--) {
    memcpy(gHash[i], gHash[i - 1], sizeof(gHash[i]));
    memcpy(gSubj[i], gSubj[i - 1], sizeof(gSubj[i]));
  }
  strncpy(gHash[0], h, 7); gHash[0][7] = 0;
  strcpy(gSubj[0], "new commit");
  commitAt = millis();
}

/* L:<i>,<hash>,<subject>  — one row of the history (empty hash clears it) */
void setGitRow(const char* v) {
  int i = v[0] - '0';
  const char* p = strchr(v, ',');
  if (i < 0 || i > 4 || !p) return;
  const char* q = strchr(p + 1, ',');
  size_t hl = q ? (size_t)(q - p - 1) : strlen(p + 1);
  if (hl > 7) hl = 7;
  memcpy(gHash[i], p + 1, hl); gHash[i][hl] = 0;
  strncpy(gSubj[i], q ? q + 1 : "", sizeof(gSubj[i]) - 1);
  gSubj[i][sizeof(gSubj[i]) - 1] = 0;
  gitKnown = true;
}

/* R:<branch>,<repo name> */
void setGitRepo(const char* v) {
  const char* p = strchr(v, ',');
  size_t bl = p ? (size_t)(p - v) : strlen(v);
  if (bl > sizeof(gitBranch) - 1) bl = sizeof(gitBranch) - 1;
  memcpy(gitBranch, v, bl); gitBranch[bl] = 0;
  strncpy(gitRepo, p ? p + 1 : "", sizeof(gitRepo) - 1);
  gitRepo[sizeof(gitRepo) - 1] = 0;
  gitKnown = true;
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
    case 'C': pushCommit(v); onEvent("git"); addXp(10); if (!gameMode) sndCommit(); break;
    case 'L': setGitRow(v); break;
    case 'R': setGitRepo(v); break;
    case 'T':
      strncpy(ticker, v, sizeof(ticker) - 1);
      ticker[sizeof(ticker) - 1] = 0;
      tickerAt = millis();
      break;
    case 'S': {
      int a[7] = { 0 };
      sscanf(v, "%d,%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
      stCommits = a[0]; stTasks = a[1]; stTests = a[2];
      stCostC = a[3]; stAdded = a[4]; stRemoved = a[5]; stMin = a[6];
      statsKnown = true;
      break;
    }
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
    drawTicker();
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
      lastInput = now;
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
    else {
      petting = true; tapCount = 0; sndPetStart();
      if (now - lastPetXp > 60000 || lastPetXp == 0) { addXp(1); lastPetXp = now; }
    }
  }

  // resolve taps once the multi-tap window closes
  if (tapCount && now - lastTap > TAP_WINDOW_MS) {
    if (tapCount == 1) {
      setScene(scene + 1);
      autoCycle = false;
      sndTap();
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
  if (c == '\n' || c == '\r') return;
  lastInput = millis();
  if      (c == 'n') { setScene(scene + 1);               autoCycle = false; live = false; sndTap(); }
  else if (c == 'p') { setScene(scene + SCENE_COUNT - 1); autoCycle = false; live = false; sndTap(); }
  else if (c == 'r') { setScene(random(SCENE_COUNT));     autoCycle = false; live = false; sndTap(); }
  else if (c == 'c') { autoCycle = !autoCycle; sceneSince = millis(); sceneDur = 0; }
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
char    lineBuf[72];
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
  // mid-turn and no tool running (Claude is thinking / writing): show the spinner
  if (busy && live && !gameMode && !menuOpen && !petting && followStage == 0 &&
      scene != 54 && scene != 10 && scene != 44 && now - lastToolAt > SPIN_AFTER_MS)
    setScene(54);

  // while a turn is running, stay synced (no random faces) — up to 15 min of silence
  bool turnAlive = busy && now - lastEvent < 900000UL;
  if (busy && !turnAlive) busy = false;
  if (live && !turnAlive && now - lastEvent > LIVE_TIMEOUT_MS) {
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
  display.setTextWrap(false);             // long ticker text scrolls instead of wrapping

  prefs.begin("face", true);              // read-only
  briIndex = prefs.getUChar("bri", 5);
  volIndex = prefs.getUChar("vol", 3);
  uint8_t m = prefs.getUChar("sndm", SND_ALL);
  gHi      = prefs.getUShort("hi", 0);
  xp       = prefs.getULong("xp", 0);
  prefs.end();
  while (level < 250 && xp >= xpNeed(level + 1)) level++;
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

  // nobody around for a while: Claude goes back to living on its own
  if (!autoCycle && !live && !petting && !menuOpen && !gameMode && millis() - lastInput > RESUME_MS) {
    autoCycle = true; sceneDur = 0;
  }
  if (autoCycle && !petting && !menuOpen && !gameMode) director();

  if (levelPending && !petting && !menuOpen && !gameMode) {   // celebrate a level up
    levelPending = false;
    setScene(52);
    sceneDur = 4500; followStage = 0;
    sndLevelUp();
  }

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
     RESUME_MS       idle time before Claude goes back to living on its own
     MOODS/WORKS/PLAYS/RESTS[]   which scenes the life director picks from
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