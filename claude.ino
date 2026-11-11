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
     H:1            bridge connected -> "Build something great" intro
     Z:<ms>,<line>  lyric line, sung over <ms> (Z:0, = no lyrics)
     F:<key>,<val>  settings page: bri / vol / snd / face / hello; F:? asks (reply F:bri,n,vol,n,snd)
     I:1            heartbeat, every 5 s (15 s of silence = bridge gone, Claude goes solo)
     B:<min>        you're back after <min> minutes away
     N:<min>,<title> meeting in <min> minutes
     O:<icon>,<text> notification banner (0 bell 1 github 2 chat 3 check 4 calendar)
     Y:<done>,<total>,<now>  Claude's to-do list
     V:<icon>,<title> achievement / streak (0 trophy 1 flame 2 star)
                    (also plays at power-on, after an old-TV switch-on)
   ============================================================= */

/* The laptop bridge talks to this board over the C3's built-in USB port.
   Without "USB CDC On Boot" the Serial output goes to the UART pins instead
   and the bridge sees a silent, unresponsive port. */
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
#error "Arduino IDE: Tools > USB CDC On Boot > Enabled, then upload again."
#endif

/* WIRELESS: the laptop bridge can also reach the board over Bluetooth LE
   ("Claude Buddy", Nordic UART service), so it only needs USB for power.
   0 = USB cable only. */
#define USE_BLE 1

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#if USE_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#endif

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
#define ALIVE_AFTER_MS 12000 // bridge connected, Claude idle this long -> plays / reacts on its own
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
/* is the laptop bridge there? any line from it (it sends I:1 every 5 s) */
#define BRIDGE_TIMEOUT_MS 15000
uint32_t lastBridgeAt = 0;
bool     bridgeUp = false;

int8_t   usage5h     = -1;            // -1 = unknown
int8_t   usageWk     = -1;
int16_t  resetMin    = -1;            // minutes to 5h reset, at resetStamp
uint32_t resetStamp  = 0;
int8_t   ctxPct      = -1;
/* your repo's real history (L: / R: lines from the bridge), newest first */
bool     gitKnown      = false;
#define  GIT_ROWS      7
char     gHash[GIT_ROWS][8];
char     gSubj[GIT_ROWS][20];
char     gitBranch[12] = "";
char     gitRepo[20]   = "";
uint32_t commitAt      = 0;           // when the newest commit arrived (row flashes)

uint8_t  followStage = 0;             // after "done": trophy -> usage -> idle
uint32_t followAt    = 0;

char     ticker[44]  = "";            // what Claude is doing right now (T:)
uint32_t tickerAt    = 0;
bool     shellPs     = false;         // shell window: PowerShell or bash
uint8_t  keyScene    = 255;           // scene shown as a reaction to your typing
uint32_t lastKeyAt   = 0;

/* music playing on the laptop (M: / A: lines) */
bool     musicOn     = false;
bool     musicMode   = false;         // the board is showing music faces
uint32_t musicAt     = 0;
char     musicTitle[44] = "";
uint8_t  musicLevel  = 0;             // live output level 0-99
float    musicAvg    = 0;
uint32_t beatAt      = 0;
float beatPulse() {                   // 1 right on a beat, fading to 0 over 250 ms
  uint32_t d = millis() - beatAt;
  return d < 250 ? 1.0f - d / 250.0f : 0;
}
bool     musicPaused = false;         // paused, but played recently: the player stays up
float    musicSmooth = 0;             // eased loudness, for smooth movement
float    beatGlow    = 0;             // eased beat
uint8_t  pressedBtn  = 0;             // player button just pressed (1 prev, 2 play/pause, 3 next)
uint32_t pressedAt   = 0;
float    discAngle   = 0;
uint32_t trackPos = 0, trackDur = 0, trackAt = 0;   // song position / length in s (Q: lines)

/* date & time from the laptop (D:<local epoch seconds>); kept running with millis() */
bool     timeKnown  = false;
uint32_t localEpoch = 0, epochAt = 0;
int      tYear = 0, tMon = 0, tDay = 0, tWday = 0, tHour = 0, tMin = 0, tSec = 0;

void updateClock() {
  if (!timeKnown) return;
  time_t t = (time_t)localEpoch + (millis() - epochAt) / 1000;
  struct tm tmv;
  gmtime_r(&t, &tmv);                  // the epoch is already local time
  tYear = tmv.tm_year + 1900; tMon = tmv.tm_mon + 1; tDay = tmv.tm_mday; tWday = tmv.tm_wday;
  tHour = tmv.tm_hour; tMin = tmv.tm_min; tSec = tmv.tm_sec;
}

bool isNight() { return timeKnown && (tHour >= 23 || tHour < 6); }

uint32_t manualAt    = 0;             // you changed face yourself: music logic backs off

/* a Claude turn in progress: from your prompt until Claude finishes */
bool     busy        = false;
uint32_t busyStart   = 0;
uint32_t busyDur     = 0;             // how long the last turn took
uint32_t lastToolAt  = 0;             // last real tool event in this turn
uint16_t busySteps   = 0;             // tools Claude has run this turn
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
#define PET_HOLD_MS    650       // a deliberate hold, not a brush of the finger
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
bool     touchBusy     = false;   // this touch began in a game / menu / intro: it never pets

/* ============================================================
   BLUETOOTH LE  — a wireless "serial cable" (Nordic UART service)
   Bytes arrive on the BLE task; a queue hands them to loop().
   ============================================================ */
#if USE_BLE
#define BLE_NAME     "Claude Buddy"
#define NUS_SERVICE  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

QueueHandle_t bleQ = NULL;
volatile bool bleConnected = false;
BLECharacteristic* bleTx = NULL;      // board -> laptop (music buttons)

class BleRx : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    auto v = ch->getValue();
    for (size_t i = 0; i < v.length(); i++) { char c = v[i]; xQueueSend(bleQ, &c, 0); }
  }
};

class BleConn : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override    { bleConnected = true; }
#if defined(CONFIG_BLUEDROID_ENABLED)
  // ask for a snappy link (7.5-15 ms interval, 2 s timeout): the equalizer
  // gets ~12 level updates a second and touch buttons should feel instant
  void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* p) override {
    bleConnected = true;
    s->updateConnParams(p->connect.remote_bda, 0x06, 0x0C, 0, 200);
  }
#endif
  void onDisconnect(BLEServer* s) override { bleConnected = false; BLEDevice::startAdvertising(); }
};

void bleInit() {
  bleQ = xQueueCreate(2048, 1);
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(185);                 // bigger packets: fewer, faster writes from the laptop
  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new BleConn());
  BLEService* svc = srv->createService(NUS_SERVICE);
  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRx());
  bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  // Bluedroid doesn't add the CCCD by itself: without it the laptop can't
  // subscribe and the music buttons never reach it over Bluetooth
  bleTx->addDescriptor(new BLE2902());
#endif
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);             // connection interval hints for the central
  adv->setMaxPreferred(0x0C);
  BLEDevice::startAdvertising();
}
#endif

/* a line back to the laptop bridge, over USB and Bluetooth */
void sendToLaptop(const char* line) {
  Serial.println(line);
#if USE_BLE
  if (bleConnected && bleTx) {
    String s = String(line) + "\n";
    bleTx->setValue((uint8_t*)s.c_str(), s.length());
    bleTx->notify();
  }
#endif
}

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
void sndDone() {                                        // resolving chime + "all done!"
  if (!soundOn) return;
  sndClear();
  sndPushV(784, 784, 60, 15, 70);
  sndPushV(988, 988, 60, 15, 75);
  sndPushV(1319, 1319, 140, 70, 85);
  sndPushV(1047, 1175, 60, 35, 70);
  sndPushV(1175, 1568, 110, 0, 75);
}
void sndPrompt() {                                      // message sent -> "got it!"
  if (!soundOn) return;
  sndClear();
  sndPushV(500, 1500, 70, 40, 40);
  sndPushV(1319, 1568, 50, 25, 55);
  sndPushV(1760, 1760, 70, 0, 50);
}
void sndFail()    { if (soundOn) { sndClear(); sndPush(700, 600, 110, 50); sndPush(560, 400, 180, 0); } }
void sndCommit() {                                      // "ka-ching" + sparkle
  if (!soundOn) return;
  static const uint16_t n[] = { 1047, 1319, 1568, 2093 };
  sndClear();
  for (uint8_t i = 0; i < 4; i++) sndPushV(n[i], n[i], i < 3 ? 40 : 120, 10, 80);
  for (uint8_t i = 0; i < 4; i++) sndPushV(i % 2 ? 3136 : 2637, i % 2 ? 3136 : 2637, 25, 5, 55);
}
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

/* --- petting voice + love ------------------------------------
   Love (0-100) grows every second you pet and with every new pet,
   and fades slowly when you don't; the sounds get more affectionate:
   hold starts -> "oo-h!" (or "kyaa~" when loved)   while held -> coos,
   purr-trills, "la-la" love tunes   3 s -> giggle   let go -> sigh / "mwah" */
uint32_t petStart = 0, nextCoo = 0;
bool     giggled  = false;
uint8_t  love = 0;
uint32_t lastLoveTick = 0, lastLoveDecay = 0;

uint8_t loveStage() { return love < 30 ? 0 : love < 60 ? 1 : love < 90 ? 2 : 3; }

void sndPetStart() {
  petStart = millis(); giggled = false;
  love = love + 4 > 100 ? 100 : love + 4;              // every pet counts
  uint8_t st = loveStage();
  nextCoo = petStart + 1100 - st * 200;

  if (!soundOn) return;
  sndClear();
  if (st == 0) {
    sndPushV(900, 1300, 90, 25, 90);                     // "oo-
    sndPushV(1300, 1550, 70, 0, 100);                    //    -h!"
  } else if (st == 1) {
    sndPushV(900, 1300, 80, 20, 90);
    sndPushV(1300, 1650, 70, 30, 100);
    sndPushV(1568, 1760, 40, 20, 80); sndPushV(1568, 1760, 40, 0, 80);   // hee-hee
  } else {
    sndPushV(1000, 1900, 130, 25, 100);                  // "kyaa~!"
    sndPushV(1568, 1568, 60, 15, 85); sndPushV(2093, 2093, 110, 0, 85);  // + a heart
  }
}

void sndLoveUp() {                                       // love grew a stage
  if (!soundOn) return;
  static const uint16_t n[] = { 1047, 1319, 1568, 2093 };
  sndClear();
  for (uint8_t i = 0; i < 4; i++) sndPushV(n[i], n[i], 55, 12, 85);
  sndPushV(2093, 2637, 220, 0, 95);
}

void sndLoveTune() {                                     // "la-la-la~"
  static const uint16_t n[] = { 1319, 1568, 1760, 2093, 1760 };
  sndClear();
  for (uint8_t i = 0; i < 5; i++) sndPushV(n[i], n[i], 70, 25, 60 + i * 6);
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

void sndPetEnd() {                                       // contented sigh, or a "mwah" when loved
  prefs.begin("face", false); prefs.putUChar("love", love); prefs.end();
  if (!soundOn) return;
  sndClear();
  if (loveStage() >= 2) {
    sndPushV(1500, 2300, 60, 30, 85);                    // "mwa-
    sndPushV(2637, 2637, 20, 80, 70);                    //    -h!" (pop)
  }
  sndPushV(1250, 1350, 60, 0, 70);
  sndPushV(1350, 820, 260, 0, 50);
}

void petUpdate() {
  uint32_t now = millis();
  if (petting) {                                         // love grows while you pet
    lastLoveDecay = now;
    if (now - lastLoveTick >= 1000) {
      lastLoveTick = now;
      uint8_t before = loveStage();
      love = love + 3 > 100 ? 100 : love + 3;
      if (loveStage() > before) { sndLoveUp(); nextCoo = now + 1600; }
    }
  } else if (love && now - lastLoveDecay > 90000) {      // ...and fades slowly without you
    lastLoveDecay = now;
    love--;
  }

  if (!petting || !soundOn || sqLen) return;
  if (now < nextCoo) return;
  uint8_t st = loveStage();
  if (!giggled && now - petStart > 3000)  { sndGiggle(); giggled = true; }
  else if (st >= 2 && random(3) == 0)     sndLoveTune();
  else if (random(3) == 0)                sndChirr();
  else                                    sndCoo();
  nextCoo = now + random(1300, 2100) - st * 300;         // chattier the more it's loved
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

/* Bluetooth link toast: the rune pulses on connect, cracks apart when it drops */
uint32_t linkToastAt = 0;
bool     linkToastUp = false;

void btRune(int x, int y, int h, int split) {        // the Bluetooth rune, centred on x,y
  int q = h / 4;
  display.drawLine(x - split, y - 2 * q, x - split, y + 2 * q, C_WHITE);
  display.drawLine(x - split, y - 2 * q, x + q - split, y - q, C_WHITE);
  display.drawLine(x + q + split, y - q, x - q + split, y + q, C_WHITE);
  display.drawLine(x - split, y + 2 * q, x + q - split, y + q, C_WHITE);
  display.drawLine(x + q + split, y + q, x - q + split, y - q, C_WHITE);
}

/* a phone-style notification card: drops in from the top with a little
   bounce, an icon in a circle, a title line and a (scrolling) message line */
int cardY(int t, int dur) {
  if (t < 320) return -26 + (int)(28 * easeOutBack(t / 320.0f));
  if (t > dur - 220) return 2 - (int)(28 * clamp01((t - (dur - 220)) / 220.0f));
  return 2;
}

void drawCard(int y, const char* title, const char* body, int scroll) {
  display.fillRoundRect(1, y, 126, 25, 6, C_BLACK);
  txt(27 - scroll, y + 14, 1, body);                 // message first: it may scroll...
  display.fillRect(0, y + 13, 26, 10, C_BLACK);      // ...inside its own lane
  display.fillRect(123, y + 13, 5, 10, C_BLACK);
  display.drawRoundRect(1, y, 126, 25, 6, C_WHITE);
  txt(27, y + 3, 1, title);
  display.drawFastHLine(27, y + 11, (int)strlen(title) * 6 - 1, C_WHITE);   // title underline
  display.drawCircle(13, y + 12, 9, C_WHITE);
}

/* Bluetooth connected / disconnected */
void drawLinkToast() {
  if (!linkToastAt) return;
  const int dur = 2400;
  int t = (int)(millis() - linkToastAt);
  if (t > dur) { linkToastAt = 0; return; }
  int y = cardY(t, dur);
  drawCard(y, "Bluetooth", linkToastUp ? "connected" : "disconnected", 0);
  if (linkToastUp) {
    if ((t / 260) % 2) display.drawCircle(13, y + 12, 7, C_WHITE);   // pulsing ring
    btRune(13, y + 12, 12, 0);
  } else {
    btRune(13, y + 12, 12, t < 400 ? 0 : t < 700 ? (t - 400) / 100 : 3);   // cracks in half
  }
}

void drawToast() {
  drawLinkToast();
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
    case 54: {                                            // Claude thinking: generative melody
      static const uint16_t TUNE[] = { 523, 587, 659, 784, 880, 1047, 1175, 1319, 1568, 1760 };
      static int8_t note = 4;
      if (f % 168 == 0 && ambOk()) { sndPushV(392, 392, 90, 0, 35); break; }   // low pulse
      if (f % 21 == 0 && random(4)) {                    // in time with the ✻ breathing
        note += random(-2, 3);
        if (note < 0) note = 1;
        if (note > 9) note = 8;
        amb(TUNE[note], TUNE[note], 140, 26);
      }
      break;
    }
    case 55: if (f % 50 == 0) amb(2093 + random(3) * 250, 2637, 40, 20); break;   // finished: sparkles
    case 57: {                                            // commit log: hash types, then ka-ching
      uint32_t lt = (f - sceneTick0) % 120;
      if (lt < 28 && lt % 4 == 0) amb(2200, 2000, 6, 20);
      else if (lt == 30 && ambOk()) { sndPushV(1568, 1568, 40, 10, 40); sndPushV(2093, 2093, 90, 0, 40); }
      break;
    }
    case 58: {                                            // merge: dot rides, then chime
      uint32_t lt = (f - sceneTick0) % 120;
      if (lt < 60 && lt % 10 == 0) amb(900 + lt * 10, 900 + lt * 10, 15, 16);
      else if (lt == 60 && ambOk()) { sndPushV(1319, 1319, 50, 15, 40); sndPushV(1760, 1760, 100, 0, 40); }
      break;
    }
    case 42: case 59: {                                   // plane: "hup!" + whoosh
      uint32_t t = (f - sceneTick0) % 130;
      if (t == 4)       amb(500, 420, 60, 25);
      else if (t == 10) amb(900, 1900, 180, 32);
      break;
    }
    case 60: if (f % 20 == 0) amb(f % 40 ? 1319 : 1568, 1600, 18, 26); break;   // catches
    case 61: if (f % 96 == 0) amb(480, 400, 600, 16); break;                   // soft snore
    case 62: if (f % 90 == 1 && ambOk()) { sndPushV(1047, 1319, 60, 30, 40); sndPushV(1568, 1568, 80, 0, 40); } break;   // "hi!"
    case 56: {                                            // shell: key clicks, then "enter"
      uint32_t lt = f - sceneTick0, len = strlen(ticker) + 6;
      if (lt < len && f % 2 == 0)  amb(2400, 2000, 6, 24);
      else if (lt == len + 2)      amb(1200, 900, 35, 40);
      break;
    }
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
    case 13: case 14: case 15: case 16: case 17:          // music: soft melody (silent over real music)
      if (!musicOn && f % 10 == 0 && random(4)) { uint16_t n = MELODY[random(5)]; amb(n, n, 120, 22); }
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
/* Claude peeking over the top of a keyboard, fingers on the edge */
void sc_keyboard(uint32_t f) {
  const char* k = "asdfjkl;";
  uint8_t hit = ((f / 6) * 5) % 8;                      // which key is down

  // eyes rise from behind the keyboard, look around, blink
  int peek = 7 + (int)(fabsf(sinf(f * 0.04f)) * 8);     // 7..15 px showing
  int look = (int)wobble(f, 0.03f, 5);
  int eh = (int)(peek * gBlink); if (eh < 2) eh = 2;
  display.fillRect(30 + look, 43 - eh, 12, eh, C_WHITE);
  display.fillRect(74 + look, 43 - eh, 12, eh, C_WHITE);

  // fingertips gripping the edge, tapping on their side
  display.fillRect(13, 37 + (hit < 4 ? 1 : 0), 6, 6, C_WHITE);
  display.fillRect(102, 37 + (hit >= 4 ? 1 : 0), 6, 6, C_WHITE);

  // the keyboard: top edge, a row of keys, the next row peeking in below
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 44, C_WHITE);
  char c[2] = { 0, 0 };
  for (uint8_t i = 0; i < 8; i++) {
    int x = 2 + i * 16;
    c[0] = k[i];
    if (i == hit) { display.fillRect(x, 47, 13, 11, C_WHITE); txtCol(x + 4, 49, c, C_BLACK); }
    else          { display.drawRect(x, 47, 13, 11, C_WHITE); txt(x + 4, 49, 1, c); }
  }
  for (uint8_t i = 0; i < 9; i++) display.drawRect(-6 + i * 16, 60, 13, 6, C_WHITE);
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
/* with music playing on the laptop these follow the real sound:
   musicLevel = loudness right now, beatPulse() = 1 on a beat */
/* all music movement is eased (musicSmooth / beatGlow) and slow,
   like Claude swaying to a mellow song - no harsh jumps */
void sc_music(uint32_t f) {
  float lv = musicOn ? musicSmooth : 20;
  int bob = (int)(sinf(f * 0.06f) * (1.5f + lv / 30) - beatGlow * 2);
  chevronEyes(64, 26 + bob, 20 + (int)(lv / 25), false);
  note(22, 12 + (int)wobble(f, 0.05f, 3), 6);
  note(100, 34 + (int)wobble(f, 0.06f, 3), 6);
}

/* --- headbang (a gentle nod) -------------------------------- */
void sc_headbang(uint32_t f) {
  float lv = musicOn ? musicSmooth : 30;
  int dy = (int)((1 - cosf(f * 0.09f)) * (1.5f + lv / 25) + beatGlow * 2);
  eyes(64, 22 + dy, 22, 30, 26, EY_NORMAL, 7);
  note(106, 16 + (int)wobble(f, 0.07f, 2), 7);
}

/* --- dance: a slow, happy sway, eyes closed ------------------ */
void sc_dance(uint32_t f) {
  static float phase = 0;
  float lv = musicOn ? musicSmooth : 25;
  phase += 0.045f + lv * 0.0004f;                       // sways a little faster when it's louder
  int dx = (int)(sinf(phase) * (4 + lv / 10));
  int dy = (int)(sinf(phase * 2) * 1.5f - beatGlow * 2);
  eyes(64 + dx, 28 + dy, 20, 30, 26, EY_HAPPY, 6);
  note(14, 14 + (int)wobble(f, 0.05f, 3), 6);
  note(108, 34 + (int)wobble(f, 0.06f, 3), 6);
}

/* --- equalizer: snaps to the song's real loudness, bass kicks on beats */
void sc_equalizer(uint32_t f) {
  static float bar[14];
  for (uint8_t i = 0; i < 14; i++) {
    float target;
    if (musicOn) {                                  // the real loudness, spread over the bands
      float shape = 0.45f + 0.55f * (sinf(f * 0.31f + i * 1.7f) * 0.5f + 0.5f);
      float bass  = i < 4 ? beatPulse() * 0.5f : 0;
      target = musicLevel * 0.46f * (shape + bass);
    } else {
      float a = sinf(f * 0.12f + i * 0.9f) + sinf(f * 0.05f + i * 2.1f) * 0.6f;
      target = fabsf(a) * 20;
    }
    bar[i] = target > bar[i] ? target : bar[i] * 0.82f;   // fast attack, smooth fall
    int h = 4 + (int)bar[i];
    if (h > 46) h = 46;
    display.fillRect(4 + i * 9, 52 - h, 7, h, C_WHITE);
    display.drawFastHLine(4 + i * 9, 52 - h - 2, 7, C_WHITE);   // peak cap
  }
  display.drawFastHLine(0, 53, 128, C_WHITE);
}

/* --- now playing -------------------------------------------- */
void sc_nowPlaying(uint32_t f) {
  note(2, 1, 5);
  const char* t = (musicOn && musicTitle[0]) ? musicTitle : "music";
  int w = (int)strlen(t) * 6;
  if (w <= 112) txt(12, 3, 1, t);
  else {                                            // song title scrolls
    int span = w + 30, off = (int)((f * 1) % span);
    txt(12 - off, 3, 1, t);
    txt(12 - off + span, 3, 1, t);
    display.fillRect(0, 0, 11, 11, C_BLACK);
    note(2, 1, 5);
  }
  int bounce = (int)(sinf(f * 0.06f) * 1.5f - beatGlow * 2);
  eyes(64, 30 + bounce, 16, 22, 30, EY_HAPPY, 5);
  for (uint8_t i = 0; i < 16; i++) {
    float lvl = musicOn ? musicSmooth / 12.0f : 7;
    int h = 2 + (int)(fabsf(sinf(f * 0.06f + i)) * lvl);
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
  int sway = (int)wobble(f, 0.12f, 4);             // leaning into the hand
  int dy = (int)fabsf(wobble(f, 0.24f, 3));        // a happy squish with each stroke
  chevronEyes(64 + sway, 27 + dy, 22, true);

  // blush
  for (uint8_t i = 0; i < 3; i++) {
    display.drawLine(30 + sway + i * 4, 45 + dy, 32 + sway + i * 4, 42 + dy, C_WHITE);
    display.drawLine(88 + sway + i * 4, 45 + dy, 90 + sway + i * 4, 42 + dy, C_WHITE);
  }
  // little "w" mouth
  int mx = 58 + sway;
  display.drawLine(mx, 42 + dy, mx + 3, 46 + dy, C_WHITE);
  display.drawLine(mx + 3, 46 + dy, mx + 6, 43 + dy, C_WHITE);
  display.drawLine(mx + 6, 43 + dy, mx + 9, 46 + dy, C_WHITE);
  display.drawLine(mx + 9, 46 + dy, mx + 12, 42 + dy, C_WHITE);

  // the stroke: a soft swish moving over the top of the head
  int hx = 64 + (int)wobble(f, 0.12f, 26);
  for (int k = 0; k < 3; k++) display.drawFastHLine(hx - 9 + k * 3, 3 + k * 2, 18 - k * 6, C_WHITE);

  // purring: little wiggles beside the eyes
  if ((f / 4) % 2) {
    display.drawLine(21, 22, 24, 25, C_WHITE); display.drawLine(24, 25, 21, 28, C_WHITE);
    display.drawLine(107, 22, 104, 25, C_WHITE); display.drawLine(104, 25, 107, 28, C_WHITE);
  }

  // hearts drifting up both sides
  for (uint8_t i = 0; i < 2; i++) {
    int p = (int)((f + i * 22) % 44);
    int x = i ? 116 : 12;
    heart(x + (int)wobble(f + i * 9, 0.2f, 2), 52 - p, p < 30 ? 4 : 3);
  }
}

/* --- juggling ----------------------------------------------- */

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
void sc_sleepy(uint32_t f) {
  uint32_t c = f % 160;                            // 4 s: droop... droop... jolt!
  int h, jx = 0;
  if (c < 130)      h = 26 - (int)(c * 20 / 130);  // eyelids sink
  else if (c < 136) { h = 30; jx = (c % 2) ? 2 : -2; }   // jolt awake, a little shake
  else              h = 26;
  eyes(64 + jx, 30, 28, h, 22, EY_SLEEPY, 8, false);
  if (c > 90 && c < 130) txt(104, 30 - (int)((c - 90) / 8), 1, "z");
  display.drawRect(100, 3, 22, 11, C_WHITE);       // battery
  display.fillRect(122, 6, 2, 5, C_WHITE);
  if ((f / 12) % 2) display.fillRect(102, 5, 4, 7, C_WHITE);   // the last bar, blinking
}

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
  // parcels drop into an open crate, one after another
  for (uint8_t i = 0; i < 2; i++) {
    int ph = (int)((f + i * 30) % 60);
    int x = i ? 66 : 44, y = -16 + ph * 44 / 60;
    if (y > 26) continue;                          // landed inside
    display.fillRect(x, y, 18, 14, C_WHITE);
    display.drawFastVLine(x + 8, y, 14, C_BLACK);  // tape across the top
    display.drawFastVLine(x + 9, y, 14, C_BLACK);
    display.drawFastHLine(x + 2, y + 10, 4, C_BLACK);   // a label
  }
  // the crate, in front of the parcels
  display.fillRect(34, 30, 60, 23, C_BLACK);
  display.drawRect(34, 30, 60, 23, C_WHITE);
  display.drawLine(34, 30, 25, 21, C_WHITE);       // flaps
  display.drawLine(93, 30, 102, 21, C_WHITE);
  // its front slat is the progress bar
  int w = (int)((f / 2) % 52);
  display.drawRect(38, 40, 52, 6, C_WHITE);
  display.fillRect(38, 40, w, 6, C_WHITE);
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
  uint32_t p = f % 140;                            // 3.5 s: run-up, kick, flight, goal!
  // the goal on the right; the net bulges when the ball hits it
  bool hit = p >= 84 && p < 96;
  display.drawRect(104, 26, 22, 28, C_WHITE);
  for (uint8_t c = 0; c < 4; c++)
    for (uint8_t r = 0; r < 6; r++)
      display.drawPixel(107 + c * 5 + (hit && r > 1 && r < 5 ? 1 : 0), 29 + r * 4, C_WHITE);
  display.drawFastHLine(0, 54, 128, C_WHITE);      // the pitch

  // Claude: runs up, kicks, then jumps for joy
  int cx, cy = 35;
  if (p < 40)      { cx = 4 + (int)p * 26 / 40; cy = 35 - (int)((f / 4) % 2); }   // running: a little bob
  else             cx = 30;
  if (p >= 96)     cy = 35 - (int)(fabsf(sinf((p - 96) * 0.25f)) * 6);
  claudeFB(cx, cy, false, p >= 40 && p < 48 ? -3 : 0);

  // the ball: waits at the spot, then flies into the net
  int bx = 62, by = 51;
  if (p >= 42 && p < 84) {
    float t = (p - 42) / 42.0f;
    bx = 62 + (int)(t * 52);
    by = 51 - (int)(t * 10) - (int)(sinf(t * PI) * 22);
  } else if (p >= 84) { bx = 117; by = 44; }
  display.fillCircle(bx, by, 3, C_BLACK);
  display.drawCircle(bx, by, 3, C_WHITE);
  display.drawPixel(bx, by, C_WHITE);

  if (p >= 90 && (p / 6) % 2) txt(34, 4, 2, "GOAL!");
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
void sc_paperPlane(uint32_t f) { sc_planeThrow(f); }     // same scene: Claude launches it

/* --- bug squashed ------------------------------------------- */

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
/* one course of bricks, 24x4, mortar joints offset on alternate rows */
void brickRow(int x, int y, uint8_t row) {
  display.fillRect(x, y, 24, 4, C_WHITE);
  for (int g = (row % 2) ? 5 : 8; g < 24; g += 6) display.drawFastVLine(x + g, y, 4, C_BLACK);
}

/* The crane picks a block up off the ground, carries it over and lowers
   it onto the building; Claude supervises with a flag and plants it on
   the roof when the building is done.                                 */
void sc_crane(uint32_t f) {
  const uint16_t CY = 150;                         // frames per block
  const int PX = 76, SX = 110, UP = 14;            // pickup x, stack x, hook-up height
  uint32_t lt = f - sceneTick0;
  uint8_t  n  = (lt / CY + 2) % 7;                 // blocks on the building (6 = finished)
  uint16_t ph = lt % CY;
  int dropHy = 58 - n * 5 - 5, grabHy = 53;
  int tx = SX, hy = UP;                            // trolley x, hook y
  bool carrying = false, groundBlock = false;

  if (n < 6) {
    if      (ph < 22)  { tx = SX - (SX - PX) * ph / 22; groundBlock = true; }          // to pickup
    else if (ph < 40)  { tx = PX; hy = UP + (grabHy - UP) * (ph - 22) / 18; groundBlock = true; }  // lower
    else if (ph < 46)  { tx = PX; hy = grabHy; carrying = true; }                      // hook on
    else if (ph < 64)  { tx = PX; hy = grabHy - (grabHy - UP) * (ph - 46) / 18; carrying = true; } // lift
    else if (ph < 94)  { tx = PX + (SX - PX) * (ph - 64) / 30; carrying = true; }      // carry
    else if (ph < 112) { hy = UP + (dropHy - UP) * (ph - 94) / 18; carrying = true; }  // lower
    else if (ph < 118) { hy = dropHy; }                                                // release
    else if (ph < 136) { hy = dropHy - (dropHy - UP) * (ph - 118) / 18; }              // raise
    if (ph >= 128) groundBlock = true;             // the next block is delivered
  } else {
    tx = 84;                                       // park clear of the roof flag
  }
  uint8_t shown = n + ((n < 6 && ph >= 112) ? 1 : 0);

  // jib with its sloped tie-line, counterweight, cab and lattice mast
  display.drawLine(2, 5, 8, 1, C_WHITE);
  display.drawFastHLine(8, 1, 22, C_WHITE);
  display.drawLine(30, 1, 62, 5, C_WHITE);
  display.drawFastHLine(0, 5, 126, C_WHITE);
  display.drawFastHLine(0, 8, 126, C_WHITE);
  for (int x = 0; x < 123; x += 6) {
    display.drawLine(x, 8, x + 3, 5, C_WHITE);
    display.drawLine(x + 3, 5, x + 6, 8, C_WHITE);
  }
  display.fillRect(0, 9, 6, 6, C_WHITE);           // counterweight
  display.drawRect(12, 9, 10, 8, C_WHITE);         // cab
  display.drawRect(14, 11, 6, 3, C_WHITE);
  display.drawFastVLine(6, 8, 54, C_WHITE);
  display.drawFastVLine(11, 8, 54, C_WHITE);
  for (int y = 9; y < 60; y += 6) {
    display.drawLine(6, y, 11, y + 3, C_WHITE);
    display.drawLine(11, y + 3, 6, y + 6, C_WHITE);
  }

  // trolley, cable, hook / load
  display.fillRect(tx - 3, 8, 7, 2, C_WHITE);
  display.drawFastVLine(tx, 10, hy - 10, C_WHITE);
  if (carrying) {
    display.drawLine(tx, hy, tx - 9, hy + 5, C_WHITE);
    display.drawLine(tx, hy, tx + 9, hy + 5, C_WHITE);
    brickRow(tx - 12, hy + 5, n);
  } else {
    display.drawLine(tx, hy, tx + 2, hy + 3, C_WHITE);
    display.drawLine(tx + 2, hy + 3, tx, hy + 4, C_WHITE);
  }
  if (groundBlock) brickRow(PX - 12, 58, n + 1);

  // the building
  for (uint8_t i = 0; i < shown; i++) brickRow(SX - 12, 58 - i * 5, i);
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 62, C_WHITE);

  // warning light on top of the crane
  if ((f / 20) % 2) display.fillRect(7, 0, 3, 2, C_WHITE);

  // dust puff where a block just landed
  if (n < 6 && ph >= 112 && ph < 124) {
    int k = ph - 112, by = 58 - n * 5 + 4;
    for (int s = -1; s <= 1; s += 2) {
      display.drawPixel(SX + s * (13 + k), by - k / 3, C_WHITE);
      display.drawPixel(SX + s * (15 + k), by - 1 - k / 2, C_WHITE);
    }
  }

  // Claude runs the crane with a remote control; cheers after each block
  bool cheer  = (n < 6 && ph >= 112 && ph < 136) || n == 6;
  bool active = n < 6 && ph < 136;                       // crane is moving
  int dy = cheer ? -(int)(fabsf(sinf(f * 0.3f)) * 3) : 0;
  int wave = cheer && (f / 4) % 2;
  claudeFB(22, 43 + dy, false, cheer ? -3 : 0);
  int rx = 47, ry = 36 + dy + (cheer ? -3 : 0);
  display.fillRect(rx, ry, 7, 8, C_WHITE);               // remote
  display.fillRect(rx + 2, ry + 2, 3, 2, C_BLACK);       // its little screen
  display.drawPixel(rx + 3, ry + 5, C_BLACK);            // button
  display.drawFastVLine(rx + 5, ry - 7, 7, C_WHITE);     // antenna
  display.fillRect(rx + 4, ry - 8, 3, 2, C_WHITE);
  if (active && (f / 4) % 2) {                           // signal waves
    display.drawLine(rx + 8, ry - 10, rx + 9, ry - 8, C_WHITE);
    display.drawLine(rx + 9, ry - 8, rx + 8, ry - 6, C_WHITE);
    if ((f / 8) % 2) {
      display.drawLine(rx + 11, ry - 12, rx + 12, ry - 8, C_WHITE);
      display.drawLine(rx + 12, ry - 8, rx + 11, ry - 4, C_WHITE);
    }
  }
  if (n == 6) {
    display.drawFastVLine(SX, 20, 13, C_WHITE);              // flag on the roof
    display.fillRect(SX + 1, 20 + wave, 7, 4, C_WHITE);
    sparkle(88, 36, f, 3);
    sparkle(124, 30, f + 5, 2);
  }

  // sounds, in step with the machine
  if (n < 6) {
    bool moving = ph < 22 || (ph >= 64 && ph < 94);
    if (moving && ph % 6 == 0)  amb(700, 700, 5, 12);      // trolley motor
    else if (ph == 22 || ph == 94) amb(900, 650, 400, 14); // lowering
    else if (ph == 40)          amb(1200, 900, 25, 30);    // hook on
    else if (ph == 46)          amb(650, 950, 400, 14);    // hoist
    else if (ph == 112)         fx(500, 280, 45, 45);      // clunk onto the building
    else if (ph == 128)         amb(700, 1000, 30, 20);    // next block delivered
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
    // Skilled AI: a flap (-2.0) rises ~9 px, so flap the moment the bird's
    // feet get within 3 px of the lower pipe — it then peaks just under the top.
    if (lazy) lazy--;
    else if (random(10000) < 4) lazy = 14;         // a very rare daydream
    int gapBottom = gy[nx] + GAP;
    if (!lazy && vy >= 0 && y + 10 >= gapBottom - 3) { vy = -2.0f; amb(900, 1300, 18, 18); }
    vy += 0.2f; if (vy > 2.8f) vy = 2.8f;
    y += vy;    if (y < 9) { y = 9; vy = 0; }

    for (uint8_t i = 0; i < 2; i++) {
      px[i] -= 1.3f;
      if (px[i] < -PW - 2) {                        // next gap always reachable from the last
        px[i] += 140;
        gy[i] = constrain(gy[1 - i] + (int)random(-12, 13), 12, 34);
        scored[i] = false;
      }
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
   RECORD PLAYER — Claude listening with headphones; the buttons
   really control the music on the laptop (via the bridge)
   ============================================================ */
void musicButton(uint8_t b) {                     // 1 prev, 2 play/pause, 3 next
  sendToLaptop(b == 1 ? "P:prev" : b == 2 ? "P:toggle" : "P:next");
  pressedBtn = b; pressedAt = millis();
  if (b == 2) {                                   // flip right away; the bridge confirms
    if (musicOn) { musicOn = false; musicPaused = true; musicLevel = 0; }
    else         { musicOn = true;  musicPaused = false; }
    musicAt = millis();
  }
  if (soundOn) { sndClear(); sndPushV(1500, 1500, 15, 0, 45); }
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

/* a clean "now playing": the record on the left; song, artist, a thin
   progress line and three simple buttons on the right. Nothing else. */

/* dashed ring whose dashes travel round with the record: reads as spinning */
void grooveRing(int cx, int cy, int r, float ang) {
  int n = (int)(2 * PI * r);
  int shift = (int)(ang * r);                     // same turning speed on every ring
  for (int k = 0; k < n; k++) {
    if (((k + shift) / 3) % 3 == 0) continue;     // gaps between dashes
    float a = k * 2 * PI / n;
    display.drawPixel(cx + (int)lroundf(cosf(a) * r), cy + (int)lroundf(sinf(a) * r), C_WHITE);
  }
}

void sc_player(uint32_t f) {
  const int RX = 64;                              // right column starts here

  // song + artist ("Song - Artist"); the song glides along if it's long
  char song[44], artist[44] = "";
  strncpy(song, musicTitle[0] ? musicTitle : "music", sizeof(song) - 1);
  song[sizeof(song) - 1] = 0;
  char* dash = strstr(song, " - ");
  if (dash) { strncpy(artist, dash + 3, sizeof(artist) - 1); artist[sizeof(artist) - 1] = 0; *dash = 0; }
  int w = (int)strlen(song) * 6;
  if (w <= 128 - RX) txt(RX, 10, 1, song);
  else {
    int span = w + 30, off = (int)((f / 3) % span);
    txt(RX - off, 10, 1, song);
    txt(RX - off + span, 10, 1, song);
  }
  if (strlen(artist) > 10) { artist[8] = '.'; artist[9] = '.'; artist[10] = 0; }
  txt(RX, 21, 1, artist);
  display.fillRect(0, 8, RX - 1, 12, C_BLACK);    // the scrolling song stays in its column

  // thin progress line with a small playhead (only when the player shares it)
  if (trackDur) {
    uint32_t pos = trackPos + (musicOn ? (millis() - trackAt) / 1000 : 0);
    if (pos > trackDur) pos = trackDur;
    int len = 126 - RX, fw = (int)(len * pos / trackDur);
    for (int x = RX + fw; x < RX + len; x += 2) display.drawPixel(x, 35, C_WHITE);   // to go
    display.drawFastHLine(RX, 35, fw, C_WHITE);                                       // played
    display.fillRect(RX + fw - 1, 34, 3, 3, C_WHITE);                                 // playhead
  }

  // three simple buttons; the one you just used flashes
  static const int BX[3] = { 72, 95, 117 };
  for (uint8_t b = 1; b <= 3; b++) {
    bool lit = pressedBtn == b && millis() - pressedAt < 300;
    if (lit) display.fillRoundRect(BX[b - 1] - 8, 45, 17, 11, 3, C_WHITE);
    btnIcon(b, BX[b - 1], 50, lit ? C_BLACK : C_WHITE);
  }

  // the record: black vinyl, bright rim, grooves whose dashes travel round
  const int cx = 30, cy = 32;
  display.drawCircle(cx, cy, 28, C_WHITE);
  display.drawCircle(cx, cy, 27, C_WHITE);
  grooveRing(cx, cy, 24, discAngle);
  grooveRing(cx, cy, 20, discAngle);
  grooveRing(cx, cy, 16, discAngle);
  for (int r = 13; r <= 25; r++) {                // glossy sheen (the light doesn't turn)
    display.drawPixel(cx + (int)(r * 0.70f), cy - (int)(r * 0.70f), C_WHITE);
    display.drawPixel(cx - (int)(r * 0.70f), cy + (int)(r * 0.70f), C_WHITE);
  }

  // the label, with Claude's star turning on it
  display.fillCircle(cx, cy, 10, C_WHITE);
  for (uint8_t k = 0; k < 3; k++) {
    float a = discAngle + k * PI / 3;
    int dx = (int)lroundf(cosf(a) * 7), dy = (int)lroundf(sinf(a) * 7);
    display.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, C_BLACK);
    display.drawLine(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, C_BLACK);
  }
  display.fillCircle(cx, cy, 1, C_WHITE);         // spindle

  // tonearm: pivot, arm, angled headshell; needle in the grooves while playing
  display.fillCircle(60, 6, 3, C_WHITE);
  display.fillCircle(60, 6, 1, C_BLACK);
  int ex = musicOn ? 55 : 62, ey = musicOn ? 24 : 27;                 // elbow
  int tx = musicOn ? 47 : 60, ty = musicOn ? 22 : 34;                 // needle
  display.drawLine(60, 8, ex, ey, C_WHITE);
  display.drawLine(61, 8, ex + 1, ey, C_WHITE);
  display.drawLine(ex, ey, tx, ty, C_WHITE);
  display.fillRect(tx - 2, ty - 1, 4, 3, C_WHITE);
}

/* ============================================================
   CLOCK — big time, the date and a greeting (time from the laptop)
   ============================================================ */
void sc_clock(uint32_t f) {
  static const char* DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char* MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char b[24];
  if (!timeKnown) {
    txtC(8, 3, "--:--");
    txtC(38, 1, "start the bridge");
    txtC(48, 1, "to get the time");
    return;
  }
  int h12 = tHour % 12 == 0 ? 12 : tHour % 12;
  snprintf(b, sizeof(b), "%d%c%02d", h12, (tSec % 2) ? ':' : ' ', tMin);   // blinking colon
  int w = (int)strlen(b) * 18;
  int x0 = (128 - w - 14) / 2;
  txt(x0, 6, 3, b);
  txt(x0 + w + 2, 6, 1, tHour < 12 ? "AM" : "PM");
  for (int s = 0; s < 60; s += 5)                       // seconds ticking along under the time
    display.drawPixel(4 + s * 2, 31, C_WHITE);
  display.fillRect(4 + tSec * 2, 30, 2, 3, C_WHITE);

  snprintf(b, sizeof(b), "%s %02d %s %d", DAYS[tWday % 7], tDay, MONTHS[(tMon + 11) % 12], tYear);
  txtC(36, 1, b);

  const char* hi = tHour < 5 ? "good night" : tHour < 12 ? "good morning" :
                   tHour < 17 ? "good afternoon" : tHour < 21 ? "good evening" : "good night";
  miniClaude(10, 50 + (int)wobble(f, 0.1f, 1), false);
  txt(34, 52, 1, hi);
}

/* ============================================================
   SUPER CLAUDE — a side-scrolling platformer Claude plays itself.
   Fresh level every time: gaps, pipes, ? blocks, coins, stairs,
   bugs to stomp, then the flagpole, the castle and fireworks.
   (original art & music, Mario-style)
   ============================================================ */
#define MLEN       72                 // level length in 8 px tiles
#define T_GROUND   1
#define T_BRICK    2
#define T_QBLOCK   3
#define T_USED     4
#define T_HARD     5
#define T_PIPE_TL  6
#define T_PIPE_TR  7
#define T_PIPE_B   8
#define T_COIN     9
#define T_POLE     10
#define T_CASTLE   11

uint8_t mt[MLEN][8];                  // the level: [column][row], row 0 = HUD
struct Foe { float x, y, vx; uint8_t st, t; };     // st: 0 gone, 1 walking, 2 squashed
Foe     foes[6];
uint8_t mFlagCol = 0, mCastleCol = 0;

void mGround(int c) { if (c >= 0 && c < MLEN) mt[c][7] = T_GROUND; }

void marioGen() {
  memset(mt, 0, sizeof(mt));
  for (uint8_t i = 0; i < 6; i++) foes[i].st = 0;
  uint8_t nf = 0;
  int c = 0;
  for (; c < 8; c++) mGround(c);
  while (c < MLEN - 26) {
    switch (random(6)) {
      case 0:                                     // a gap to jump
        c += random(2, 4);
        for (int k = 0; k < 3; k++) mGround(c++);
        break;
      case 1: {                                   // a pipe
        int h = random(2, 4), pc = c + 2;
        for (int k = 0; k < 6; k++) mGround(c + k);
        for (int y = 7 - h; y < 7; y++) {
          mt[pc][y]     = y == 7 - h ? T_PIPE_TL : T_PIPE_B;
          mt[pc + 1][y] = y == 7 - h ? T_PIPE_TR : T_PIPE_B;
        }
        c += 6;
        break;
      }
      case 2:                                     // bricks with ? blocks
        for (int k = 0; k < 7; k++) mGround(c + k);
        for (int k = 0; k < 5; k++) mt[c + 1 + k][3] = (k == 1 || k == 3) ? T_QBLOCK : T_BRICK;
        c += 7;
        break;
      case 3:                                     // a bug walking about
        for (int k = 0; k < 8; k++) mGround(c + k);
        if (nf < 6) { foes[nf].x = (c + 6) * 8; foes[nf].y = 49; foes[nf].vx = -0.45f; foes[nf].st = 1; nf++; }
        mt[c + 2][4] = mt[c + 3][4] = T_COIN;
        c += 8;
        break;
      case 4: {                                   // a little staircase
        static const uint8_t H[] = { 1, 2, 3, 3, 2, 1 };
        for (int k = 0; k < 9; k++) mGround(c + k);
        for (int i = 0; i < 6; i++)
          for (int y = 7 - H[i]; y < 7; y++) mt[c + 1 + i][y] = T_HARD;
        c += 9;
        break;
      }
      default:                                    // coins in the air
        for (int k = 0; k < 7; k++) mGround(c + k);
        for (int k = 1; k < 5; k++) mt[c + k][4] = T_COIN;
        c += 7;
    }
  }
  for (; c < MLEN; c++) mGround(c);
  int s = MLEN - 20;                              // the finale: steps, flagpole, castle
  for (int i = 0; i < 3; i++) for (int y = 6 - i; y < 7; y++) mt[s + i][y] = T_HARD;
  mFlagCol = MLEN - 12;
  mt[mFlagCol][6] = T_HARD;
  for (int y = 1; y < 6; y++) mt[mFlagCol][y] = T_POLE;
  mCastleCol = MLEN - 6;
  mt[mCastleCol][6] = T_CASTLE;
}

bool mSolid(int px, int py) {
  if (px < 0) return true;
  if (py < 0) return false;
  int tx = px >> 3, ty = py >> 3;
  if (tx >= MLEN || ty > 7) return false;
  uint8_t t = mt[tx][ty];
  return t == T_GROUND || t == T_BRICK || t == T_QBLOCK || t == T_USED || t == T_HARD ||
         t == T_PIPE_TL || t == T_PIPE_TR || t == T_PIPE_B;
}

void mTile(uint8_t t, int x, int y, uint32_t f) {
  switch (t) {
    case T_GROUND:
      display.fillRect(x, y, 8, 8, C_WHITE);
      display.drawPixel(x + 2, y + 2, C_BLACK); display.drawPixel(x + 6, y + 5, C_BLACK);
      break;
    case T_BRICK:
      display.fillRect(x, y, 8, 8, C_WHITE);
      display.drawFastHLine(x, y + 3, 8, C_BLACK); display.drawFastHLine(x, y + 7, 8, C_BLACK);
      display.drawFastVLine(x + 3, y, 3, C_BLACK); display.drawFastVLine(x + 7, y + 4, 3, C_BLACK);
      break;
    case T_QBLOCK:                                 // ? block, glinting
      display.drawRect(x, y, 8, 8, C_WHITE);
      if ((f / 10) % 4) {
        display.drawFastHLine(x + 3, y + 1, 2, C_WHITE);
        display.drawPixel(x + 5, y + 2, C_WHITE); display.drawPixel(x + 4, y + 3, C_WHITE);
        display.drawPixel(x + 4, y + 4, C_WHITE); display.drawPixel(x + 4, y + 6, C_WHITE);
      }
      break;
    case T_USED:
      display.drawRect(x, y, 8, 8, C_WHITE);
      display.drawPixel(x + 2, y + 2, C_WHITE); display.drawPixel(x + 5, y + 5, C_WHITE);
      break;
    case T_HARD:
      display.fillRect(x, y, 8, 8, C_WHITE);
      display.drawRect(x + 2, y + 2, 4, 4, C_BLACK);
      break;
    case T_PIPE_TL:                                // the whole pipe is drawn from its top-left
      display.fillRect(x - 1, y, 18, 5, C_BLACK);
      display.drawRect(x - 1, y, 18, 5, C_WHITE);
      display.drawFastVLine(x + 1, y + 5, 56 - (y + 5), C_WHITE);
      display.drawFastVLine(x + 14, y + 5, 56 - (y + 5), C_WHITE);
      for (int yy = y + 6; yy < 56; yy += 2) display.drawPixel(x + 4, yy, C_WHITE);
      break;
    case T_COIN: {                                 // spinning coin
      static const uint8_t SPIN[] = { 4, 3, 1, 3 };
      uint8_t w = SPIN[(f / 5) % 4];
      display.fillRoundRect(x + 4 - w / 2, y + 1, w, 6, 1, C_WHITE);
      break;
    }
    case T_POLE:
      display.drawFastVLine(x + 4, y, 8, C_WHITE);
      if (y == 8) display.fillCircle(x + 4, 9, 2, C_WHITE);
      break;
    case T_CASTLE:                                 // drawn from its bottom-left tile
      display.drawRect(x, 40, 24, 16, C_WHITE);
      for (int k = 0; k < 3; k++) display.fillRect(x + 1 + k * 8, 36, 5, 4, C_WHITE);
      display.drawRect(x + 6, 28, 12, 12, C_WHITE);
      display.fillRect(x + 6, 25, 3, 3, C_WHITE); display.fillRect(x + 15, 25, 3, 3, C_WHITE);
      display.fillRect(x + 11, 31, 2, 4, C_WHITE);
      display.drawRoundRect(x + 9, 45, 6, 11, 3, C_WHITE);
      break;
  }
}

void mClaude(int x, int y, bool air, bool dead, uint32_t f) {   // 12x10 runner
  display.fillRect(x + 1, y, 10, 6, C_WHITE);
  display.fillRect(x, y + (air ? 0 : 2), 12, 2, C_WHITE);       // arms up when jumping
  if (dead) {
    display.drawLine(x + 2, y + 1, x + 4, y + 3, C_BLACK); display.drawLine(x + 4, y + 1, x + 2, y + 3, C_BLACK);
    display.drawLine(x + 7, y + 1, x + 9, y + 3, C_BLACK); display.drawLine(x + 9, y + 1, x + 7, y + 3, C_BLACK);
  } else {
    display.drawFastVLine(x + 3, y + 2, 2, C_BLACK);
    display.drawFastVLine(x + 8, y + 2, 2, C_BLACK);
  }
  int a = air ? 1 : ((f / 4) % 2) * 2;                          // scissoring legs
  display.fillRect(x + 2 + a, y + 6, 2, 4, C_WHITE);
  display.fillRect(x + 8 - a, y + 6, 2, 4, C_WHITE);
}

void sc_hello(uint32_t f) {
  int x = 51, y = 36;
  claudeFB(x, y, false, 0);
  display.fillRect(x + 26, y + 4, 5, 5, C_BLACK);          // lift the right arm...
  float a = -1.1f + sinf(f * 0.3f) * 0.5f;                 // ...and wave it
  int ex = x + 27 + (int)(cosf(a) * 10), ey = y + 6 + (int)(sinf(a) * 10);
  for (int o = -1; o <= 1; o++) display.drawLine(x + 26, y + 6 + o, ex, ey + o, C_WHITE);
  display.drawRoundRect(84, 6, 32, 14, 4, C_WHITE);
  display.fillTriangle(88, 19, 94, 19, 86, 24, C_WHITE);
  txt(91, 9, 1, "hi!");
  for (int xx = 0; xx < 128; xx += 2) display.drawPixel(xx, 56, C_WHITE);
}

void sc_mario(uint32_t f) {
  static uint32_t enter = 0xFFFFFFFF;
  static bool     restart = false, onGround = false;
  static float    x, y, vx, vy, flagY;
  static uint8_t  st, lives, world, hesitate, inv, bumpT;   // st: 0 title 1 play 2 dead 3 flag 4 walk 5 castle 6 over
  static uint16_t t, coins, overT;
  static uint32_t score;
  static int      camX, bumpC, bumpR, popX[4], popY[4];
  static uint8_t  popT[4];
  const float RUN = 1.25f, G = 0.35f, JUMP = -4.6f;

  if (enter != sceneTick0 || restart) {
    enter = sceneTick0; restart = false;
    lives = 3; world = 1; score = 0; coins = 0; overT = 0;
    marioGen();
    st = 0; t = 0; x = 16; y = 46; vx = vy = 0; camX = 0; flagY = 12; inv = 0; bumpT = 0;
    for (uint8_t i = 0; i < 4; i++) popT[i] = 0;
  }
  t++;

  // player physics against the tiles; bumping ? blocks from below
  auto physics = [&]() {
    float nx = x + vx;
    if (vx > 0 && (mSolid((int)nx + 11, (int)y + 1) || mSolid((int)nx + 11, (int)y + 9)))
      nx = (float)((((int)nx + 11) >> 3) * 8 - 12);
    x = nx;
    vy += G; if (vy > 4.5f) vy = 4.5f;
    float ny = y + vy;
    onGround = false;
    if (vy >= 0) {
      if (mSolid((int)x + 2, (int)ny + 10) || mSolid((int)x + 9, (int)ny + 10)) {
        ny = (float)((((int)ny + 10) >> 3) * 8 - 10); vy = 0; onGround = true;
      }
    } else if (ny >= 8 && (mSolid((int)x + 2, (int)ny) || mSolid((int)x + 9, (int)ny))) {
      int bc = ((int)x + 6) >> 3, br = (int)ny >> 3;
      if (bc < MLEN && mt[bc][br] == T_QBLOCK) {
        mt[bc][br] = T_USED; coins++; score += 200;
        for (uint8_t i = 0; i < 4; i++) if (!popT[i]) { popT[i] = 14; popX[i] = bc * 8; popY[i] = br * 8 - 8; break; }
        fx2(1976, 2637, 40, 45);
      } else {
        fx(300, 250, 30, 40);
      }
      bumpC = bc; bumpR = br; bumpT = 6;
      ny = (float)((br + 1) * 8); vy = 0.5f;
    }
    y = ny;
    // coins
    for (int k = 0; k < 4; k++) {
      int cx = ((int)x + (k % 2 ? 9 : 2)) >> 3, cy = ((int)y + (k < 2 ? 2 : 8)) >> 3;
      if (cx < MLEN && cy >= 1 && cy < 8 && mt[cx][cy] == T_COIN) {
        mt[cx][cy] = 0; coins++; score += 100; fx2(1976, 2637, 40, 40);
      }
    }
  };

  auto die = [&]() { st = 2; t = 0; vy = -4.2f; fx(784, 180, 600, 45); };

  switch (st) {
    case 0:                                                    // WORLD 1-1 title card
      if (t == 2 && ambOk()) { sndPushV(1047, 1047, 60, 20, 35); sndPushV(1319, 1319, 60, 20, 35); sndPushV(1568, 1568, 120, 0, 35); }
      if (t > 50) { st = 1; t = 0; }
      break;

    case 1: {                                                  // running the level
      if (onGround) {                                          // Claude reads what's ahead
        int front = (int)x + 12, feet = (int)y + 10;
        bool hole = false, wall = false, foe = false, qb = false;
        for (int dx = 0; dx <= 3; dx++)                        // jump right at the edge: clears 3-wide gaps
          if (!mSolid(front + dx, feet + 2) && !mSolid(front + dx, 60)) hole = true;
        for (int dx = 1; dx <= 14; dx++)
          if (mSolid(front + dx, (int)y + 2) || mSolid(front + dx, (int)y + 8)) wall = true;
        for (uint8_t i = 0; i < 6; i++)
          if (foes[i].st == 1) {
            float d = foes[i].x - front;
            if (d > 2 && d < 22 && fabsf(foes[i].y - y) < 12) foe = true;
          }
        int hc = ((int)x + 6) >> 3, hr = ((int)y >> 3) - 1;
        for (int r = hr; r >= hr - 2 && r >= 1; r--) if (hc < MLEN && mt[hc][r] == T_QBLOCK) qb = true;
        if (hole && !hesitate && random(100) < 3) hesitate = 12;   // a rare misjudged jump
        if (hesitate) hesitate--;
        if ((hole && !hesitate) || wall || foe || qb) { vy = JUMP; fx(600, 1150, 70, 40); }
      }
      vx = RUN;
      physics();
      if (inv) inv--;

      for (uint8_t i = 0; i < 6; i++) {                        // the bugs
        Foe& e = foes[i];
        if (e.st == 2) { if (--e.t == 0) e.st = 0; continue; }
        if (e.st != 1 || e.x < camX - 16 || e.x > camX + 140) continue;
        if (mSolid((int)e.x + (e.vx < 0 ? -1 : 8), (int)e.y + 3)) e.vx = -e.vx;
        e.x += e.vx;
        if (!mSolid((int)e.x + 4, (int)e.y + 8)) e.y += 2;
        if (e.y > 70) { e.st = 0; continue; }
        bool hit = x + 11 >= e.x && x <= e.x + 7 && y + 9 >= e.y && y <= e.y + 6;
        if (hit) {
          if (vy > 0 && y + 9 <= e.y + 4) {                    // stomp!
            e.st = 2; e.t = 20; vy = -3.2f; score += 100; fx(400, 150, 60, 50);
          } else if (!inv) die();
        }
      }
      if (st == 1 && y > 70) die();                            // fell in a gap
      if (st == 1 && x + 8 >= mFlagCol * 8) {                  // grabbed the flagpole
        st = 3; t = 0; x = mFlagCol * 8 - 9; vy = 0;
        score += 400 + (uint32_t)(48 - (int)y) * 40;
        fx(2093, 600, 700, 45);
      }
      break;
    }

    case 2:                                                    // oops: hop and fall away
      vy += G; y += vy;
      if (t > 70) {
        if (--lives == 0) { st = 6; sndOver(); }
        else { x = 16; y = 40; vx = vy = 0; camX = 0; st = 0; t = 0; inv = 60; }
      }
      break;

    case 3:                                                    // slide down the pole
      if (y < 38) y += 1.5f; else y = 38;
      if (flagY < 40) flagY += 1.5f;
      if (y >= 38 && flagY >= 40) { st = 4; t = 0; }
      break;

    case 4:                                                    // walk into the castle
      vx = 1; physics();
      if (x >= mCastleCol * 8 + 8) { st = 5; t = 0; }
      break;

    case 5:                                                    // fireworks!
      if (t == 1 && soundMode == SND_ALL) {
        static const uint16_t FAN[] = { 1047, 1319, 1568, 2093, 1568, 2093 };
        sndClear();
        for (uint8_t i = 0; i < 6; i++) sndPushV(FAN[i], FAN[i], i < 5 ? 70 : 220, 15, 45);
      }
      if (t == 40 || t == 60 || t == 80) fx(1500, 400, 60, 40);
      if (t > 110) {                                           // on to the next world
        world++; marioGen();
        x = 16; y = 46; vx = vy = 0; camX = 0; flagY = 12; st = 0; t = 0;
      }
      break;

    case 6:                                                    // game over
      if (gameOverTick(overT)) restart = true;
      break;
  }
  if (bumpT) bumpT--;

  int maxCam = MLEN * 8 - 128;
  camX = constrain((int)x - 40, 0, maxCam);

  // background tune while running (ALL mode)
  static const uint16_t MEL[32] = { 784, 0, 784, 988, 0, 1175, 988, 0, 784, 0, 659, 784, 0, 880, 784, 0,
                                    659, 0, 587, 659, 0, 784, 659, 0, 587, 523, 587, 659, 784, 0, 0, 0 };
  if (st == 1 && f % 8 == 0 && MEL[(f / 8) % 32]) amb(MEL[(f / 8) % 32], MEL[(f / 8) % 32], 90, 20);

  // ----- draw -----
  if (st == 0) {                                               // title card
    char b[20];
    txtC(12, 1, "SUPER CLAUDE");
    snprintf(b, sizeof(b), "WORLD 1-%u", world);
    txtC(26, 1, b);
    mClaude(48, 42, false, false, 0);
    snprintf(b, sizeof(b), "x %u", lives);
    txt(66, 43, 1, b);
    playLives = lives; playScore = (uint16_t)score;
    return;
  }

  for (uint8_t k = 0; k < 3; k++) {                            // parallax clouds
    int cx = (k * 61 + 20) - camX / 3;
    cx = ((cx % 180) + 180) % 180 - 20;
    int cy = 13 + (k % 2) * 3;                                 // high, clear of the block rows
    display.drawRoundRect(cx, cy, 16, 6, 3, C_WHITE);
    display.drawRoundRect(cx + 4, cy - 3, 8, 5, 2, C_WHITE);
  }

  int c0 = camX >> 3;
  for (int c = c0 - 2; c <= c0 + 16; c++) {
    if (c < 0 || c >= MLEN) continue;
    for (int r = 1; r < 8; r++) {
      uint8_t tt = mt[c][r];
      if (!tt || tt == T_PIPE_TR || tt == T_PIPE_B) continue;
      int yo = (bumpT && c == bumpC && r == bumpR) ? -2 : 0;  // bumped block jumps
      mTile(tt, c * 8 - camX, r * 8 + yo, f);
    }
  }
  int fpx = mFlagCol * 8 - camX;                               // the flag
  display.fillTriangle(fpx + 4, (int)flagY, fpx - 5, (int)flagY + 3, fpx + 4, (int)flagY + 6, C_WHITE);

  for (uint8_t i = 0; i < 4; i++) if (popT[i]) {               // coins popping out of ? blocks
    popT[i]--;
    int py = popY[i] - (14 - popT[i]) * 2 + (popT[i] < 5 ? (5 - popT[i]) * 3 : 0);
    display.fillRoundRect(popX[i] - camX + 2, py, 4, 6, 1, C_WHITE);
    if (popT[i] < 3) sparkle(popX[i] - camX + 4, py, f, 2);
  }

  for (uint8_t i = 0; i < 6; i++) {                            // bugs
    Foe& e = foes[i];
    int ex = (int)e.x - camX, ey = (int)e.y;
    if (e.st == 1) {
      display.fillRoundRect(ex, ey + 1, 8, 5, 2, C_WHITE);
      display.drawPixel(ex + 2, ey + 2, C_BLACK); display.drawPixel(ex + 5, ey + 2, C_BLACK);
      display.drawLine(ex + 2, ey + 1, ex + 1, ey - 1, C_WHITE);
      display.drawLine(ex + 5, ey + 1, ex + 6, ey - 1, C_WHITE);
      int l = (f / 4) % 2;
      display.drawPixel(ex + 1 + l, ey + 6, C_WHITE); display.drawPixel(ex + 6 - l, ey + 6, C_WHITE);
    } else if (e.st == 2) {
      display.fillRect(ex, ey + 5, 8, 2, C_WHITE);
    }
  }

  if (st != 5 && !(inv && (f / 3) % 2))                        // Claude
    mClaude((int)x - camX, (int)y, !onGround || st == 2, st == 2, f);
  if (st == 5) {                                               // fireworks over the castle
    int cxp = mCastleCol * 8 - camX + 12;
    if (t > 40) sparkle(cxp - 14, 14, f, 3);
    if (t > 60) sparkle(cxp + 12, 10, f + 4, 3);
    if (t > 80) sparkle(cxp, 6, f + 8, 4);
  }

  // HUD: score, coins, world, lives
  char b[12];
  display.fillRect(0, 0, 128, 8, C_BLACK);
  snprintf(b, sizeof(b), "%06lu", (unsigned long)(score % 1000000));
  txt(0, 0, 1, b);
  display.fillRoundRect(42, 0, 4, 7, 1, C_WHITE);
  snprintf(b, sizeof(b), "x%02u", coins % 100);
  txt(48, 0, 1, b);
  snprintf(b, sizeof(b), "1-%u", world);
  txt(74, 0, 1, b);
  heartS(104, 1, true);
  snprintf(b, sizeof(b), "%u", lives);
  txt(111, 0, 1, b);

  if (st == 6) gameOverBox((uint16_t)score);
  playLives = lives; playScore = (uint16_t)score;
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
  static const uint8_t L[] = { 1, 2, 4, 5, 7, 8, 9, 10, 33, 34, 41, 54 };
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

/* Claude Code's spinner glyphs:  ·  ✢  ✳  ✶  ✻  ✽ */
void starArms(int cx, int cy, uint8_t n, int r, bool thick) {
  for (uint8_t k = 0; k < n / 2; k++) {
    float a = k * PI / (n / 2) + PI / 2;
    int dx = (int)lroundf(cosf(a) * r), dy = (int)lroundf(sinf(a) * r);
    display.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, C_WHITE);
    if (thick) {
      display.drawLine(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, C_WHITE);
      display.drawLine(cx - dx, cy - dy + 1, cx + dx, cy + dy + 1, C_WHITE);
    }
  }
}

void spinGlyph(uint8_t k, int cx, int cy) {
  switch (k) {
    case 0:  display.fillCircle(cx, cy, 1, C_WHITE); break;                       // ·
    case 1:  starArms(cx, cy, 4, 3, false);                                       // ✢
             for (int s = -1; s <= 1; s += 2) {
               display.drawPixel(cx + s * 2, cy + 2, C_WHITE);
               display.drawPixel(cx + s * 2, cy - 2, C_WHITE);
             }
             break;
    case 2:  starArms(cx, cy, 8, 5, false); break;                                // ✳
    case 3:  starArms(cx, cy, 6, 6, false); display.fillCircle(cx, cy, 1, C_WHITE); break;  // ✶
    case 4:  starArms(cx, cy, 6, 7, true);  display.fillCircle(cx, cy, 2, C_WHITE); break;  // ✻
    default: starArms(cx, cy, 8, 8, true);  display.fillCircle(cx, cy, 2, C_WHITE); break;  // ✽
  }
}

void sc_working(uint32_t f) {
  // the spinner breathes through its glyphs and back, like Claude Code's
  static const uint8_t SEQ[] = { 0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0 };
  spinGlyph(SEQ[(f / 4) % sizeof(SEQ)], 13, 13);

  // "Tinkering..." with a shimmer sweeping underneath
  char b[24], d[12];
  snprintf(b, sizeof(b), "%s...", VERBS[verbIdx]);
  txt(28, 6, 1, b);
  int n = (int)strlen(b), sh = (int)((f / 2) % (n + 6)) - 2;
  if (sh >= 0 && sh < n) display.drawFastHLine(28 + sh * 6, 15, 12, C_WHITE);

  fmtDur(d, sizeof(d), busy ? millis() - busyStart : 0);
  snprintf(b, sizeof(b), "(%s)", d);
  txt(28, 19, 1, b);
  if (busySteps) {
    snprintf(b, sizeof(b), "%u step%s", busySteps, busySteps == 1 ? "" : "s");
    txt(28, 30, 1, b);
  }

  // eyes: glance at the text, look around, squint when thinking hard,
  // and pop wide the moment a tool fires
  bool fired = lastToolAt && millis() - lastToolAt < 700;
  uint8_t phase = (f / 90) % 3;
  int look = (phase == 1 ? -3 : phase == 2 ? 4 : 0) + (int)wobble(f, 0.05f, 2);
  uint8_t style = fired ? EY_WIDE : ((f / 160) % 4 == 3 ? EY_SUSPICIOUS : EY_NORMAL);
  eyes(101 + look, 38, 12, fired ? 20 : 16, 12, style, 3);
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

/* --- real shell: types out the command Claude is running ----- */
void sc_shell(uint32_t f) {
  uint32_t lt = f - sceneTick0;
  winBox(0, 0, 128, 64, shellPs ? "PowerShell" : "bash");

  const char* cmd = ticker;
  if (cmd[0] == '$' && cmd[1] == ' ') cmd += 2;
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %s", shellPs ? "PS C:\\>" : "$", cmd);

  size_t len = strlen(buf), shown = lt < len ? lt : len;
  const int COLS = 20;
  int row = 0, col = 0;
  char c[2] = { 0, 0 };
  for (size_t i = 0; i < shown && row < 4; i++) {
    c[0] = buf[i];
    txt(3 + col * 6, 13 + row * 10, 1, c);
    if (++col >= COLS) { col = 0; row++; }
  }
  if (row < 4 && (shown < len || (f / 8) % 2))           // cursor: solid while typing
    display.fillRect(3 + col * 6, 13 + row * 10, 5, 8, C_WHITE);
  if (shown >= len) {
    static const char SPIN[] = "|/-\\";
    char b[16];
    snprintf(b, sizeof(b), "%c running", SPIN[(f / 4) % 4]);
    txt(3, 53, 1, b);
  }
}

/* ============================================================
   CLAUDE CHARACTER SCENES  (the full-body Claude as the actor)
   ============================================================ */
void dline(int x0, int y0, int x1, int y1) {          // dotted line
  int steps = max(abs(x1 - x0), abs(y1 - y0));
  if (!steps) { display.drawPixel(x0, y0, C_WHITE); return; }
  for (int s = 0; s <= steps; s += 2)
    display.drawPixel(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, C_WHITE);
}

void fakeHash(char* b, uint8_t seed) {                 // stable made-up hash
  uint32_t h = seed * 2654435761UL + 0x9E37;
  for (uint8_t i = 0; i < 7; i++) { b[i] = "0123456789abcdef"[h & 15]; h = h * 1103515245UL + 12345; h ^= h >> 7; }
  b[7] = 0;
}

/* git log beside Claude. commit: newest hash types in, Claude hops.
   merge: a dot rides the side branch back into main.               */
void gitLogScene(uint32_t f, bool merge) {
  uint32_t lt = (f - sceneTick0) % 120;
  const int X0 = 6, X1 = 16;
  const uint8_t COLS = 10;                                   // text fits between graph and Claude

  // how many real commits we have (no data yet: a made-up 7-row history)
  uint8_t rows = 7;
  if (gitKnown) { rows = 0; while (rows < GIT_ROWS && gHash[rows][0]) rows++; if (!rows) rows = 1; }
  bool branchy = rows == 7;                                  // room for the side-branch shape
  int bs = merge ? 4 : 3, be = merge ? 5 : 4, mr = be + 1;   // branch rows, merge row
  if (!branchy) bs = be = mr = 99;
  char t[COLS + 3];

  for (int i = 0; i < rows; i++) {
    int y = 5 + i * 9;
    bool side = i >= bs && i <= be;
    int x = side ? X1 : X0;
    if (i < rows - 1) {
      int nx = (i + 1 >= bs && i + 1 <= be) ? X1 : X0;
      dline(x, y, nx, y + 9);
      if (side || i == bs - 1) dline(X0, y, X0, y + 9);      // main keeps going
    }
    if (i == 0)       display.fillRect(x - 2, y - 2, 5, 5, C_WHITE);           // HEAD
    else if (i == mr) {                                                       // merge commit
      bool flash = merge && lt >= 60 && lt < 72 && (lt / 3) % 2;
      if (flash) display.fillCircle(x, y, 3, C_WHITE); else display.drawCircle(x, y, 3, C_WHITE);
    }
    else              display.fillRect(x - 1, y - 1, 3, 3, C_WHITE);

    // the commit message (falls back to the hash, or a made-up one before data arrives)
    const char* s;
    char h[8];
    if (gitKnown && gSubj[i][0])      s = gSubj[i];
    else if (gitKnown && gHash[i][0]) s = gHash[i];
    else                              { fakeHash(h, i * 7 + 3); s = h; }
    size_t n = strlen(s);
    if (n > COLS) { memcpy(t, s, COLS - 1); t[COLS - 1] = '.'; t[COLS] = '.'; t[COLS + 1] = 0; n = COLS + 1; }
    else          { strcpy(t, s); }
    if (!merge && i == 0 && lt / 3 < n) t[lt / 3] = 0;       // newest commit types itself in
    txt(24, y - 3, 1, t);
  }

  if (merge && branchy && lt < 60) {                          // dot riding the branch
    float k = lt / 60.0f;
    int ya = 5 + (bs - 1) * 9, yb = 5 + mr * 9;
    int dy = (int)(ya + (yb - ya) * k);
    int dx = (k < 0.2f) ? (int)(X0 + (X1 - X0) * k / 0.2f) : (k > 0.8f) ? (int)(X1 - (X1 - X0) * (k - 0.8f) / 0.2f) : X1;
    display.fillCircle(dx, dy, 2, C_WHITE);
  }

  bool cheer = merge ? (lt >= 60 && lt < 80) : (lt >= 30 && lt < 44);
  int hop = cheer ? -(int)(sinf((lt % 14) / 14.0f * PI) * 5) : 0;
  txt(92, 14, 1, merge ? "merge" : "commit");
  claudeFB(98, 40 + hop, false, cheer ? -3 : 0);
  if (cheer) sparkle(116, 30, f, 2);
}
void sc_commitLog(uint32_t f) { gitLogScene(f, false); }
void sc_mergeLog(uint32_t f)  { gitLogScene(f, true); }

/* Claude winds up and throws a paper plane */
/* Claude launches a dart from its shoulder; it glides off almost level,
   then the sky stays empty a moment before the next one */
void sc_planeThrow(uint32_t f) {
  uint32_t t = (f - sceneTick0) % 130;
  bool launch = t >= 10 && t < 22;
  int bob = t >= 70 ? (int)wobble(f, 0.15f, 1) : 0;      // watching it go
  claudeFB(12, 37 + bob, false, launch ? -3 : 0);

  int px = 34, py = 27;                                  // resting on Claude's shoulder
  if (t >= 10) {
    px = 34 + (int)((t - 10) * 1.9f);
    py = 27 - (int)((t - 10) / 14) + (int)(sinf(t * 0.2f) * 1.2f);
  }
  if (px < 130) {
    display.fillTriangle(px, py, px, py + 7, px + 15, py + 3, C_WHITE);   // dart
    display.drawLine(px + 1, py + 4, px + 11, py + 3, C_BLACK);           // fold
    if (t >= 10) {                                       // speed dots
      display.drawPixel(px - 4 - (int)(t % 3), py + 4, C_WHITE);
      display.drawPixel(px - 10 - (int)(t % 4), py + 3, C_WHITE);
      display.drawPixel(px + 19 + (int)(t % 2), py + 3, C_WHITE);
    }
    if (launch) sparkle(px + 17, py - 3, f, 2);
  }
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 57, C_WHITE);
}

/* three-object cascade juggle */
void sc_juggle3(uint32_t f) {
  int arm = ((f / 10) % 2) ? -1 : 1;
  claudeFB(51, 38, false, arm);
  for (uint8_t i = 0; i < 3; i++) {
    uint32_t p = (f + i * 20) % 60;
    float k = (p % 30) / 30.0f;
    int x = p < 30 ? 48 + (int)(k * 32) : 80 - (int)(k * 32);
    int y = 40 - (int)(sinf(k * PI) * (p < 30 ? 30 : 12));
    if (i == 0) display.drawRect(x - 2, y - 2, 5, 5, C_WHITE);
    else if (i == 1) {
      display.drawLine(x, y - 3, x + 3, y, C_WHITE); display.drawLine(x + 3, y, x, y + 3, C_WHITE);
      display.drawLine(x, y + 3, x - 3, y, C_WHITE); display.drawLine(x - 3, y, x, y - 3, C_WHITE);
    }
    else display.drawCircle(x, y, 2, C_WHITE);
  }
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 58, C_WHITE);
}

/* Claude lying down for a nap, breathing slowly */
void sc_napClaude(uint32_t f) {
  int x = 40, y = 42, b = (f / 24) % 2;
  display.fillRect(x, y + 3 - b, 32, 10 + b, C_WHITE);
  display.fillRect(x - 3, y + 9, 38, 3, C_WHITE);
  display.drawFastHLine(x + 8,  y + 7, 4, C_BLACK);      // closed eyes
  display.drawFastHLine(x + 20, y + 7, 4, C_BLACK);
  for (uint8_t i = 0; i < 4; i++) display.fillRect(x + 2 + i * 8, y + 13, 2, 2, C_WHITE);
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t p = (f + i * 14) % 42;
    display.setTextSize(i == 2 ? 2 : 1);
    display.setCursor(x + 36 + i * 6, y - 4 - p / 3);
    display.print('z');
  }
  display.setTextSize(1);
  display.fillCircle(110, 10, 6, C_WHITE);
  display.fillCircle(107, 8, 6, C_BLACK);
  for (uint8_t i = 0; i < 6; i++) if (((f / 8) + i) % 4) display.drawPixel((i * 37) % 90 + 4, (i * 13) % 20 + 3, C_WHITE);
  for (int xx = 0; xx < 128; xx += 2) display.drawPixel(xx, 58, C_WHITE);
}

/* Claude waving hello */

/* ============================================================
   LYRICS — Z:<ms>,<line> from the bridge (lrclib.net synced lyrics).
   Claude sings along: mouth opens with the volume, notes float up,
   and the line fills karaoke-style over <ms>.
   ============================================================ */
char     lyricText[64] = "";
uint16_t lyricMs  = 0;
uint32_t lyricAt  = 0;
bool     lyricsOn = false;

void noteGlyph(int x, int y) {                     // a little eighth note, head at x,y
  display.fillCircle(x, y, 2, C_WHITE);
  display.drawFastVLine(x + 2, y - 8, 8, C_WHITE);
  display.drawLine(x + 2, y - 8, x + 5, y - 5, C_WHITE);
}

/* a tiny Claude, x = middle, y = where its feet touch; arms up on the beat */
void singClaude(int x, int y, bool armsUp) {
  display.fillRect(x - 6, y - 9, 13, 6, C_WHITE);                 // body
  int ay = armsUp ? y - 9 : y - 7;
  display.fillRect(x - 8, ay, 2, 3, C_WHITE);                     // arms
  display.fillRect(x + 7, ay, 2, 3, C_WHITE);
  display.drawFastVLine(x - 3, y - 8, 2, C_BLACK);                // eyes
  display.drawFastVLine(x + 3, y - 8, 2, C_BLACK);
  for (int lx : { -5, -3, 2, 4 }) display.drawFastVLine(x + lx, y - 3, 3, C_WHITE);   // legs
}

void sc_lyrics(uint32_t f) {
  uint32_t now = millis();
  int t = (int)(now - lyricAt);
  bool noLyrics = !lyricsOn || t > 20000;
  bool gap = !noLyrics && (!lyricText[0] || t > (int)lyricMs + 1500);
  if (noLyrics || gap) { sc_player(f); return; }   // a break in the singing: the record player

  // word-wrap into up to 3 rows of 21 characters
  char rows[3][22];
  uint8_t nr = 0;
  const char* q = lyricText;
  while (*q && nr < 3) {
    while (*q == ' ') q++;
    int n = (int)strlen(q);
    if (!n) break;
    if (n <= 21) { strcpy(rows[nr++], q); break; }
    int cut = 21;
    while (cut > 0 && q[cut] != ' ') cut--;
    if (cut == 0) cut = 21;
    memcpy(rows[nr], q, cut); rows[nr][cut] = 0; nr++;
    q += cut;
  }
  int total = 0;
  for (uint8_t r = 0; r < nr; r++) total += (int)strlen(rows[r]);

  // the new line slides up into place, then fills in karaoke-style as it's sung
  float pf = lyricMs ? clamp01(t / (lyricMs * 0.85f)) : 1;
  int fill = (int)(pf * total);
  int slide = t < 180 ? (180 - t) / 20 : 0;
  int y0 = (nr == 1 ? 36 : nr == 2 ? 30 : 24) + slide;
  int done = 0;
  for (uint8_t r = 0; r < nr; r++) {
    int len = (int)strlen(rows[r]), x = (SCREEN_WIDTH - len * 6) / 2, y = y0 + r * 12;
    txt(x, y, 1, rows[r]);
    int k = fill - done;
    if (k > len) k = len;
    if (k > 0) {
      char part[22];
      memcpy(part, rows[r], k); part[k] = 0;
      display.fillRect(x - 1, y - 1, k * 6 + 1, 9, C_WHITE);
      txtInv(x, y, part);
    }
    done += len;
  }

  // small Claude hops along the top as the line is sung; zips back for the next one
  int cx;
  if (t < 260) cx = 118 - (int)((118 - 10) * clamp01(t / 260.0f));       // zip back
  else         cx = 10 + (int)(108 * pf);
  float hopF = pf * total / 3.0f;                                         // a hop every 3 letters
  int hop = (t < 260 || pf >= 1) ? 0 : (int)(fabsf(sinf(hopF * PI)) * 6);
  bool hit = millis() - beatAt < 120;

  // the track Claude walks: dotted ahead, solid where it's been
  for (int x = 4; x < 124; x += 3) display.drawPixel(x, 19, C_WHITE);
  if (t >= 260 && cx > 4) display.drawFastHLine(4, 19, cx - 4, C_WHITE);

  // tiny notes pop out behind Claude and drift away
  if (t >= 260)
    for (uint8_t i = 0; i < 3; i++) {
      int age = (t + i * 330) % 990;
      int nx = cx - 9 - age / 45, ny = 12 - age / 110;
      if (nx < 2 || ny < 1) continue;
      display.fillRect(nx, ny, 2, 2, C_WHITE);              // head
      display.drawFastVLine(nx + 1, ny - 4, 4, C_WHITE);    // stem
      display.drawPixel(nx + 2, ny - 4, C_WHITE);           // flag
    }

  // two stars twinkle in the top corners
  if ((f / 9) % 4 != 0) sparkle(6, 6, f, 1); else display.drawPixel(6, 6, C_WHITE);
  if ((f / 11 + 2) % 4 != 0) sparkle(121, 5, f + 5, 1); else display.drawPixel(121, 5, C_WHITE);

  // a small equalizer along the bottom, when the lyrics leave room
  if (y0 + nr * 12 <= 56)
    for (uint8_t i = 0; i < 32; i++) {
      float wv = 0.5f + 0.5f * sinf(f * 0.27f + i * 1.7f) * cosf(f * 0.11f + i * 0.6f);
      int h = 1 + (int)(musicSmooth / 99.0f * 5 * wv + (hit ? 1 : 0));
      display.fillRect(i * 4 + 1, 64 - h, 2, h, C_WHITE);
    }

  singClaude(cx, 18 - hop, hit);
  if (t < 260)                                                            // speed lines
    for (int k = 0; k < 3; k++) display.drawFastHLine(cx + 9 + k * 3, 10 + k * 3, 6, C_WHITE);
}

/* ============================================================
   FIXING THE WIRING — small Claude reconnects the database to the
   server: picks up the loose cable, plugs it in (spark!), data flows,
   the server's lights settle... until it pops loose again.
   ============================================================ */
/* the cable from the database to (ex, ey), sagging a little */
void cable(int ex, int ey, int sag) {
  const int sx = 33, sy = 31;
  int px = sx, py = sy;
  for (int i = 1; i <= 16; i++) {
    float t = i / 16.0f;
    int x = sx + (int)((ex - sx) * t), y = sy + (int)((ey - sy) * t + sag * 4 * t * (1 - t));
    display.drawLine(px, py, x, y, C_WHITE);
    px = x; py = y;
  }
}

void sc_wiring(uint32_t f) {
  uint32_t p = f % 240;                            // 6 s
  const int portX = 97, portY = 29;
  bool connected = p >= 100 && p < 200;

  // the database: three stacked platters
  for (int k = 0; k < 3; k++) {
    int y = 14 + k * 9;
    display.fillRoundRect(8, y, 24, 10, 4, C_BLACK);
    display.drawRoundRect(8, y, 24, 10, 4, C_WHITE);
    display.drawPixel(26, y + 5, C_WHITE);
  }
  display.fillRect(31, 29, 3, 4, C_WHITE);         // its port

  // the server rack: three slots with status lights
  display.drawRect(98, 10, 24, 38, C_WHITE);
  for (int k = 0; k < 3; k++) {
    int y = 14 + k * 11;
    display.drawRect(101, y, 18, 8, C_WHITE);
    bool on = connected ? ((f / 6 + k) % 3 != 0) : ((f / 4) % 2 == 0);   // busy / alarm
    if (on) display.fillRect(114, y + 3, 3, 2, C_WHITE);
    if (connected) display.drawFastHLine(103, y + 3, 2 + (int)((f / 3 + k * 2) % 8), C_WHITE);
  }
  display.fillRect(95, portY - 1, 3, 4, C_WHITE);  // its port
  if (connected && p > 110) {                      // all good
    display.drawLine(106, 4, 108, 6, C_WHITE); display.drawLine(108, 6, 113, 1, C_WHITE);
  } else if (!connected && (f / 8) % 2) {
    txt(108, 1, 1, "!");
  }
  display.drawFastHLine(0, 56, 128, C_WHITE);      // the floor

  // Claude: walks over, picks the cable up, plugs it in, cheers, walks back
  int cx = 36, cy = 37, arm = 0;
  if (p < 60)        { cx = 36 + (int)p / 3; cy = 37 - (int)((f / 4) % 2); }
  else if (p < 100)  { cx = 56 + (int)(p - 60) / 7; arm = -3; }
  else if (p < 200)  { cx = 62; if ((p / 10) % 2) { arm = -3; cy = 35; } }
  else               { cx = 62 - (int)(p - 200) * 26 / 40; cy = 37 - (int)((f / 4) % 2); }
  claudeFB(cx, cy, false, arm);

  // the cable's loose end: on the floor, in Claude's hand, or plugged in
  int hx = cx + 31, hy = cy + 5 + arm;             // Claude's right hand
  int ex, ey, sag = 6;
  if (p < 62)        { ex = 84; ey = 53; sag = 10; }
  else if (p < 70)   { float t = (p - 62) / 8.0f; ex = 84 + (int)((hx - 84) * t); ey = 53 + (int)((hy - 53) * t); }
  else if (p < 100)  { float t = (p - 70) / 30.0f; ex = hx + (int)((portX - hx) * t); ey = hy + (int)((portY - hy) * t); }
  else if (p < 200)  { ex = portX - 2; ey = portY; sag = 5; }
  else               { float t = clamp01((p - 200) / 12.0f);        // pops out and drops
                       ex = portX - 2 - (int)(t * 11); ey = portY + (int)(t * 24); sag = 10; }
  cable(ex, ey, sag);
  display.fillRect(ex - 1, ey - 1, 3, 3, C_WHITE); // the plug

  if ((p >= 100 && p < 110) || (p >= 200 && p < 208)) sparkle(portX - 1, portY, f, 3);   // zap
  if (connected)                                   // data flowing along the cable
    for (int i = 0; i < 3; i++) {
      float t = ((f + i * 9) % 27) / 27.0f;
      int x = 33 + (int)((ex - 33) * t), y = 31 + (int)((ey - 31) * t + sag * 4 * t * (1 - t));
      display.fillRect(x - 1, y - 1, 3, 3, C_WHITE);
    }
}

/* ============================================================
   DJ SET — small Claude in headphones on the decks, under a light
   show: sweeping moving-head spots, a turning fan of lasers, a
   strobe on the beat, and level bars on the console.
   ============================================================ */
void dottedLine(int x0, int y0, int x1, int y1, int step, int phase) {
  int n = max(abs(x1 - x0), abs(y1 - y0));
  for (int i = phase % step; i <= n; i += step) {
    int y = y0 + (y1 - y0) * i / max(n, 1);
    if (y < 57) display.drawPixel(x0 + (x1 - x0) * i / max(n, 1), y, C_WHITE);   // not below the stage
  }
}

void sc_dj(uint32_t f) {
  uint32_t now = millis();
  bool hit = now - beatAt < 110;                    // on the beat
  float lvl = musicSmooth / 99.0f;

  // lasers: a fan shooting up from behind the decks, turning; on with the beat
  if ((beatAt / 500) % 2 == 0 || hit) {
    float base = sinf(f * 0.03f) * 0.45f;
    for (int k = -2; k <= 2; k++) {
      float a = -PI / 2 + base + k * 0.36f;
      dottedLine(64, 40, 64 + (int)(cosf(a) * 110), 40 + (int)(sinf(a) * 110), 3, (int)f);
    }
  }

  // moving-head spots in the corners, beams sweeping the stage
  for (int side = 0; side < 2; side++) {
    int fx = side ? 120 : 7;
    float sw = sinf(f * 0.045f + side * 2.2f);
    int tx = 64 + (int)(sw * 44);                  // where the beam lands on the floor
    display.fillRect(fx - 4, 0, 9, 4, C_WHITE);    // the fixture
    display.fillRect(fx - 2, 4, 5, 3, C_WHITE);
    dottedLine(fx, 7, tx - 9, 56, 2, 0);           // the beam's edges
    dottedLine(fx, 7, tx + 9, 56, 2, 1);
    for (int y = 14; y < 54; y += 7)               // a little haze inside the beam
      display.drawPixel(fx + (tx - fx) * (y - 7) / 49 + (int)((f + y) % 5) - 2, y, C_WHITE);
  }

  // strobe on the beat
  if (hit) { sparkle(30, 12, f, 3); sparkle(98, 10, f + 5, 3); }

  // Claude behind the decks: headphones on, nodding, scratching
  int bob = (int)(beatGlow * 2);
  int cx = 51, cy = 22 + bob;
  display.fillRect(cx - 6, cy - 3, 38, 18, C_BLACK);   // clear the beams behind it
  claudeFB(cx, cy, false, ((f / 6) % 2) ? -3 : 0);
  for (int r = 15; r <= 16; r++) display.drawCircleHelper(64, cy + 6, r, 1 | 2, C_WHITE);   // headband
  display.fillRoundRect(cx - 5, cy - 1, 5, 9, 2, C_WHITE);    // ear cups
  display.fillRoundRect(cx + 26, cy - 1, 5, 9, 2, C_WHITE);

  // the console, in front: two turntables on top, level bars on the front
  display.fillRect(20, 40, 88, 17, C_BLACK);
  display.drawRect(20, 40, 88, 17, C_WHITE);
  for (int t = 0; t < 2; t++) {
    int tx = t ? 82 : 46;
    display.fillRoundRect(tx - 14, 35, 28, 7, 3, C_BLACK);
    display.drawRoundRect(tx - 14, 35, 28, 7, 3, C_WHITE);
    float a = f * 0.25f + t;                       // the platter's mark spins
    display.drawPixel(tx + (int)(cosf(a) * 10), 38 + (int)(sinf(a) * 2), C_WHITE);
    display.drawPixel(tx, 38, C_WHITE);
  }
  for (int i = 0; i < 12; i++) {
    float wv = 0.5f + 0.5f * sinf(f * 0.3f + i * 1.3f);
    int h = 1 + (int)((2 + lvl * 9) * wv);
    if (h > 12) h = 12;
    display.fillRect(26 + i * 7, 55 - h, 4, h, C_WHITE);
  }
  display.drawFastHLine(0, 57, 128, C_WHITE);       // the stage edge
}


/* ============================================================
   NIGHT SKY — small Claude stargazing: twinkling stars, two dithered
   clouds drifting, a crescent moon, and now and then a shooting star
   that makes Claude hop.
   ============================================================ */
void ditherBox(int x, int y, int w, int h, int phase) {      // a checkerboard grey
  for (int py = y; py < y + h; py++)
    for (int px = x; px < x + w; px++)
      if (((px + py + phase) & 1) == 0) display.drawPixel(px, py, C_WHITE);
}

void nightCloud(int x, int y, int s) {                        // three stacked tiers, like pixel art
  ditherBox(x + 6 * s, y, 8 * s, 2 * s, 0);
  ditherBox(x + 2 * s, y + 2 * s, 16 * s, 2 * s, 0);
  ditherBox(x, y + 4 * s, 22 * s, 2 * s, 0);
}

void sc_night(uint32_t f) {
  uint32_t t = millis();

  // stars: some steady, some twinkling
  static const int8_t ST[][2] = { { 20, 8 }, { 52, 4 }, { 74, 12 }, { 10, 30 }, { 44, 26 }, { 64, 40 },
                                  { 116, 6 }, { 120, 34 }, { 86, 46 }, { 30, 48 }, { 104, 52 }, { 58, 16 } };
  for (uint8_t i = 0; i < 12; i++) {
    uint32_t ph = (t / 90 + i * 37) % 60;
    if (ph < 4) continue;                                     // a blink
    if (i % 3 == 0 && ph < 14) {                              // a bright twinkle
      display.drawFastHLine(ST[i][0] - 1, ST[i][1], 3, C_WHITE);
      display.drawFastVLine(ST[i][0], ST[i][1] - 1, 3, C_WHITE);
    } else {
      display.drawPixel(ST[i][0], ST[i][1], C_WHITE);
    }
  }

  // the moon: a crescent, its inner edge dithered
  const int mx = 98, my = 20;
  display.fillCircle(mx, my, 12, C_WHITE);
  display.fillCircle(mx + 6, my - 3, 11, C_BLACK);
  for (int a = 0; a < 40; a++) {                              // soft shading along the inside
    float ang = 1.6f + a * 0.08f;
    int x = mx + 6 + (int)(cosf(ang) * 12), y = my - 3 + (int)(sinf(ang) * 12);
    if (a % 2 == 0) display.drawPixel(x, y, C_BLACK);
  }

  // clouds drifting slowly, in opposite directions
  int c1 = (int)((t / 70) % 190) - 50, c2 = 150 - (int)((t / 110) % 210);
  nightCloud(c1, 16, 2);
  nightCloud(c2, 36, 2);

  // a shooting star every ~8 s
  uint32_t sh = t % 8000;
  bool shooting = sh < 600;
  if (shooting) {
    int sx = 120 - (int)(sh * 0.18f), sy = 4 + (int)(sh * 0.05f);
    for (int k = 0; k < 10; k++) if (k % 2 == 0 || k < 3) display.drawPixel(sx + k, sy - k / 3, C_WHITE);
    display.fillRect(sx - 1, sy, 2, 2, C_WHITE);
  }

  // the ground
  for (int x = 0; x < SCREEN_WIDTH; x += 2) display.drawPixel(x, 63, C_WHITE);

  // small Claude: breathing, blinking, looking up, hopping at a shooting star
  int hop = 0;
  if (sh >= 300 && sh < 900) hop = (int)(sinf((sh - 300) / 600.0f * PI) * 6);
  int cx = 20, cy = 44 - hop - (int)((t / 900) % 2);
  claudeFB(cx, cy, false, hop ? -3 : 0);
  bool lookUp = (t % 6000) > 3500 || (sh >= 300 && sh < 1500);
  bool blink = (t % 4300) < 140;
  for (int ex : { cx + 6, cx + 18 }) {
    display.fillRect(ex, cy + 2, 2, 5, C_WHITE);             // redraw the eyes
    if (blink)       display.fillRect(ex, cy + 5, 2, 1, C_BLACK);
    else if (lookUp) display.fillRect(ex, cy + 2, 2, 3, C_BLACK);
    else             display.fillRect(ex, cy + 3, 2, 4, C_BLACK);
  }
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
  { NULL,              0, NULL           },   // 20  (removed)
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
  { NULL,              0, sc_goal        },   // 36  penalty kick
  { NULL,              0, sc_usage       },   // 37
  { NULL,              0, sc_limitHit    },   // 38
  { NULL,              0, sc_testFailed  },   // 39
  { NULL,              0, sc_varCheck    },   // 40
  { NULL,              0, sc_wait        },   // 41
  { NULL,              0, sc_paperPlane  },   // 42
  { NULL,              0, NULL           },   // 43  (removed)
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
  { NULL,              0, sc_shell       },   // 56  PowerShell / bash window
  { "git graph",       0, sc_commitLog   },   // 57  commit, Claude beside the log
  { "git graph",       0, sc_mergeLog    },   // 58  merge, dot rides the branch
  { NULL,              0, sc_planeThrow  },   // 59  Claude throws the plane
  { "juggling",        0, sc_juggle3     },   // 60  three-object cascade
  { "nap time",        0, sc_napClaude   },   // 61  Claude naps
  { NULL,              0, sc_hello       },   // 62  hi!
  { NULL,              0, sc_mario       },   // 63  SUPER CLAUDE platformer
  { NULL,              0, sc_player      },   // 64  record player (real music controls)
  { NULL,              0, sc_clock       },   // 65  clock + date
  { NULL,              0, sc_lyrics      },   // 66  karaoke: Claude sings the lyrics
  { NULL,              0, sc_wiring      },   // 67  Claude fixes the wiring
  { NULL,              0, sc_dj          },   // 68  DJ set with stage lights
  { NULL,              0, sc_night       },   // 69  night sky: stargazing, shooting star
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
  i %= SCENE_COUNT;
  for (uint8_t k = 0; k < SCENE_COUNT && !SCENES[i].draw; k++) i = (i + 1) % SCENE_COUNT;   // removed: skip
  scene = i;
  sceneSince = millis();
  sceneTick0 = tick;
  playLives = -1; playOver = false;
  Serial.printf("scene %u\n", scene);
}

/* ============================================================
   LIFE DIRECTOR — when nobody is driving, Claude lives on its own:
   moods, a bit of work, games (and reacts to how they went), naps.
   ============================================================ */
const uint8_t MOODS[] = { 0, 24, 27, 28, 29, 12, 18, 21, 3, 19, 53, 62, 65, 65 };
const uint8_t WORKS[] = { 1, 2, 4, 5, 6, 7, 8, 9, 11, 30, 31, 32, 33, 34, 40, 41, 44, 51, 37, 57, 58, 67, 67 };
const uint8_t PLAYS[] = { 47, 48, 49, 50, 48, 49, 50, 45, 46, 36, 13, 14, 15, 16, 17, 59, 60, 60, 63, 63, 63 };
const uint8_t RESTS[] = { 22, 23, 21, 61, 61, 69, 69 };
const uint8_t WINS[]  = { 35, 24, 45, 28 };
const uint8_t LOSES[] = { 26, 25 };

/* can this scene be shown right now? without the bridge, the usage, context,
   stats and git screens would only show stale or empty numbers */
bool sceneOk(uint8_t s) {
  if (!SCENES[s].draw) return false;                     // removed face
  if (s == 65) return timeKnown;                         // clock: needs the time once
  if (!bridgeUp && (s == 11 || s == 37 || s == 51)) return false;
  if ((s == 32 || s == 57 || s == 58) && !gitKnown) return false;
  return true;
}

bool isGameScene(uint8_t s) { return (s >= 48 && s <= 50) || s == 63; }
bool isLongScene(uint8_t s) { return (s >= 47 && s <= 50) || s == 63; }

/* alive = you're working (bridge connected) but Claude is idle: no fake
   "work" scenes then, only moods, play (football, crane, games...) and naps */
void director(bool alive) {
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
  if (alive && r >= 30 && r < 58) r = 58 + random(32);   // play instead of fake work
  if (isNight() && r < 90 && random(100) < 40) r = 95;   // late at night: more naps
  do {
    if (r < 30)      next = MOODS[random(sizeof(MOODS))];
    else if (r < 58) next = WORKS[random(sizeof(WORKS))];
    else if (r < 90) next = PLAYS[random(sizeof(PLAYS))];
    else             next = RESTS[random(sizeof(RESTS))];
  } while (next == scene || !sceneOk(next));

  if (isLongScene(next))  sceneDur = alive ? random(12000, 16000) : random(16000, 24000);
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
  if (touchState) holdConsumed = true;   // the hold that left the game isn't a pet
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
/* each event has a few fitting scenes; one is picked at random so it feels
   fresh every time (and kept while the same kind of work continues) */
struct Ev { const char* name; uint8_t n; uint8_t sc[4]; };
const Ev EVENTS[] = {
  { "idle",    1, { 0 } },               { "think",   1, { 54 } },
  { "read",    2, { 4, 40 } },           { "type",    4, { 5, 44, 6, 7 } },
  { "write",   3, { 47, 7, 44 } },       { "build",   2, { 47, 34 } },     // building -> crane
  { "search",  2, { 31, 40 } },          { "web",     1, { 8 } },
  { "bash",    1, { 56 } },              { "ps",      1, { 56 } },         // real shell window
  { "git",     2, { 32, 57 } },          { "test",    1, { 9 } },
  { "need",    1, { 10 } },              { "done",    1, { 35 } },
  { "fail",    2, { 39, 26 } },          { "merge",   2, { 33, 58 } },
  { "wait",    1, { 41 } },              { "bug",     2, { 45, 46 } }, // tests pass -> celebrate
  { "plane",   2, { 42, 59 } },          { "compact", 1, { 11 } },
  { "pkg",     2, { 34, 47 } },          { "key",     1, { 44 } },
  { "goal",    1, { 45 } },              { "kick",    1, { 46 } },
  { "crane",   1, { 47 } },              { "flappy",  1, { 48 } },
  { "panic",   1, { 49 } },              { "whack",   1, { 50 } },
  { "stats",   1, { 51 } },              { "profile", 1, { 53 } },
  { "mario",   1, { 63 } },              { "clock",   1, { 65 } },
};
const uint8_t EVENT_COUNT = sizeof(EVENTS) / sizeof(EVENTS[0]);

void onEvent(const char* name) {
  uint32_t now = millis();
  static uint32_t lastNeed = 0, lastHm = 0, lastDone = 0;

  if (!strcmp(name, "key")) {                        // you're typing
    static uint32_t lastKeyReact = 0;
    static const uint8_t KEYS[] = { 44, 44, 6, 27 }; // typing along, key peek, watching you
    live = true; lastEvent = now; autoCycle = false; lastKeyAt = now;
    bool inKey = (scene == 44 || scene == 6 || scene == 27);
    // don't copy every keystroke: never while Claude works, at most every 30 s
    if (busy || gameMode || followStage || inKey || now - lastKeyReact < 30000) return;
    lastKeyReact = now;
    setScene(KEYS[random(4)]);
    keyScene = scene;
    return;
  }
  live = true; lastEvent = now; autoCycle = false; followStage = 0;
  if (!strcmp(name, "done"))     addXp(4);
  else if (!strcmp(name, "bug")) addXp(6);

  if (!strcmp(name, "need")) {                     // Claude needs you: leave the game
    gameMode = false;
    if (now - lastNeed > 10000) { sndNeed(); lastNeed = now; }
  }
  if (gameMode) return;                            // don't interrupt a game otherwise

  // the voice only speaks up for the moments that matter
  if (!strcmp(name, "think") && !busy)               sndPrompt();      // a new turn
  else if (!strcmp(name, "think") && now - lastHm > 8000) { sndHm(); lastHm = now; }
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
    if (!busy) { busy = true; busyStart = now; busySteps = 0; verbIdx = random(VERB_COUNT); }
    lastToolAt = 0;
    if (scene != 54) setScene(54);
    return;
  }
  if (busy && strcmp(name, "need") && strcmp(name, "key")) {   // a real tool step
    lastToolAt = now;
    if (busySteps < 999) busySteps++;
    amb(1760, 2093, 12, 30);                                    // tiny "tick"
  }
  for (uint8_t i = 0; i < EVENT_COUNT; i++)
    if (!strcmp(name, EVENTS[i].name)) {
      const Ev& e = EVENTS[i];
      if (!strcmp(name, "ps"))   shellPs = true;
      if (!strcmp(name, "bash")) shellPs = false;
      bool already = false;
      for (uint8_t k = 0; k < e.n; k++) if (scene == e.sc[k]) already = true;
      if (!already || e.sc[0] == 56) setScene(e.sc[random(e.n)]);   // shell: retype each command
      sceneSince = now;
      return;
    }
}

/* a new commit: shift it in now; the bridge follows up with the real list */
void pushCommit(const char* h) {
  for (uint8_t i = GIT_ROWS - 1; i > 0; i--) {
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
  if (i < 0 || i >= GIT_ROWS || !p) return;
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

/* state shared by the line parser, the intro and the moments */
#define INTRO_MS    5400
#define BOOT_PRE_MS 800      // power-on only: the old-TV switch-on before the spark
uint32_t introAt = 0, lastIntroAt = 0;
uint8_t  introTyped = 0;
bool     introLanded = false;
bool     introBoot = false;  // this intro is the power-on one
#define MO_BACK 1
#define MO_MEET 2
#define MO_TODO 3
#define MO_DONE 4
#define MO_ACH  5
#define MO_SOLO 6
#define MO_MAX  4
uint8_t  moKind[MO_MAX];
int16_t  moA[MO_MAX], moB[MO_MAX];
char     moText[MO_MAX][34];
uint8_t  moLen = 0;
uint32_t moAt  = 0;                     // when the front moment started (0 = waiting)

/* to-do list (Y:) and the notification banner (O:) */
uint8_t  tdDone = 0, tdTotal = 0;
uint32_t tdAt = 0;
char     tdText[34] = "";
char     ntText[66] = "";
uint8_t  ntIcon = 0;
uint32_t ntAt = 0, ntMs = 0;

void handleLine(char* line) {
  if (line[1] != ':') return;
  char* v = line + 2;
  lastBridgeAt = millis();                          // the bridge is alive
  bridgeUp = true;
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
    case 'M': {                                       // M:<0 none|1 playing|2 paused>,<title - artist>
      bool on = v[0] == '1';
      const char* p = strchr(v, ',');
      if (on && !musicOn && !musicPaused && soundOn) {   // Claude notices the music: "ooh!"
        sndClear(); sndPushV(900, 1400, 80, 30, 50); sndPushV(1568, 1568, 60, 0, 45);
      }
      musicOn = on; musicPaused = v[0] == '2'; musicAt = millis();
      if (p) { strncpy(musicTitle, p + 1, sizeof(musicTitle) - 1); musicTitle[sizeof(musicTitle) - 1] = 0; }
      if (!on) musicLevel = 0;
      break;
    }
    case 'D':                                         // D:<local epoch seconds>
      localEpoch = strtoul(v, NULL, 10); epochAt = millis(); timeKnown = localEpoch > 1600000000UL;
      updateClock();
      break;
    case 'Q': {                                       // Q:<position s>,<length s>
      int p = 0, d = 0;
      sscanf(v, "%d,%d", &p, &d);
      trackPos = p > 0 ? p : 0; trackDur = d > 0 ? d : 0; trackAt = millis();
      break;
    }
    case 'A': {                                       // A:<level> ~12x a second
      musicLevel = constrain(atoi(v), 0, 99);
      musicAt = millis();
      if (musicLevel > musicAvg + 14 && millis() - beatAt > 180) beatAt = millis();
      musicAvg = musicAvg * 0.9f + musicLevel * 0.1f;
      break;
    }
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
    case 'I': break;                                  // I:1  heartbeat (handled above)
    case 'F': {                                       // F:<setting>,<value>  from the settings page
      char key[8] = "";
      const char* c = strchr(v, ',');
      size_t kl = c ? (size_t)(c - v) : strlen(v);
      if (kl > 7) kl = 7;
      memcpy(key, v, kl); key[kl] = 0;
      int val = c ? atoi(c + 1) : -1;
      if (!strcmp(key, "bri") && val >= 0 && val < BRI_COUNT) {
        briIndex = val; applyBrightness(); saveSettings();
      } else if (!strcmp(key, "vol") && val >= 0 && val < VOL_COUNT) {
        volIndex = val; saveSettings(); sndPreview();
      } else if (!strcmp(key, "snd") && val >= SND_OFF && val <= SND_ALL) {
        setSoundMode(val); saveSettings();
      } else if (!strcmp(key, "face") && val >= 0 && val < SCENE_COUNT && SCENES[val].draw) {
        setScene(val);                                // hold it like a face you picked by tapping
        autoCycle = false; manualAt = lastInput = millis(); sceneDur = 20000;
      } else if (!strcmp(key, "hello")) {
        lastIntroAt = 0; startIntro();
      }
      char b[32];                                     // tell the page what the board has now
      snprintf(b, sizeof(b), "F:%u,%u,%u,%u,%u", briIndex, BRI_COUNT, volIndex, VOL_COUNT, soundMode);
      sendToLaptop(b);
      break;
    }
    case 'Z': {                                       // Z:<ms>,<lyric line>   "Z:0," = no lyrics
      const char* p = strchr(v, ',');
      int ms = atoi(v);
      lyricsOn = ms > 0 || (p && p[1]);
      lyricMs = constrain(ms, 0, 15000); lyricAt = millis();
      strncpy(lyricText, p ? p + 1 : "", sizeof(lyricText) - 1);
      lyricText[sizeof(lyricText) - 1] = 0;
      break;
    }
    case 'H': startIntro(); break;                    // H:1  the laptop bridge just linked up
    case 'B': pushMoment(MO_BACK, atoi(v), 0, ""); break;      // B:<minutes you were away>
    case 'N': {                                        // N:<minutes>,<meeting title>
      const char* p = strchr(v, ',');
      pushMoment(MO_MEET, atoi(v), 0, p ? p + 1 : "");
      break;
    }
    case 'O': {                                        // O:<icon>,<text>  notification banner
      const char* p = strchr(v, ',');
      startNotify(constrain(atoi(v), 0, 4), p ? p + 1 : v);
      break;
    }
    case 'Y': {                                        // Y:<done>,<total>,<current item>
      int d = 0, n = 0;
      sscanf(v, "%d,%d", &d, &n);
      const char* p = strchr(v, ',');
      p = p ? strchr(p + 1, ',') : NULL;
      bool ticked = n > 0 && n == tdTotal && d > tdDone;   // an item just got done
      tdDone = constrain(d, 0, 255); tdTotal = constrain(n, 0, 255); tdAt = millis();
      strncpy(tdText, p ? p + 1 : "", sizeof(tdText) - 1); tdText[sizeof(tdText) - 1] = 0;
      if (ticked) {
        if (d >= n) pushMoment(MO_DONE, n, 0, "");
        else        pushMoment(MO_TODO, d, n, tdText);
        addXp(2);
      }
      break;
    }
    case 'V': {                                        // V:<0 trophy|1 flame|2 star>,<title>
      const char* p = strchr(v, ',');
      pushMoment(MO_ACH, atoi(v), 0, p ? p + 1 : v);
      addXp(15);
      break;
    }
  }
}

/* ============================================================
   CONNECT INTRO — the bridge sends H:1 when it links up (USB or
   Bluetooth): Claude's spark bursts in with "connected" ripples,
   "Build something" types itself out, "great" drops in with a
   burst of stars, then the screen shuts like an old CRT.
   Tap to skip.
   ============================================================ */

float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
float easeOutBack(float x) {                        // overshoots a little, then settles
  const float c1 = 1.70158f, c3 = c1 + 1;
  x -= 1;
  return 1 + c3 * x * x * x + c1 * x * x;
}

/* Claude's spark: 12 tapered spokes, long and short */
void claudeSpark(int cx, int cy, float r, float rot) {
  if (r < 1) { display.drawPixel(cx, cy, C_WHITE); return; }
  float w = r * 0.14f < 0.8f ? 0.8f : r * 0.14f;
  for (uint8_t i = 0; i < 12; i++) {
    float a = rot + i * (PI / 6);
    float l = r * ((i & 1) ? 0.6f : 1.0f);
    int tx = cx + (int)lroundf(cosf(a) * l), ty = cy + (int)lroundf(sinf(a) * l);
    int px = (int)lroundf(cosf(a + PI / 2) * w), py = (int)lroundf(sinf(a + PI / 2) * w);
    display.fillTriangle(cx + px, cy + py, cx - px, cy - py, tx, ty, C_WHITE);
  }
  int c = (int)(r * 0.2f);
  display.fillCircle(cx, cy, c < 1 ? 1 : c, C_WHITE);
}

void startIntro() {
  uint32_t now = millis();
  if (lastIntroAt && now - lastIntroAt < 30000) return;   // a quick reconnect isn't a new hello
  if (menuOpen || gameMode) return;
  lastIntroAt = now;
  introAt = now ? now : 1;
  introTyped = 0; introLanded = false; introBoot = false;
  linkToastAt = 0;                                        // the intro says hello instead
  if (soundOn) {                                          // rising "link up" chime
    sndClear();
    sndPushV(523, 523, 55, 15, 55);
    sndPushV(659, 659, 55, 15, 60);
    sndPushV(784, 784, 55, 15, 65);
    sndPushV(1047, 1568, 180, 0, 70);
  }
}

/* power-on: a dot, a line, the picture rolls open full of static, snow fades */
void drawPowerOn(int t) {
  if (t < 150) {                                   // a pinpoint warms up
    display.fillCircle(64, 32, 1 + t / 60, C_WHITE);
  } else if (t < 400) {                            // ...stretches into a scanline
    int w = (t - 150) * 64 / 250;
    display.drawFastHLine(64 - w, 32, w * 2, C_WHITE);
    display.drawFastHLine(64 - w / 2, 31, w, C_WHITE);
    display.drawFastHLine(64 - w / 2, 33, w, C_WHITE);
  } else if (t < 650) {                            // ...and opens up, crackling with static
    int h = 1 + (t - 400) * 31 / 250;
    display.fillRect(0, 32 - h, SCREEN_WIDTH, h * 2, C_WHITE);
    for (int i = 0; i < 180; i++) display.drawPixel(random(SCREEN_WIDTH), 32 - h + random(h * 2), C_BLACK);
  } else {                                         // the snow settles
    int n = (BOOT_PRE_MS - t) * 4;
    for (int i = 0; i < n; i++) display.drawPixel(random(SCREEN_WIDTH), random(SCREEN_HEIGHT), C_WHITE);
  }
}

/* power-on: the switch-on hum, then the usual intro and its chime */
void startBootIntro() {
  bool s = soundOn;
  soundOn = false;                                 // hold the chime: it follows the hum
  startIntro();
  soundOn = s;
  if (!introAt) return;
  introBoot = true;
  if (soundOn) {
    sndClear();
    sndPushV(120, 900, 520, 60, 35);               // the tube warming up
    sndPushV(2600, 1800, 40, 180, 20);             // static crackle
    sndPushV(523, 523, 55, 15, 55);
    sndPushV(659, 659, 55, 15, 60);
    sndPushV(784, 784, 55, 15, 65);
    sndPushV(1047, 1568, 180, 0, 70);
  }
}

/* returns false once the intro is over */
bool drawIntro() {
  int t = (int)(millis() - introAt);
  if (t >= INTRO_MS + (introBoot ? BOOT_PRE_MS : 0) || touchPressed) {
    introAt = 0;
    if (!busy && !gameMode) setScene(0);           // straight to Claude's face
    return false;
  }
  if (introBoot) {                                 // power-on: the screen warms up first
    if (t < BOOT_PRE_MS) { drawPowerOn(t); return true; }
    t -= BOOT_PRE_MS;
  }
  static const char L1[] = "Build something";
  const int n1 = sizeof(L1) - 1;

  // "connected" ripples while the spark bursts in
  for (uint8_t k = 0; k < 3; k++) {
    int rr = (t - k * 220) / 11;
    if (t > k * 220 && rr > 0 && rr < 72) display.drawCircle(64, 32, rr, C_WHITE);
  }

  // the spark: spins in fast and slows down, then floats up above the words
  float rot = t * 0.002f - 4.0f * sq(1 - clamp01(t / 900.0f));
  float r, cy;
  if (t < 700) { r = 15 * easeOutBack(clamp01(t / 700.0f)); cy = 32; }
  else {
    float p = clamp01((t - 700) / 450.0f);
    p = p * p * (3 - 2 * p);
    r = 15 - 7 * p; cy = 32 - 21 * p;
  }
  claudeSpark(64, (int)cy, r, rot);

  // typewriter: "Build something" with a blinking block cursor
  if (t >= 1150) {
    int n = (t - 1150) / 65 + 1;
    if (n > n1) n = n1;
    if (n > introTyped) {
      introTyped = n;
      if (soundMode == SND_ALL) sndPushV(2200 + random(400), 2200, 8, 0, 25);   // key click
    }
    char buf[sizeof(L1)];
    memcpy(buf, L1, n); buf[n] = 0;
    int x0 = (SCREEN_WIDTH - n1 * 6) / 2;
    txt(x0, 24, 1, buf);
    if (t < 2500 && (t / 180) % 2 == 0) display.fillRect(x0 + n * 6, 24, 5, 8, C_WHITE);
  }

  // "great" drops in big, with a little bounce
  if (t >= 2250) {
    float p = clamp01((t - 2250) / 350.0f);
    txt(34, -16 + (int)(52 * easeOutBack(p)), 2, "great");
    if (p >= 1 && !introLanded) {
      introLanded = true;
      if (soundOn) { sndPushV(2093, 2637, 40, 10, 45); sndPushV(3136, 3136, 70, 0, 35); }
    }
  }

  // star dust flies out where it lands
  if (t >= 2600 && t < 3150) {
    float d = (t - 2600) * 0.06f;
    for (uint8_t i = 0; i < 14; i++) {
      float a = i * (2 * PI / 14) + 0.2f, sp = 1 + (i % 3) * 0.35f;
      int x = 64 + (int)(cosf(a) * d * sp * 1.6f), y = 43 + (int)(sinf(a) * d * sp * 0.7f);
      display.drawPixel(x, y, C_WHITE);
      if (!(i & 1)) display.drawPixel(x + 1, y, C_WHITE);
    }
  }

  // underline sweeps out from the middle, then stars twinkle around
  if (t >= 2700) {
    float p = clamp01((t - 2700) / 400.0f);
    p = 1 - (1 - p) * (1 - p);
    int hw = (int)(38 * p);
    display.drawFastHLine(64 - hw, 54, hw * 2, C_WHITE);
    if (hw > 8) display.drawFastHLine(64 - hw + 8, 56, hw * 2 - 16, C_WHITE);
    static const int8_t TW[][2] = { { 12, 22 }, { 115, 22 }, { 18, 48 }, { 110, 48 }, { 100, 9 }, { 28, 9 } };
    for (uint8_t i = 0; i < 6; i++)
      if (((t / 110) + i * 3) % 9 < 4) sparkle(TW[i][0], TW[i][1], tick, 2);
  }

  // CRT power-off: shutters close to a bright line, the line shrinks to nothing
  if (t >= 4700) {
    float p = clamp01((t - 4700) / 500.0f);
    int h = (int)(32 * p * p);
    display.fillRect(0, 0, SCREEN_WIDTH, h, C_BLACK);
    display.fillRect(0, SCREEN_HEIGHT - h, SCREEN_WIDTH, h, C_BLACK);
    if (h < 31) {
      display.drawFastHLine(0, h, SCREEN_WIDTH, C_WHITE);
      display.drawFastHLine(0, SCREEN_HEIGHT - 1 - h, SCREEN_WIDTH, C_WHITE);
    } else {
      display.fillRect(0, 30, SCREEN_WIDTH, 4, C_BLACK);
      int w = (int)(SCREEN_WIDTH * (1 - clamp01((t - 5200) / 180.0f)));
      if (w > 0) display.drawFastHLine(64 - w / 2, 31, w, C_WHITE);
      else display.drawPixel(64, 31, C_WHITE);
    }
  }
  return true;
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

/* ============================================================
   MOMENTS — short full-screen reactions queued by the bridge:
     B: you're back      N: meeting soon      Y: a to-do ticked off
     Y: all done         V: achievement / streak
   Tap to skip one.
   ============================================================ */

void pushMoment(uint8_t kind, int a, int b, const char* text) {
  uint8_t i = moLen;
  for (uint8_t k = 1; k < moLen; k++) if (moKind[k] == kind) i = k;   // update a queued one
  if (i == moLen) { if (moLen >= MO_MAX) return; moLen++; }
  moKind[i] = kind; moA[i] = a; moB[i] = b;
  strncpy(moText[i], text, sizeof(moText[i]) - 1);
  moText[i][sizeof(moText[i]) - 1] = 0;
}

void popMoment() {
  for (uint8_t i = 1; i < moLen; i++) {
    moKind[i - 1] = moKind[i]; moA[i - 1] = moA[i]; moB[i - 1] = moB[i];
    memcpy(moText[i - 1], moText[i], sizeof(moText[0]));
  }
  if (moLen) moLen--;
  moAt = 0;
}

uint16_t momentMs(uint8_t k) {
  return k == MO_BACK ? 3800 : k == MO_MEET ? 5600 : k == MO_TODO ? 2600 : k == MO_DONE ? 4000 :
         k == MO_SOLO ? 3400 : 4600;
}

void momentSound(uint8_t k) {
  if (!soundOn) return;
  sndClear();
  switch (k) {
    case MO_BACK:                                         // ...hm? oh! hi!
      sndPushV(300, 300, 1, 700, 0);
      sndPushV(520, 900, 90, 40, 55);
      sndPushV(1047, 1047, 60, 20, 55); sndPushV(1319, 1568, 110, 0, 60);
      break;
    case MO_MEET:                                         // alarm clock: bi-bi-bi  bi-bi-bi
      for (uint8_t r = 0; r < 2; r++)
        for (uint8_t i = 0; i < 3; i++) sndPushV(2100, 2100, 55, i == 2 ? 260 : 45, 60);
      break;
    case MO_TODO:
      sndPushV(1568, 1568, 40, 15, 45); sndPushV(2093, 2093, 70, 0, 45);
      break;
    case MO_DONE:
      sndGoal();
      break;
    case MO_SOLO:                                         // oh... ok! (falls, then perks up)
      sndPushV(880, 660, 120, 60, 45); sndPushV(620, 470, 180, 900, 40);
      sndPushV(784, 1175, 120, 0, 50);
      break;
    default: {                                            // achievement fanfare
      static const uint16_t n[] = { 784, 988, 1175, 1568, 1175, 1568, 2093 };
      for (uint8_t i = 0; i < 7; i++) sndPushV(n[i], n[i], i == 6 ? 220 : 70, 20, 60);
    }
  }
}

void txtInv(int x, int y, const char* t) {               // black text (on a white bar)
  display.setTextSize(1);
  display.setTextColor(C_BLACK);
  display.setCursor(x, y);
  display.print(t);
  display.setTextColor(C_WHITE);
}

void clipCopy(char* dst, const char* src, uint8_t chars) { // ".." when it doesn't fit
  size_t n = strlen(src);
  if (n <= chars) { strcpy(dst, src); return; }
  memcpy(dst, src, chars - 2); dst[chars - 2] = '.'; dst[chars - 1] = '.'; dst[chars] = 0;
}

void moBack(int t) {
  char b[24];
  if (t < 700) {                                          // dozing...
    eyes(64, 28, 28, 5, 22, EY_SLEEPY, 2, false);
    txt(94, 12 - (t / 150) % 3, 1, "z"); txt(102, 4, 1, "Z");
    int m = moA[0];
    if (m < 60) snprintf(b, sizeof(b), "away %dm", m);
    else        snprintf(b, sizeof(b), "away %dh %02dm", m / 60, m % 60);
    txtC(54, 1, b);
  } else if (t < 1200) {                                  // ...!
    int g = t < 820 ? (t - 700) / 30 : 4;
    eyes(64, 28, 28 + g, 34 + g, 20, EY_WIDE, 10, false);
    txt(110, 6, 2, "!");
  } else {                                                // so happy
    eyes(64, 24 + (int)wobble(tick, 0.25f, 2), 30, 30, 22, EY_HAPPY, 9, false);
    sparkle(16, 12, tick, 3); sparkle(112, 14, tick + 7, 3);
    static const char M[] = "oh, you're back!";
    const int n1 = sizeof(M) - 1;
    int n = (t - 1200) / 45 + 1; if (n > n1) n = n1;
    char buf[sizeof(M)]; memcpy(buf, M, n); buf[n] = 0;
    txt((SCREEN_WIDTH - n1 * 6) / 2, 52, 1, buf);
  }
}

void moMeet(int t) {
  int sh = (t / 50) % 2 ? 2 : -2, cx = 18 + sh, cy = 30;  // alarm clock, ringing
  display.drawCircle(cx, cy, 10, C_WHITE); display.drawCircle(cx, cy, 9, C_WHITE);
  display.drawLine(cx, cy, cx, cy - 6, C_WHITE); display.drawLine(cx, cy, cx + 4, cy + 2, C_WHITE);
  display.fillCircle(cx - 8, cy - 9, 3, C_WHITE); display.fillCircle(cx + 8, cy - 9, 3, C_WHITE);
  display.drawLine(cx - 6, cy + 8, cx - 8, cy + 12, C_WHITE);
  display.drawLine(cx + 6, cy + 8, cx + 8, cy + 12, C_WHITE);
  if ((t / 100) % 2) {
    display.drawLine(cx - 13, cy - 12, cx - 16, cy - 15, C_WHITE);
    display.drawLine(cx + 13, cy - 12, cx + 16, cy - 15, C_WHITE);
  }
  eyes(74 + random(-2, 3), 30 + random(-1, 2), 20, 24, 16, EY_WIDE, 8, false);   // panic!
  int d = (t / 14) % 24;                                  // sweat drop
  display.fillCircle(116, 18 + d, 2, C_WHITE);
  display.fillTriangle(114, 18 + d, 118, 18 + d, 116, 13 + d, C_WHITE);

  char b[24];
  clipCopy(b, moText[0][0] ? moText[0] : "meeting", 21);
  txtC(1, 1, b);
  if (moA[0] <= 0) strcpy(b, "starting now!");
  else snprintf(b, sizeof(b), "in %d min!", moA[0]);
  int w = (int)strlen(b) * 6, x = (SCREEN_WIDTH - w) / 2;
  if ((t / 260) % 2) { display.fillRect(x - 3, 52, w + 6, 11, C_WHITE); txtInv(x, 54, b); }
  else txt(x, 54, 1, b);
}

void moTodo(int t) {
  // the tick draws itself
  float p = clamp01(t / 380.0f);
  int ax = 8, ay = 22, bx = 17, by = 31, cx = 36, cy = 9;
  for (int o = 0; o < 3; o++) {
    if (p < 0.35f) {
      float q = p / 0.35f;
      display.drawLine(ax, ay + o, ax + (int)((bx - ax) * q), ay + o + (int)((by - ay) * q), C_WHITE);
    } else {
      float q = (p - 0.35f) / 0.65f;
      display.drawLine(ax, ay + o, bx, by + o, C_WHITE);
      display.drawLine(bx, by + o, bx + (int)((cx - bx) * q), by + o + (int)((cy - by) * q), C_WHITE);
    }
  }
  if (t > 380 && t < 900) sparkle(38, 8, tick, 3);

  char b[12];
  snprintf(b, sizeof(b), "%d/%d", moA[0], moB[0]);
  txt(46, 6, 2, b);
  if (46 + (int)strlen(b) * 12 + 26 <= SCREEN_WIDTH) txt(48 + (int)strlen(b) * 12, 13, 1, "done");

  // the bar fills up to the new count
  float q = clamp01((t - 200) / 650.0f); q = 1 - (1 - q) * (1 - q);
  float from = moB[0] ? (moA[0] - 1) / (float)moB[0] : 0, to = moB[0] ? moA[0] / (float)moB[0] : 0;
  display.drawRoundRect(46, 26, 78, 9, 3, C_WHITE);
  int w = (int)(74 * (from + (to - from) * q));
  if (w > 0) display.fillRect(48, 28, w, 5, C_WHITE);

  if (moText[0][0]) {                                     // what's next
    char nx[24];
    clipCopy(nx, moText[0], 21);
    txt(2, 42, 1, "next:");
    txt(2, 53, 1, nx);
  }
}

void moDone(int t) {
  for (uint8_t i = 0; i < 26; i++) {                      // confetti rain
    float sp = 0.035f + (i % 5) * 0.012f;
    int x = (i * 53 + 7) % 128 + (int)(sinf(t * 0.01f + i) * 3);
    int y = (int)(t * sp) - (i * 11) % 40 - 4;
    if (y < 0 || y > 63) continue;
    if (i % 3 == 0) display.fillRect(x, y, 2, 2, C_WHITE);
    else if (i % 3 == 1) display.drawLine(x, y, x + 2, y + 1, C_WHITE);
    else display.drawPixel(x, y, C_WHITE);
  }
  eyes(64, 18 + (int)wobble(tick, 0.3f, 3), 24, 24, 20, EY_HAPPY, 8, false);
  txtC(38, 2, "all done!");
  char b[20];
  snprintf(b, sizeof(b), "%d of %d tasks", moA[0], moA[0]);
  txtC(56, 1, b);
}

void moAch(int t) {
  // banner slides down, a shine sweeps across it
  int by = t < 300 ? -12 + t * 12 / 300 : 0;
  display.fillRect(0, by, SCREEN_WIDTH, 12, C_WHITE);
  const char* head = moA[0] == 1 ? "STREAK!" : "ACHIEVEMENT";
  txtInv((SCREEN_WIDTH - (int)strlen(head) * 6) / 2, by + 2, head);
  int sx = (t - 400) / 4;
  if (sx > -10 && sx < 140)
    for (int o = 0; o < 4; o++) display.drawLine(sx + o, by, sx + o - 8, by + 11, C_BLACK);

  // the badge pops in, rays turning behind it
  float p = clamp01((t - 150) / 450.0f);
  int cy = 31 + (int)((1 - easeOutBack(p)) * 30);
  if (p >= 1)
    for (uint8_t i = 0; i < 10; i++) {
      float a = t * 0.0015f + i * PI / 5;
      display.drawLine(64 + (int)(cosf(a) * 15), cy + (int)(sinf(a) * 13),
                       64 + (int)(cosf(a) * 21), cy + (int)(sinf(a) * 17), C_WHITE);
    }
  if (moA[0] == 1) {                                      // flame
    int fl = (int)wobble(tick, 0.5f, 2);
    display.fillCircle(64, cy + 4, 8, C_WHITE);
    display.fillTriangle(56, cy + 3, 72, cy + 3, 64 + fl, cy - 14, C_WHITE);
    display.fillCircle(64, cy + 6, 4, C_BLACK);
    display.fillTriangle(60, cy + 5, 68, cy + 5, 64 - fl, cy - 4, C_BLACK);
  } else if (moA[0] == 2) {                               // star
    claudeSpark(64, cy, 11, t * 0.002f);
  } else {                                                // trophy
    display.fillRect(57, cy - 11, 14, 9, C_WHITE);
    display.fillTriangle(57, cy - 3, 71, cy - 3, 64, cy + 3, C_WHITE);
    display.drawCircle(54, cy - 7, 3, C_WHITE); display.drawCircle(74, cy - 7, 3, C_WHITE);
    display.fillRect(62, cy + 3, 4, 4, C_WHITE);
    display.fillRect(58, cy + 7, 12, 3, C_WHITE);
  }
  if (p >= 1) { sparkle(30, 30, tick, 2); sparkle(98, 26, tick + 9, 2); }

  if (t > 650) {                                          // the title types in
    char b[24];
    clipCopy(b, moText[0], 21);
    int n1 = (int)strlen(b), n = (t - 650) / 40 + 1;
    if (n < n1) b[n] = 0;
    txt((SCREEN_WIDTH - n1 * 6) / 2, 54, 1, b);
  }
}

/* the laptop bridge went quiet: the plug pops out, Claude looks around,
   then shrugs it off and goes on living on its own */
void moSolo(int t) {
  int gap = t < 500 ? t / 50 : 10;                       // plug and socket drift apart
  display.fillRect(36 - gap, 6, 14, 8, C_WHITE);          // plug body
  display.drawFastHLine(50 - gap, 8, 5, C_WHITE);         // prongs
  display.drawFastHLine(50 - gap, 11, 5, C_WHITE);
  display.drawFastHLine(8, 10, 28 - gap, C_WHITE);        // cable
  display.drawRect(72 + gap, 4, 12, 12, C_WHITE);         // socket
  display.drawFastHLine(84 + gap, 10, 36 - gap, C_WHITE);
  if (t > 150 && t < 700) {                               // spark as it lets go
    display.drawLine(60, 4, 62, 8, C_WHITE); display.drawLine(64, 14, 66, 10, C_WHITE);
  }
  if (t < 1700) {                                         // looking around: laptop?
    int dx = (int)(sinf(t * 0.006f) * 14);
    eyes(64 + dx, 36, 24, 24, 20, EY_NORMAL, 8, false);
    char b[12]; uint8_t dots = (t / 300) % 4;
    snprintf(b, sizeof(b), "laptop%.*s", dots, "???");
    txtC(55, 1, b);
  } else {                                                // ...fine, solo it is
    eyes(64, 34 + (int)wobble(tick, 0.2f, 2), 26, 24, 20, EY_HAPPY, 8, false);
    sparkle(22, 30, tick, 2); sparkle(106, 30, tick + 6, 2);
    txtC(55, 1, "going solo!");
  }
}

/* the bridge stopped talking: drop everything that came from the laptop */
void goSolo() {
  bridgeUp = false;
  live = false; busy = false; autoCycle = true; sceneDur = 0;
  followStage = 0; keyScene = 255;
  musicOn = musicPaused = false; musicLevel = 0;
  tdTotal = tdDone = 0; ticker[0] = 0; ntAt = 0; lyricsOn = false;
  moLen = 0; moAt = 0;                                    // stale reactions are pointless now
  if (!menuOpen && !gameMode) pushMoment(MO_SOLO, 0, 0, "");
  Serial.println("bridge gone: living on my own");
}

bool drawMoment() {
  uint32_t now = millis();
  if (!moAt) { moAt = now ? now : 1; momentSound(moKind[0]); }
  int t = (int)(now - moAt);
  if (t >= momentMs(moKind[0]) || (touchPressed && t > 300)) { popMoment(); return false; }
  switch (moKind[0]) {
    case MO_BACK: moBack(t); break;
    case MO_MEET: moMeet(t); break;
    case MO_TODO: moTodo(t); break;
    case MO_DONE: moDone(t); break;
    case MO_SOLO: moSolo(t); break;
    default:      moAch(t);  break;
  }
  return true;
}

/* ---- notification banner: slides up at the bottom, scrolls, slides away */
void notifyIcon(uint8_t k, int x, int y) {               // 9x9
  switch (k) {
    case 1:                                               // github cat
      display.fillCircle(x + 4, y + 5, 4, C_WHITE);
      display.fillTriangle(x, y + 3, x + 1, y, x + 3, y + 2, C_WHITE);
      display.fillTriangle(x + 8, y + 3, x + 7, y, x + 5, y + 2, C_WHITE);
      display.drawPixel(x + 3, y + 5, C_BLACK); display.drawPixel(x + 5, y + 5, C_BLACK);
      break;
    case 2:                                               // chat bubble
      display.drawRoundRect(x, y, 9, 7, 2, C_WHITE);
      display.drawLine(x + 2, y + 6, x + 1, y + 8, C_WHITE);
      display.drawPixel(x + 2, y + 3, C_WHITE); display.drawPixel(x + 4, y + 3, C_WHITE);
      display.drawPixel(x + 6, y + 3, C_WHITE);
      break;
    case 3:                                               // check
      for (int o = 0; o < 2; o++) {
        display.drawLine(x, y + 4 + o, x + 3, y + 7 + o, C_WHITE);
        display.drawLine(x + 3, y + 7 + o, x + 8, y + 1 + o, C_WHITE);
      }
      break;
    case 4:                                               // calendar
      display.drawRect(x, y + 1, 9, 8, C_WHITE);
      display.drawFastHLine(x, y + 3, 9, C_WHITE);
      display.drawPixel(x + 2, y, C_WHITE); display.drawPixel(x + 6, y, C_WHITE);
      break;
    default:                                              // bell
      display.fillCircle(x + 4, y + 4, 3, C_WHITE);
      display.fillRect(x + 1, y + 4, 7, 3, C_WHITE);
      display.drawFastHLine(x, y + 7, 9, C_WHITE);
      display.drawPixel(x + 4, y + 8, C_WHITE);
  }
}

void startNotify(uint8_t icon, const char* text) {
  strncpy(ntText, text, sizeof(ntText) - 1);
  ntText[sizeof(ntText) - 1] = 0;
  ntIcon = icon;
  ntAt = millis() ? millis() : 1;
  int w = (int)strlen(ntText) * 6;
  ntMs = w <= 140 ? 5000 : 2200 + (uint32_t)(w - 95) * 22 + 1800;
  if (soundOn && !gameMode) { sndPushV(1319, 1319, 60, 25, 40); sndPushV(1047, 1047, 100, 0, 35); }
}

void drawNotify() {
  if (!ntAt) return;
  int t = (int)(millis() - ntAt);
  if (t > (int)ntMs) { ntAt = 0; return; }
  // "source: message" -> title + message
  char title[16];
  const char* body = ntText;
  const char* colon = strstr(ntText, ": ");
  if (colon && colon - ntText <= 15) {
    int n = (int)(colon - ntText);
    memcpy(title, ntText, n); title[n] = 0;
    body = colon + 2;
  } else {
    strcpy(title, ntIcon == 1 ? "GitHub" : ntIcon == 2 ? "Message" : ntIcon == 4 ? "Calendar" : "Notification");
  }
  int w = (int)strlen(body) * 6, lane = 95, scroll = 0;
  if (w > lane) {
    scroll = t > 1300 ? (t - 1300) / 22 : 0;
    if (scroll > w - lane) scroll = w - lane;
  }
  int y = cardY(t, (int)ntMs);
  drawCard(y, title, body, scroll);
  notifyIcon(ntIcon, 9, y + 8);
}

/* ---- to-do strip: one segment per item along the top edge */
void drawTodoStrip() {
  if (!live || !tdTotal || millis() - tdAt > 900000UL) return;
  if (tdDone >= tdTotal && millis() - tdAt > 30000) return;
  int n = tdTotal > 32 ? 32 : tdTotal, sw = SCREEN_WIDTH / n;
  for (int i = 0; i < n; i++) {
    int x = i * sw, w = sw > 2 ? sw - 2 : 1;
    if (i < tdDone) display.fillRect(x, 0, w, 2, C_WHITE);
    else if (i == tdDone && (millis() / 400) % 2) display.drawFastHLine(x, 0, w, C_WHITE);
    else for (int px = x; px < x + w; px += 3) display.drawPixel(px, 0, C_WHITE);
  }
}

void drawFrame() {
  updateBlink();
  updateClock();
  musicSmooth += (musicLevel - musicSmooth) * 0.06f;           // eased per frame: smooth movement
  beatGlow    += (beatPulse() - beatGlow) * 0.15f;
  if (musicOn) discAngle += 0.07f + musicSmooth * 0.0008f;     // the record spins while playing
  display.clearDisplay();

  if (menuOpen) {
    drawSettings(tick);
  } else if (gameMode) {
    gameStep();
    if (gameMode) gameDraw();          // gameStep may have exited
  } else if (introAt && drawIntro()) { // laptop just connected: "Build something great"
  } else if (moLen && drawMoment()) {  // you're back / meeting / to-do / achievement
  } else if (petting) {                // hold overrides the active scene
    sc_petted(tick);
    txtC(56, 1, "petted");
  } else {
    SCENES[scene].draw(tick);
    drawCaption(SCENES[scene].cap, SCENES[scene].capStyle, tick);
    sceneSfx(scene, tick);
    drawTicker();
    drawTodoStrip();
  }
  if (!introAt && !moAt && !menuOpen && !gameMode) drawNotify();

  if (!introAt) drawToast();           // nothing pops over the intro
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
      touchBusy = gameMode || menuOpen || introAt || moAt;
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
  if (touchState && !gameMode && !petting && !holdConsumed && now - touchDown > holdMs &&
      (menuOpen || !touchBusy)) {
    if (menuOpen) { menuHold(); holdConsumed = true; }
    else {
      petting = true; tapCount = 0; sndPetStart();
      if (now - lastPetXp > 60000 || lastPetXp == 0) { addXp(1); lastPetXp = now; }
    }
  }

  // resolve taps once the multi-tap window closes
  if (tapCount && now - lastTap > TAP_WINDOW_MS) {
    if (scene == 64 && (musicOn || musicPaused) && tapCount >= 2) {
      // on the record player: 2 taps play/pause, 3 next, 4 previous (1 tap still changes face)
      musicButton(tapCount == 2 ? 2 : tapCount == 3 ? 3 : 1);
    } else if (tapCount == 1) {
      setScene(scene + 1);
      autoCycle = false;
      manualAt = now;                  // you picked this face: music won't pull it back for a while
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
  if      (c == 'n') { setScene(scene + 1);               autoCycle = false; live = false; manualAt = millis(); sndTap(); }
  else if (c == 'p') { setScene(scene + SCENE_COUNT - 1); autoCycle = false; live = false; manualAt = millis(); sndTap(); }
  else if (c == 'r') { setScene(random(SCENE_COUNT));     autoCycle = false; live = false; manualAt = millis(); sndTap(); }
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
/* one parser per link (0 = USB, 1 = Bluetooth) so they never mix lines */
char    lineBuf[2][72];
uint8_t lineLen[2] = { 0, 0 };
bool    inLine[2]  = { false, false };

void feedChar(uint8_t src, char c) {
  if (inLine[src]) {
    if (c == '\n' || c == '\r') {
      lineBuf[src][lineLen[src]] = 0;
      if (lineLen[src] > 2) handleLine(lineBuf[src]);
      inLine[src] = false; lineLen[src] = 0;
    } else if (lineLen[src] < sizeof(lineBuf[src]) - 1) {
      lineBuf[src][lineLen[src]++] = c;
    }
  } else if (c >= 'A' && c <= 'Z') {
    inLine[src] = true; lineLen[src] = 0; lineBuf[src][lineLen[src]++] = c;
  } else if (src == 0) {
    handleKey(c);           // single-key commands: USB serial monitor only. Over Bluetooth a
  }                         // dropped packet would turn a commit message into commands
}

void handleSerial() {
  while (Serial.available()) feedChar(0, Serial.read());
#if USE_BLE
  char c;
  while (bleQ && xQueueReceive(bleQ, &c, 0) == pdTRUE) feedChar(1, c);
#endif
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
  // you stopped typing: drop the typing reaction
  if (keyScene != 255) {
    if (scene != keyScene) keyScene = 255;
    else if (now - lastKeyAt > 4000 && !gameMode) { keyScene = 255; setScene(0); }
  }

  // mid-turn and no tool running (Claude is thinking / writing): show the spinner
  if (busy && live && !gameMode && !menuOpen && !petting && followStage == 0 &&
      scene != 10 && scene != 44 && now - lastToolAt > SPIN_AFTER_MS) {
    if (scene != 54 && scene != 67) setScene(54);
    else if (now - sceneSince > 9000) setScene(scene == 54 ? 67 : 54);   // spinner <-> fixing wires
  }

  // while a turn is running, stay synced (no random faces) — up to 15 min of silence
  bool turnAlive = busy && now - lastEvent < 900000UL;
  if (busy && !turnAlive) busy = false;
  if (live && !turnAlive && now - lastEvent > LIVE_TIMEOUT_MS) {
    live = false; autoCycle = true; sceneSince = now;
  }
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
  love     = prefs.getUChar("love", 0);
  prefs.end();
  while (level < 250 && xp >= xpNeed(level + 1)) level++;
  if (briIndex >= BRI_COUNT) briIndex = 5;
  if (volIndex >= VOL_COUNT) volIndex = 3;
  setSoundMode(m > SND_ALL ? SND_ALL : m);
  applyBrightness();
  buzzInit();
#if USE_BLE
  bleInit();                              // wireless link to the laptop bridge
#endif

  display.clearDisplay();
  display.display();

  randomSeed(esp_random());
  scheduleBlink();
  setScene(0);
  startBootIntro();                       // CRT power-on -> "Build something great"
  Serial.printf("%u scenes. tap=next, 2x=settings, 3x=game, 4x=mute, hold=pet.\n",
                SCENE_COUNT);
}

void loop() {
  handleTouch();
  handleSerial();
  if (bridgeUp && millis() - lastBridgeAt > BRIDGE_TIMEOUT_MS) goSolo();   // laptop's gone
#if USE_BLE
  static bool bleWas = false;              // the laptop linked up / dropped over Bluetooth
  if (bleConnected != bleWas) {
    bleWas = bleConnected;
    inLine[1] = false; lineLen[1] = 0;     // a new link starts with a clean line
    linkToastUp = bleWas; linkToastAt = millis();
    if (soundOn) {
      sndClear();
      if (bleWas) { sndPushV(1047, 1047, 40, 10, 45); sndPushV(1568, 1568, 60, 0, 50); }
      else        { sndPushV(880, 440, 220, 0, 45); }
    }
  }
#endif
  updateLive();
  petUpdate();                            // sound itself plays in soundTask

  // nobody around for a while: Claude goes back to living on its own
  if (!autoCycle && !live && !petting && !menuOpen && !gameMode && millis() - lastInput > RESUME_MS) {
    autoCycle = true; sceneDur = 0;
  }
  // music on the laptop: dance to it (Claude's real work still comes first)
  if ((musicOn || musicPaused) && millis() - musicAt > 5000) musicOn = musicPaused = false;
  bool musicNow = (musicOn || musicPaused) && !busy && !gameMode && !menuOpen && !petting &&
                  followStage == 0 && scene != 10 &&
                  (manualAt == 0 || millis() - manualAt > RESUME_MS);   // you changed face: leave it
  // music, 12 s per face:
  //   singing:        lyrics -> disk or equalizer (taking turns) -> lyrics ...
  //   just the tune:  DJ set -> disk -> equalizer -> DJ set ...
  static bool eqNext = false;
  bool inMus = scene == 64 || scene == 16 || scene == 66 || scene == 68;
  uint32_t la = millis() - lyricAt;
  bool lyricsNow = lyricsOn && musicOn && !musicPaused &&
                   lyricText[0] && la < lyricMs + 3000UL;      // a break in the singing: the tune
  bool due = millis() - sceneSince > 12000UL;
  if (musicNow) {
    musicMode = true;
    if (musicPaused) {                             // paused: keep the player up to resume it
      if (scene != 64) setScene(64);
    } else if (lyricsNow) {
      if (scene != 66 && scene != 64 && scene != 16) setScene(66);
      else if (due && scene == 66) { setScene(eqNext ? 16 : 64); eqNext = !eqNext; }   // a break from the words
      else if (due) setScene(66);                  // back to singing along
    } else if (!inMus || scene == 66) {            // just the tune: the DJ takes the stage
      setScene(68);
    } else if (due) {
      setScene(scene == 68 ? 64 : scene == 64 ? 16 : 68);   // DJ -> disk -> equalizer -> DJ
    }
  } else if (musicMode && !musicOn && !musicPaused) {   // the music stopped
    musicMode = false;
    if (inMus) { setScene(0); sceneDur = 4000; }
  }

  // you're working, Claude is between turns: it lives a little on its own
  bool aliveIdle = live && !busy && followStage == 0 && scene != 10 &&
                   millis() - lastEvent > ALIVE_AFTER_MS;
  if ((autoCycle || aliveIdle) && !musicNow && !petting && !menuOpen && !gameMode) director(!autoCycle);

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