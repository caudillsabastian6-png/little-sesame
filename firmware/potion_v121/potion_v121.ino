// POTION firmware v1.21 -- CoreS3 desk body.
// v1: v0 status screen stays as screen 1, plus TALK / MIC / TIME.
// v1.1: room-first voice routing (bridge decides per exchange).
// v1.2 adds three screens the owner picked from his War Room pages:
//   CREW      -- the Tiered Crew chart, live seat lines from the bridge,
//                touch-drag scrollable.
//   COUNTDOWN -- Real or Replica submission deadline (Wed Nov 4 2026,
//                9:00 PM ET), plus a WHAT'S LEFT page fed by the bridge
//                (/hackathons). TEMPORARY: retires itself from the
//                rotation 24h after the deadline.
//   MACHINES  -- the machine chain with live-checked status dots.
// v1.21 hotfix (owner reports, Nov 1):
//   - MIC screen REMOVED from the rotation by owner order ("we don't
//     need that microphone meter"). The ES7210 hardware path and the
//     MICG/MICCHK serial calibration harness stay for the ears build.
//   - Screen switches never wait on the network: bridge-fed screens
//     (CREW / COUNTDOWN / MACHINES) draw instantly from last-known data
//     and refresh in the background; the switch itself is render-only.
//   - Owner drawing rule: screens whose content ticks or refreshes
//     update ONLY the regions that changed (see the CD/TIME counters
//     in STATS) -- no full-screen repaint for a data refresh.
// Owner UI law: one thing per screen, big full-width buttons, plain words,
// no numbered menus. Navigation = tap the left or right edge arrow.
// Serial heartbeat every 5s (POTION_STATUS), plus SCREEN and PRESS lines.
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "wifi_secrets.h"
#include "bridge_secrets.h"

// Data shapes for the bridge-fed screens. These MUST sit before every
// function: Arduino hoists generated prototypes to just after the includes,
// and the CREW/MACHINES draw functions take these structs by pointer.
struct CrewSeat { char label[28]; char name[24]; char role[56]; char status[44]; };
struct MachineRow { char name[32]; char state[12]; char line[36]; };

// ---------------------------------------------------------------- screens
// v1.21: MIC is out of the rotation (owner order), so indices shifted:
// TIME 3->2, CREW 4->3, COUNTDOWN 5->4, MACHINES 6->5. Serial GOTO uses
// these indices.
enum Screen : int { SCR_STATUS = 0, SCR_TALK = 1, SCR_TIME = 2,
                    SCR_CREW = 3, SCR_COUNTDOWN = 4, SCR_MACHINES = 5, SCR_COUNT = 6 };
static const char *kScreenNames[SCR_COUNT] = { "STATUS", "TALK", "TIME",
                                               "CREW", "COUNTDOWN", "MACHINES" };
static int g_screen = SCR_STATUS;

// Countdown target: Wed Nov 4, 2026, 9:00 PM America/New_York (EST, UTC-5;
// DST ended Nov 1). Built with mktime in board-local time below; epoch
// cross-check: 1793844000. The screen retires 24h after the deadline.
static const long CD_TARGET_EPOCH = 1793844000L;
static const long CD_RETIRE_EPOCH = 1793930400L;  // target + 24h

static bool countdownRetired() {
  time_t now = time(nullptr);
  if (now < 1600000000L) return false;  // clock not set yet: never retire blind
  return now > CD_RETIRE_EPOCH;
}

// A screen can be skipped by the rotation (the retired countdown).
static bool screenVisible(int s) {
  if (s == SCR_COUNTDOWN && countdownRetired()) return false;
  return true;
}

// Next/prev VISIBLE screen from `from`, stepping dir (+1 / -1).
static int navStep(int from, int dir) {
  int s = from;
  for (int i = 0; i < SCR_COUNT; i++) {
    s = (s + dir + SCR_COUNT) % SCR_COUNT;
    if (screenVisible(s)) return s;
  }
  return from;
}

// ---------------------------------------------------------------- state
static bool g_shownConnected = false;
static bool g_bootTested = false;
static uint32_t g_lastBeat = 0;
static uint32_t g_lastCheck = 0;
static int g_dots = 0;

// TALK result banner
static bool g_talkResult = false;
static bool g_talkOk = false;
static uint32_t g_talkResultUntil = 0;

// MIC hardware state (the screen is gone in v1.21; the ES7210 path and
// its serial calibration harness stay for the ears build)
// Mic input magnification (M5Unified mic_config_t multiplier; library
// default 16). Calibrated against room ambient over serial (MICG/MICCHK):
// highest value whose ambient peaks stay clear of the rail wins.
static uint8_t g_micMag = 16;

static void applyMicConfig() {
  auto mc = M5.Mic.config();
  mc.magnification = g_micMag;
  mc.sample_rate = 16000;
  M5.Mic.config(mc);
}

// TIME
static int g_lastShownSec = -1;
static char g_timeClockStr[16] = "";   // last clock string painted (partial tick erases it)
static char g_timeDateStr[32] = "";    // date string of the last full TIME paint

// ------------------------------------------------- v1.21: deferred refresh + draw stats
// Bridge-fed screens draw from cache at switch time and set a pending
// flag; loop() runs the fetch later and repaints only what changed.
// The counters below are the serial proof (STATS command).
static bool g_pendCrew = false;
static bool g_pendHack = false;
static bool g_pendMachines = false;
static int g_navCount = 0;
static uint32_t g_lastNavMs = 0;
static int g_cdFull = 0;        // full paints of the countdown clock page
static int g_cdTick = 0;        // partial per-second updates of the clock lines
static int g_timeFull = 0;      // full paints of the TIME screen
static int g_timeTick = 0;      // partial per-second updates of the TIME clock line
static int g_crewRepaint = 0;   // refresh landed changed data -> chart repainted
static int g_crewSkip = 0;      // refresh landed identical data -> no repaint
static int g_machRowPaint = 0;  // machine rows repainted (changed rows only)
static int g_machSkip = 0;      // refresh landed identical rows -> no repaint

// ---------------------------------------------------------------- helpers
static void drawNavArrows() {
  auto &d = M5.Display;
  // left / right edge triangles, vertically centered
  d.fillTriangle(6, 120, 30, 104, 30, 136, TFT_DARKGREY);
  d.fillTriangle(314, 120, 290, 104, 290, 136, TFT_DARKGREY);
}

static void drawScreenTitle(const char *title) {
  auto &d = M5.Display;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString(title, d.width() / 2, 26);
}

static void drawCheck(int cx, int cy, int r, uint16_t color) {
  auto &d = M5.Display;
  d.fillCircle(cx, cy, r, color);
  int s = r;  // scale strokes with radius
  for (int o = -1; o <= 1; o++) {
    d.drawLine(cx - (s * 11) / 21, cy + o, cx - (s * 3) / 21, cy + (s * 9) / 21 + o, TFT_BLACK);
    d.drawLine(cx - (s * 3) / 21, cy + (s * 9) / 21 + o, cx + (s * 12) / 21, cy - (s * 8) / 21 + o, TFT_BLACK);
  }
}

// ---------------------------------------------------------------- screen 1: STATUS (v0 behavior + her name line)
// Her name is Sesame (owner naming, Nov 1). POTION stays the big title;
// "Sesame" sits under it as her name line.
static void drawHeader() {
  auto &d = M5.Display;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextSize(5);
  d.drawString("POTION", d.width() / 2, 38);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.setTextSize(3);
  d.drawString("Sesame", d.width() / 2, 74);
}

// Joining animation: full paint once on entry; after that only the dots
// line changes (owner drawing rule) -- the old dots are overdrawn in
// black and the new ones drawn at the same spot.
static bool g_joiningUp = false;
static char g_joinDotsStr[8] = "";

static void drawJoining() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  drawHeader();
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("Joining Wi-Fi", d.width() / 2, 138);
  String dots = "";
  for (int i = 0; i < g_dots; i++) dots += ".";
  d.setTextSize(3);
  d.drawString(dots, d.width() / 2, 176);
  snprintf(g_joinDotsStr, sizeof(g_joinDotsStr), "%s", dots.c_str());
  drawNavArrows();
  g_joiningUp = true;
}

static void drawJoiningDots() {
  if (!g_joiningUp) { drawJoining(); return; }
  auto &d = M5.Display;
  String dots = "";
  for (int i = 0; i < g_dots; i++) dots += ".";
  if (dots == g_joinDotsStr) return;
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.setTextColor(TFT_BLACK, TFT_BLACK);
  d.drawString(g_joinDotsStr, d.width() / 2, 176);  // erase the old dots
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString(dots, d.width() / 2, 176);
  snprintf(g_joinDotsStr, sizeof(g_joinDotsStr), "%s", dots.c_str());
}

static void drawConnected() {
  g_joiningUp = false;
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  drawHeader();
  int cx = d.width() / 2;
  drawCheck(cx, 112, 21, TFT_GREEN);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("Connected", cx, 158);
  d.setTextSize(3);
  d.drawString(WiFi.localIP().toString(), cx, 202);
  drawNavArrows();
}

static void drawStatus() {
  if (WiFi.status() == WL_CONNECTED) drawConnected();
  else drawJoining();
}

// ---------------------------------------------------------------- screen 2: TALK
static void drawTalk() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  drawScreenTitle("TALK");
  // one huge button, center of the screen
  d.fillRoundRect(58, 62, 204, 104, 14, TFT_BLUE);
  d.setTextColor(TFT_WHITE, TFT_BLUE);
  d.setTextDatum(middle_center);
  d.setTextSize(5);
  d.drawString("TALK", d.width() / 2, 114);
  if (g_talkResult) {
    d.setTextDatum(middle_center);
    d.setTextSize(3);
    if (g_talkOk) {
      d.setTextColor(TFT_GREEN, TFT_BLACK);
      d.drawString("Talk sent", d.width() / 2, 204);
    } else {
      d.setTextColor(TFT_ORANGE, TFT_BLACK);
      d.drawString("Can't reach Allie", d.width() / 2, 204);
    }
  }
  drawNavArrows();
}

// ------------------------------------------------- voice clips (Potion's app voice)
// Served by the bridge from its voice\ folder so clips stay updatable
// without a reflash. If a clip is missing the board skips playback
// gracefully (VOICE_SKIP in the serial log) -- it never fakes a sound.
static const char *VOICE_TALK_CLIP  = "im-here-boss.wav";           // TALK press ack
static const char *VOICE_HELLO_CLIP = "hello-boss.wav";             // once after boot / first connect
static const char *VOICE_WAKE_CLIP  = "open-sesame-listening.wav";  // reserved: playback lands with the wake-word step

// ------------------------------------------------- Potion's face while she talks
// The bridge serves her picture (potion-face-320x240.jpg, the owner's own
// avatar art) at /face. Fetched once, cached in PSRAM; shown full-screen
// for the whole playback, then the previous screen is redrawn.
static uint8_t *g_faceBuf = nullptr;
static size_t g_faceLen = 0;

static bool fetchFace() {
  if (g_faceBuf) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + "/face";
  http.begin(client, url);
  http.addHeader("X-Potion-Token", BRIDGE_TOKEN);
  http.setTimeout(6000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("FACE_SKIP http=%d\n", code);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len <= 0 || len > 262144) {
    Serial.printf("FACE_SKIP bad_size len=%d\n", len);
    http.end();
    return false;
  }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(len);
  if (!buf) { http.end(); return false; }
  int got = http.getStreamPtr()->readBytes(buf, len);
  http.end();
  if (got != len) {
    heap_caps_free(buf);
    Serial.printf("FACE_SKIP short_read got=%d len=%d\n", got, len);
    return false;
  }
  g_faceBuf = buf;
  g_faceLen = (size_t)len;
  Serial.printf("FACE_CACHED bytes=%d\n", len);
  return true;
}

static void redrawCurrent() {
  switch (g_screen) {
    case SCR_STATUS:    drawStatus();    break;
    case SCR_TALK:      drawTalk();      break;
    case SCR_TIME:      drawTime();      break;
    case SCR_CREW:      drawCrew();      break;
    case SCR_COUNTDOWN: drawCountdown(); break;
    case SCR_MACHINES:  drawMachines();  break;
    default: break;
  }
}

static void gotoScreen(int s);  // defined in the navigation section below

// ------------------------------------------- the CONVERSATION screen (her face)
// Owner refinement: the face is not a splash -- it IS the conversation
// screen. It opens when a voice exchange starts and stays for the whole
// exchange (waiting on Allie, and while her voice plays). It leaves only
// when the exchange is over (playback done + a short idle beat) or when
// he navigates away with the edge tap zones. Small plain status line at
// the bottom so he knows the state without losing the picture.
static bool g_faceUp = false;
static char g_faceStatus[24] = "";

static void drawFaceStatus() {
  auto &d = M5.Display;
  d.fillRect(0, 214, d.width(), 26, TFT_BLACK);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextSize(2);
  d.drawString(g_faceStatus, d.width() / 2, 227);
}

static bool showFace(const char *status) {
  if (g_faceUp) { setFaceStatus(status); return true; }
  if (!fetchFace()) return false;
  snprintf(g_faceStatus, sizeof(g_faceStatus), "%s", status);
  bool drawn = M5.Display.drawJpg(g_faceBuf, g_faceLen, 0, 0);
  Serial.printf("FACE_SHOW drawn=%d\n", drawn ? 1 : 0);
  if (!drawn) return false;
  g_faceUp = true;
  drawFaceStatus();
  return true;
}

static void setFaceStatus(const char *status) {
  snprintf(g_faceStatus, sizeof(g_faceStatus), "%s", status);
  if (g_faceUp) drawFaceStatus();
}

static void closeFace() {
  if (!g_faceUp) return;
  g_faceUp = false;
  Serial.println("FACE_CLOSE -- exchange over");
  redrawCurrent();
}

// While the face is up, watch for an edge tap = "I'm done here": stop any
// playback, leave the face screen, perform the nav. Returns true if he
// navigated away.
static bool pollFaceNav() {
  M5.update();
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      auto &d = M5.Display;
      if (t.x < 48 || t.x > d.width() - 48) {
        M5.Speaker.stop();
        g_faceUp = false;
        Serial.println("FACE_CLOSE -- he navigated away");
        gotoScreen(navStep(g_screen, t.x < 48 ? -1 : 1));
        return true;
      }
    }
  }
  return false;
}

// ------------------------------------------- voice routing (v1.1, room-first)
// Per exchange the bridge decides the route, never cached:
//   route=room  -> the bridge plays the clip on Allie (Bluetooth speaker);
//                  the board stays silent so there is never an echo.
//   route=local -> the board downloads the clip and plays it herself.
// If the bridge does not answer within ~2.5s the board plays locally --
// he is never left in silence.
static bool g_lastPressOk = false;

// Download a clip from the bridge and play it on the board's own speaker.
// The conversation screen (face) is already up and stays up; the caller
// closes the exchange.
static void playClipLocal(const char *name) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("VOICE_SKIP name=%s reason=no_wifi\n", name);
    return;
  }
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + "/voice/" + name;
  http.begin(client, url);
  http.addHeader("X-Potion-Token", BRIDGE_TOKEN);
  http.setTimeout(6000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("VOICE_SKIP name=%s http=%d\n", name, code);
    http.end();
    return;
  }
  int len = http.getSize();
  if (len <= 44 || len > 1048576) {
    Serial.printf("VOICE_SKIP name=%s reason=bad_size len=%d\n", name, len);
    http.end();
    return;
  }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(len);
  if (!buf) {
    Serial.printf("VOICE_SKIP name=%s reason=no_mem len=%d\n", name, len);
    http.end();
    return;
  }
  int got = http.getStreamPtr()->readBytes(buf, len);
  http.end();
  if (got != len) {
    Serial.printf("VOICE_SKIP name=%s reason=short_read got=%d len=%d\n", name, got, len);
    heap_caps_free(buf);
    return;
  }
  setFaceStatus("Talking...");
  M5.Speaker.setVolume(255);  // owner order: speaker at maximum for all playback
  bool started = M5.Speaker.playWav(buf, (size_t)len);
  Serial.printf("VOICE_PLAY name=%s bytes=%d started=%d route=local\n", name, len, started ? 1 : 0);
  uint32_t t0 = millis();
  while (M5.Speaker.isPlaying() && millis() - t0 < 9000) {
    if (pollFaceNav()) break;  // he navigated away; pollFaceNav stopped playback
    delay(30);
  }
  heap_caps_free(buf);
}

// Notify the bridge of an event (hello/press) and learn the route.
// Returns true when the bridge answered 200 (the event was logged there).
static bool bridgeNotify(const char *path, const char *source, String &route, long &playMs) {
  route = "local";
  playMs = 0;
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + path;
  http.begin(client, url);
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  http.addHeader("X-Potion-Token", BRIDGE_TOKEN);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"source\":\"") + source + "\"}";
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("ROUTE bridge_no_answer path=%s http=%d -> local\n", path, code);
    http.end();
    return false;
  }
  String resp = http.getString();
  http.end();
  if (resp.indexOf("\"room\"") >= 0) route = "room";
  int i = resp.indexOf("play_ms");
  if (i >= 0) {
    int colon = resp.indexOf(':', i);
    if (colon >= 0) playMs = resp.substring(colon + 1).toInt();
  }
  Serial.printf("ROUTE path=%s route=%s play_ms=%ld resp=%s\n", path, route.c_str(), playMs, resp.c_str());
  return true;
}

// One full voice exchange: face up -> notify bridge -> obey the route ->
// short idle beat with the face still up -> face closes.
static void voiceExchange(const char *clipName, bool isHello) {
  showFace("Talking...");
  String route;
  long playMs = 0;
  bool reached = bridgeNotify(isHello ? "/hello" : "/press",
                              isHello ? "board-boot" : "board", route, playMs);
  g_lastPressOk = reached;
  if (reached && route == "room") {
    // The bridge is playing her voice in the room; the board stays silent.
    uint32_t wait = playMs > 0 ? (uint32_t)playMs : 3000;
    uint32_t t0 = millis();
    while (millis() - t0 < wait) {
      if (pollFaceNav()) break;
      delay(30);
    }
  } else {
    playClipLocal(clipName);
  }
  if (g_faceUp) {  // short idle beat, face still up, unless he navigated away
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
      if (pollFaceNav()) break;
      delay(30);
    }
  }
  closeFace();
}

// ---------------------------------------------------------------- screen 3: MIC -- REMOVED in v1.21
// Owner order (Nov 1): "We don't need that microphone meter -- go ahead
// and take that off of there." The screen, its meter task and its bar
// drawing are gone. The ES7210 capture path itself is untouched, and the
// MICG / MICCHK serial calibration harness (below, in the serial section)
// stays in the firmware: the future ears build uses these same mics and
// that harness is how the gain gets tuned against the owner's voice.

// ---------------------------------------------------------------- screen 3: TIME
// Full paint on entry; after that only the clock line is updated each
// second (owner drawing rule): the previous string is overdrawn in black
// and the new one drawn at the same spot. The date line repaints only
// when the day actually changes.
static void drawTime() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  drawScreenTitle("TIME");
  struct tm tmv;
  d.setTextDatum(middle_center);
  if (!getLocalTime(&tmv, 10)) {
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setTextSize(3);
    d.drawString("Setting clock...", d.width() / 2, 130);
    g_timeClockStr[0] = 0;
    g_timeDateStr[0] = 0;
    drawNavArrows();
    return;
  }
  char tbuf[16], dbuf[32];
  strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
  strftime(dbuf, sizeof(dbuf), "%a, %b %d, %Y", &tmv);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(6);
  d.drawString(tbuf, d.width() / 2, 116);
  d.setTextSize(3);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(dbuf, d.width() / 2, 182);
  snprintf(g_timeClockStr, sizeof(g_timeClockStr), "%s", tbuf);
  snprintf(g_timeDateStr, sizeof(g_timeDateStr), "%s", dbuf);
  g_timeFull++;
  drawNavArrows();
}

// Per-second partial update while sitting on TIME.
static void tickTime() {
  struct tm tmv;
  if (!getLocalTime(&tmv, 5)) return;
  if (g_timeClockStr[0] == 0) { drawTime(); return; }  // was in "Setting clock..." state
  char tbuf[16], dbuf[32];
  strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
  strftime(dbuf, sizeof(dbuf), "%a, %b %d, %Y", &tmv);
  if (strcmp(dbuf, g_timeDateStr) != 0) { drawTime(); return; }  // day rolled over
  if (strcmp(tbuf, g_timeClockStr) == 0) return;
  auto &d = M5.Display;
  d.setTextDatum(middle_center);
  d.setTextSize(6);
  d.setTextColor(TFT_BLACK, TFT_BLACK);
  d.drawString(g_timeClockStr, d.width() / 2, 116);  // erase the old digits
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(tbuf, d.width() / 2, 116);
  snprintf(g_timeClockStr, sizeof(g_timeClockStr), "%s", tbuf);
  g_timeTick++;
}

// ------------------------------------------------- shared bridge JSON fetch
// Same shape as fetchFace: token header, honest failure (false + a serial
// line) -- the screens show "Can't reach Allie" rather than stale fiction.
static bool fetchBridgeJson(const char *path, String &out) {
  out = "";
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("FETCH_SKIP path=%s reason=no_wifi\n", path);
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + path;
  http.begin(client, url);
  http.addHeader("X-Potion-Token", BRIDGE_TOKEN);
  http.setTimeout(6000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("FETCH_SKIP path=%s http=%d\n", path, code);
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return true;
}

// Pull the string value of "key" out of a JSON blob, searching from pos.
// Values in our bridge JSON carry no escapes, so a plain scan is honest.
// Returns the index just past the value, or -1 when not found.
static int jsonStr(const String &json, const char *key, int from, char *dst, size_t cap) {
  String pat = String("\"") + key + "\"";
  int i = json.indexOf(pat, from);
  if (i < 0) return -1;
  int colon = json.indexOf(':', i + pat.length());
  if (colon < 0) return -1;
  int q1 = json.indexOf('"', colon);
  if (q1 < 0) return -1;
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return -1;
  size_t n = (size_t)(q2 - q1 - 1);
  if (n >= cap) n = cap - 1;
  memcpy(dst, json.c_str() + q1 + 1, n);
  dst[n] = 0;
  return q2 + 1;
}

// Count the wrapped lines `text` needs at the given text size and width.
static int wrappedLines(const char *text, int maxW, int size) {
  auto &d = M5.Display;
  d.setTextSize(size);
  String rest = text;
  int lines = 0;
  while (rest.length()) {
    lines++;
    if (d.textWidth(rest) <= maxW) break;
    int cut = rest.length();
    while (cut > 0 && d.textWidth(rest.substring(0, cut)) > maxW) cut--;
    int sp = rest.lastIndexOf(' ', cut - 1);
    if (sp > 0) cut = sp;
    rest = rest.substring(cut);
    rest.trim();
  }
  return lines ? lines : 1;
}

// Draw `text` word-wrapped, horizontally centered on cx. Returns height used.
static int drawWrappedCentered(const char *text, int cx, int y, int maxW,
                               int size, uint32_t color, int lineH,
                               uint32_t bg = TFT_BLACK) {
  auto &d = M5.Display;
  d.setTextSize(size);
  d.setTextColor(color, bg);
  d.setTextDatum(top_center);
  String rest = text;
  int lines = 0;
  while (rest.length()) {
    String line = rest;
    if (d.textWidth(line) > maxW) {
      int cut = line.length();
      while (cut > 0 && d.textWidth(line.substring(0, cut)) > maxW) cut--;
      int sp = line.lastIndexOf(' ', cut - 1);
      if (sp > 0) cut = sp;
      line = line.substring(0, cut);
      rest = rest.substring(cut);
      rest.trim();
    } else {
      rest = "";
    }
    d.drawString(line, cx, y + lines * lineH);
    lines++;
    if (lines >= 6) break;
  }
  d.setTextDatum(middle_center);
  return lines * lineH;
}

// Draw `text` word-wrapped, left-aligned at x. Returns height used.
static int drawWrappedLeft(const char *text, int x, int y, int maxW,
                           int size, uint32_t color, int lineH,
                           uint32_t bg = TFT_BLACK) {
  auto &d = M5.Display;
  d.setTextSize(size);
  d.setTextColor(color, bg);
  d.setTextDatum(top_left);
  String rest = text;
  int lines = 0;
  while (rest.length()) {
    String line = rest;
    if (d.textWidth(line) > maxW) {
      int cut = line.length();
      while (cut > 0 && d.textWidth(line.substring(0, cut)) > maxW) cut--;
      int sp = line.lastIndexOf(' ', cut - 1);
      if (sp > 0) cut = sp;
      line = line.substring(0, cut);
      rest = rest.substring(cut);
      rest.trim();
    } else {
      rest = "";
    }
    d.drawString(line, x, y + lines * lineH);
    lines++;
    if (lines >= 6) break;
  }
  d.setTextDatum(middle_center);
  return lines * lineH;
}

// ------------------------------------------------- screen 4: CREW (Tiered Crew)
// Chart data + live seat status lines come from the bridge (GET /crew,
// served from crew.json on Allie) so words can change without a reflash.
// Drawn natively at 320 wide; the content is taller than the screen and
// scrolls by touch-drag. (struct CrewSeat itself is defined at the top.)
static CrewSeat g_seats[6];
static int g_seatCount = 0;
static char g_crewTitle[36] = "The Tiered Crew";
static char g_crewSubtitle[72] = "";
static char g_crewFork[84] = "";
static char g_crewFooter[110] = "";
static bool g_crewOk = false;
static int g_crewScroll = 0;
static int g_crewContentH = 0;

// Fetch + parse into scratch, commit only on a full successful parse --
// a failed refresh keeps the last-known chart (v1.21 cache rule).
// Returns 1 = new data committed, 0 = identical to cache, -1 = failed.
static int fetchCrew() {
  String json;
  if (!fetchBridgeJson("/crew", json)) return -1;
  char buf[128];
  char tTitle[36], tSubtitle[72], tFork[84], tFooter[110];
  snprintf(tTitle, sizeof(tTitle), "%s", g_crewTitle);
  snprintf(tSubtitle, sizeof(tSubtitle), "%s", g_crewSubtitle);
  snprintf(tFork, sizeof(tFork), "%s", g_crewFork);
  snprintf(tFooter, sizeof(tFooter), "%s", g_crewFooter);
  if (jsonStr(json, "title", 0, buf, sizeof(buf)) >= 0) snprintf(tTitle, sizeof(tTitle), "%s", buf);
  if (jsonStr(json, "subtitle", 0, buf, sizeof(buf)) >= 0) snprintf(tSubtitle, sizeof(tSubtitle), "%s", buf);
  if (jsonStr(json, "fork", 0, buf, sizeof(buf)) >= 0) snprintf(tFork, sizeof(tFork), "%s", buf);
  if (jsonStr(json, "footer", 0, buf, sizeof(buf)) >= 0) snprintf(tFooter, sizeof(tFooter), "%s", buf);
  CrewSeat tmp[6];
  int pos = 0;
  int count = 0;
  while (count < 6) {
    CrewSeat &s = tmp[count];
    int p = jsonStr(json, "label", pos, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(s.label, sizeof(s.label), "%s", buf);
    p = jsonStr(json, "name", p, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(s.name, sizeof(s.name), "%s", buf);
    p = jsonStr(json, "role", p, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(s.role, sizeof(s.role), "%s", buf);
    p = jsonStr(json, "status", p, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(s.status, sizeof(s.status), "%s", buf);
    pos = p;
    count++;
  }
  if (count == 0) {
    Serial.println("CREW_FETCH ok=0 seats=0");
    return -1;
  }
  bool changed = !g_crewOk || count != g_seatCount
              || strcmp(tTitle, g_crewTitle) || strcmp(tSubtitle, g_crewSubtitle)
              || strcmp(tFork, g_crewFork) || strcmp(tFooter, g_crewFooter);
  for (int i = 0; !changed && i < count; i++) {
    if (strcmp(tmp[i].label, g_seats[i].label) || strcmp(tmp[i].name, g_seats[i].name)
        || strcmp(tmp[i].role, g_seats[i].role) || strcmp(tmp[i].status, g_seats[i].status))
      changed = true;
  }
  Serial.printf("CREW_FETCH ok=1 seats=%d changed=%d\n", count, changed ? 1 : 0);
  if (!changed) return 0;
  memcpy(g_seats, tmp, sizeof(tmp));
  g_seatCount = count;
  snprintf(g_crewTitle, sizeof(g_crewTitle), "%s", tTitle);
  snprintf(g_crewSubtitle, sizeof(g_crewSubtitle), "%s", tSubtitle);
  snprintf(g_crewFork, sizeof(g_crewFork), "%s", tFork);
  snprintf(g_crewFooter, sizeof(g_crewFooter), "%s", tFooter);
  g_crewOk = true;
  return 1;
}

static const CrewSeat *seatByKey(int idx) {
  return (idx >= 0 && idx < g_seatCount) ? &g_seats[idx] : nullptr;
}

// One full-width seat box in content coordinates; returns the box height.
static int drawSeatBox(const CrewSeat *s, int yTop, uint32_t accent) {
  auto &d = M5.Display;
  if (!s) return 0;
  int roleH = wrappedLines(s->role, 272, 2) * 19;
  int boxH = 62 + roleH + 24;
  if (yTop + boxH >= 0 && yTop <= 240) {
    d.fillRoundRect(10, yTop, 300, boxH, 10, d.color565(22, 22, 34));
    d.drawRoundRect(10, yTop, 300, boxH, 10, accent);
    d.setTextDatum(top_center);
    d.setTextSize(2);
    d.setTextColor(accent, d.color565(22, 22, 34));
    d.drawString(s->label, 160, yTop + 8);
    d.setTextSize(3);
    d.setTextColor(TFT_WHITE, d.color565(22, 22, 34));
    d.drawString(s->name, 160, yTop + 30);
    drawWrappedCentered(s->role, 160, yTop + 62, 272, 2, TFT_LIGHTGREY, 19,
                        d.color565(22, 22, 34));
    d.setTextSize(2);
    d.setTextColor(TFT_GOLD, d.color565(22, 22, 34));
    d.setTextDatum(top_center);
    d.drawString(s->status, 160, yTop + 62 + roleH);
    d.setTextDatum(middle_center);
  }
  return boxH;
}

// One half-width seat box (Basil / the crew side by side); returns height.
static int drawHalfSeatBox(const CrewSeat *s, int x, int yTop, uint32_t accent) {
  auto &d = M5.Display;
  if (!s) return 0;
  int roleH = wrappedLines(s->role, 128, 2) * 17;
  int statH = wrappedLines(s->status, 128, 2) * 17;
  int boxH = 56 + roleH + statH + 8;
  if (yTop + boxH >= 0 && yTop <= 240) {
    d.fillRoundRect(x, yTop, 146, boxH, 10, d.color565(22, 22, 34));
    d.drawRoundRect(x, yTop, 146, boxH, 10, accent);
    int cx = x + 73;
    d.setTextDatum(top_center);
    d.setTextSize(2);
    d.setTextColor(accent, d.color565(22, 22, 34));
    d.drawString(s->label, cx, yTop + 7);
    d.setTextColor(TFT_WHITE, d.color565(22, 22, 34));
    d.drawString(s->name, cx, yTop + 27);
    drawWrappedCentered(s->role, cx, yTop + 52, 128, 2, TFT_LIGHTGREY, 17,
                        d.color565(22, 22, 34));
    drawWrappedCentered(s->status, cx, yTop + 52 + roleH, 128, 2, TFT_GOLD, 17,
                        d.color565(22, 22, 34));
    d.setTextDatum(middle_center);
  }
  return boxH;
}

static void drawCrewScrollHint() {
  auto &d = M5.Display;
  int maxScroll = g_crewContentH - 240;
  if (maxScroll <= 0) return;
  // thin scrollbar on the right edge
  int trackH = 200, y0 = 20;
  int thumbH = trackH * 240 / g_crewContentH;
  if (thumbH < 18) thumbH = 18;
  int thumbY = y0 + (trackH - thumbH) * g_crewScroll / maxScroll;
  d.fillRect(316, y0, 3, trackH, d.color565(50, 50, 60));
  d.fillRect(316, thumbY, 3, thumbH, TFT_GOLD);
}

static void drawCrew() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  if (!g_crewOk) {
    drawScreenTitle("CREW");
    d.setTextColor(TFT_ORANGE, TFT_BLACK);
    d.setTextDatum(middle_center);
    d.setTextSize(3);
    d.drawString("Can't reach Allie", d.width() / 2, 130);
    drawNavArrows();
    return;
  }
  uint32_t gold = d.color565(255, 200, 60);
  uint32_t pink = d.color565(255, 110, 180);
  uint32_t green = d.color565(90, 220, 130);
  uint32_t blue = d.color565(100, 170, 255);
  int cy = 6 - g_crewScroll;
  // header
  d.setTextDatum(top_center);
  d.setTextSize(3);
  d.setTextColor(gold, TFT_BLACK);
  d.drawString("THE TIERED CREW", 160, cy);
  cy += 34;
  cy += drawWrappedCentered(g_crewSubtitle, 160, cy, 300, 2, TFT_LIGHTGREY, 19) + 8;
  d.setTextDatum(middle_center);
  // OWNER -> LEAD -> ADVISOR, then the pair, then fork + footer
  cy += drawSeatBox(seatByKey(0), cy, gold) + 6;
  d.setTextSize(2);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setTextDatum(top_center);
  if (cy >= -20 && cy <= 240) d.drawString("LINE OF COMMAND", 160, cy);
  d.setTextDatum(middle_center);
  cy += 22;
  cy += drawSeatBox(seatByKey(1), cy, pink) + 10;
  cy += drawSeatBox(seatByKey(2), cy, TFT_LIGHTGREY) + 10;
  int hSpec = drawHalfSeatBox(seatByKey(3), 10, cy, green);
  int hSwarm = drawHalfSeatBox(seatByKey(4), 164, cy, blue);
  cy += (hSpec > hSwarm ? hSpec : hSwarm) + 10;
  // fork layer pill
  int forkH = wrappedLines(g_crewFork, 272, 2) * 19 + 14;
  if (cy + forkH >= 0 && cy <= 240) {
    d.fillRoundRect(10, cy, 300, forkH, 12, d.color565(16, 40, 26));
    d.drawRoundRect(10, cy, 300, forkH, 12, green);
    drawWrappedCentered(g_crewFork, 160, cy + 8, 272, 2, green, 19,
                        d.color565(16, 40, 26));
  }
  cy += forkH + 10;
  cy += drawWrappedCentered(g_crewFooter, 160, cy, 300, 2, TFT_WHITE, 19) + 14;
  g_crewContentH = cy + g_crewScroll;
  drawCrewScrollHint();
  drawNavArrows();
}

static void setCrewScroll(int y, bool verbose) {
  int maxScroll = g_crewContentH - 240;
  if (maxScroll < 0) maxScroll = 0;
  if (y < 0) y = 0;
  if (y > maxScroll) y = maxScroll;
  if (y != g_crewScroll) {
    g_crewScroll = y;
    if (g_screen == SCR_CREW) drawCrew();
    if (verbose) Serial.printf("CREW_SCROLL y=%d\n", g_crewScroll);
  }
}

// Drag state for the CREW screen: g_dragY is the touch Y where the drag
// started (-1 = no drag), g_dragStartScroll the scroll offset at that time.
static int g_dragY = -1;
static int g_dragStartScroll = 0;

// ------------------------------------------------- screen 5: COUNTDOWN
// Temporary by owner order: big live countdown to the Real or Replica
// submission deadline. After the deadline it shows a done state; 24h
// after the deadline screenVisible() retires it from the rotation.
static long countdownRemaining() {
  time_t now = time(nullptr);
  if (now < 1600000000L) return -1;  // clock not set yet
  return (long)(CD_TARGET_EPOCH - now);
}

static int g_cdLastSec = -1;
static uint32_t g_cdLastLog = 0;

// Page 2 of this screen: WHAT'S LEFT TO DO for the Meta submission.
// The list comes from the bridge (GET /hackathons, served from
// hackathons.json on Allie) so it can change without a reflash. The
// board reads the FIRST event's todos -- today that is Meta's; the
// per-hackathon screens are the v1.3 family and will key by event.
static int g_cdPage = 0;  // 0 = clock, 1 = what's-left list
static char g_todos[10][52];
static int g_todoCount = 0;
static bool g_hackOk = false;
static int g_cdScroll = 0;
static int g_cdContentH = 0;
static int g_cdDragY = -1;
static int g_cdDragStartScroll = 0;

// Fetch + parse into scratch, commit only on success (v1.21 cache rule).
// Returns 1 = new data committed, 0 = identical, -1 = failed.
static int fetchHackathons() {
  String json;
  if (!fetchBridgeJson("/hackathons", json)) return -1;
  int tpos = json.indexOf("\"todos\"");
  if (tpos < 0) { Serial.println("HACK_FETCH ok=0 reason=no_todos_key"); return -1; }
  int aStart = json.indexOf('[', tpos);
  int aEnd = json.indexOf(']', tpos);
  if (aStart < 0 || aEnd < 0) { Serial.println("HACK_FETCH ok=0 reason=no_array"); return -1; }
  char tmp[10][52];
  int pos = aStart;
  int count = 0;
  while (count < 10) {
    int q1 = json.indexOf('"', pos);
    if (q1 < 0 || q1 >= aEnd) break;
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 < 0 || q2 > aEnd) break;
    size_t n = (size_t)(q2 - q1 - 1);
    if (n >= sizeof(tmp[0])) n = sizeof(tmp[0]) - 1;
    memcpy(tmp[count], json.c_str() + q1 + 1, n);
    tmp[count][n] = 0;
    count++;
    pos = q2 + 1;
  }
  if (count == 0) { Serial.println("HACK_FETCH ok=0 todos=0"); return -1; }
  bool changed = !g_hackOk || count != g_todoCount;
  for (int i = 0; !changed && i < count; i++)
    if (strcmp(tmp[i], g_todos[i])) changed = true;
  Serial.printf("HACK_FETCH ok=1 todos=%d changed=%d\n", count, changed ? 1 : 0);
  if (!changed) return 0;
  memcpy(g_todos, tmp, sizeof(g_todos));
  g_todoCount = count;
  g_hackOk = true;
  return 1;
}

static void setCdScroll(int y, bool verbose) {
  int maxScroll = g_cdContentH - 196;
  if (maxScroll < 0) maxScroll = 0;
  if (y < 0) y = 0;
  if (y > maxScroll) y = maxScroll;
  if (y != g_cdScroll) {
    g_cdScroll = y;
    if (g_screen == SCR_COUNTDOWN && g_cdPage == 1) drawCountdown();
    if (verbose) Serial.printf("CD_SCROLL y=%d\n", g_cdScroll);
  }
}

static void drawCountdownTodos() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_center);
  d.setTextSize(3);
  d.setTextColor(TFT_GOLD, TFT_BLACK);
  d.drawString("WHAT'S LEFT TO DO", 160, 8);
  d.setTextDatum(middle_center);
  if (!g_hackOk) {
    d.setTextColor(TFT_ORANGE, TFT_BLACK);
    d.setTextSize(3);
    d.drawString("Can't reach Allie", d.width() / 2, 120);
  } else {
    int cy = 48 - g_cdScroll;
    for (int i = 0; i < g_todoCount; i++) {
      int itemH = wrappedLines(g_todos[i], 258, 2) * 19 + 12;
      if (cy + itemH >= 44 && cy <= 196) {
        d.drawRect(16, cy + 3, 12, 12, TFT_GOLD);
        drawWrappedLeft(g_todos[i], 38, cy, 258, 2, TFT_WHITE, 19);
      }
      cy += itemH;
    }
    g_cdContentH = cy + g_cdScroll;
  }
  // fixed BACK strip at the bottom (same rect the clock page's button uses)
  d.fillRect(0, 196, 320, 44, TFT_BLACK);
  d.fillRoundRect(58, 200, 204, 38, 10, TFT_BLUE);
  d.setTextColor(TFT_WHITE, TFT_BLUE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("BACK", d.width() / 2, 219);
  drawNavArrows();
}

static char g_cdDaysStr[24] = "";   // last days line painted on the clock page
static char g_cdClockStr[24] = "";  // last clock line painted on the clock page

static void drawCountdown() {
  auto &d = M5.Display;
  if (g_cdPage == 1) { drawCountdownTodos(); return; }
  d.fillScreen(TFT_BLACK);
  drawScreenTitle("COUNTDOWN");
  d.setTextDatum(middle_center);
  long rem = countdownRemaining();
  if (rem < 0 && time(nullptr) < 1600000000L) {
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setTextSize(3);
    d.drawString("Setting clock...", d.width() / 2, 130);
    g_cdDaysStr[0] = 0;
    g_cdClockStr[0] = 0;
    drawNavArrows();
    return;
  }
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setTextSize(2);
  d.drawString("Real or Replica - submission deadline", d.width() / 2, 56);
  if (rem <= 0) {
    d.setTextColor(TFT_GOLD, TFT_BLACK);
    d.setTextSize(6);
    d.drawString("DONE", d.width() / 2, 118);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(3);
    d.drawString("The deadline has passed", d.width() / 2, 172);
    g_cdDaysStr[0] = 0;
    g_cdClockStr[0] = 0;
    drawNavArrows();
    return;
  }
  long days = rem / 86400;
  long hrs = (rem % 86400) / 3600;
  long min = (rem % 3600) / 60;
  long sec = rem % 60;
  char buf[24];
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(7);
  snprintf(buf, sizeof(buf), "%ld DAYS", days);
  d.drawString(buf, d.width() / 2, 104);
  snprintf(g_cdDaysStr, sizeof(g_cdDaysStr), "%s", buf);
  d.setTextSize(6);
  snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", hrs, min, sec);
  d.drawString(buf, d.width() / 2, 156);
  snprintf(g_cdClockStr, sizeof(g_cdClockStr), "%s", buf);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setTextSize(2);
  d.drawString("HRS      MIN      SEC", d.width() / 2, 190);
  // big button to the what's-left page
  d.fillRoundRect(58, 202, 204, 36, 10, TFT_BLUE);
  d.setTextColor(TFT_WHITE, TFT_BLUE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("WHAT'S LEFT", d.width() / 2, 220);
  g_cdFull++;
  drawNavArrows();
}

// Per-second partial update of the clock page (v1.21 flicker fix): only
// the two changing text lines are repainted -- the old strings are
// overdrawn in black and the new ones drawn at the same spots. The full
// screen is painted on entry, on page switches, and on state changes
// (clock unset / DONE) only. Proof: STATS cd_full vs cd_tick.
static void tickCountdown() {
  long rem = countdownRemaining();
  if (rem <= 0 || g_cdClockStr[0] == 0) { drawCountdown(); return; }
  long days = rem / 86400;
  long hrs = (rem % 86400) / 3600;
  long min = (rem % 3600) / 60;
  long sec = rem % 60;
  char dbuf[24], cbuf[24];
  snprintf(dbuf, sizeof(dbuf), "%ld DAYS", days);
  snprintf(cbuf, sizeof(cbuf), "%02ld:%02ld:%02ld", hrs, min, sec);
  if (!strcmp(dbuf, g_cdDaysStr) && !strcmp(cbuf, g_cdClockStr)) return;
  auto &d = M5.Display;
  d.setTextDatum(middle_center);
  if (strcmp(dbuf, g_cdDaysStr)) {
    d.setTextSize(7);
    d.setTextColor(TFT_BLACK, TFT_BLACK);
    d.drawString(g_cdDaysStr, d.width() / 2, 104);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(dbuf, d.width() / 2, 104);
    snprintf(g_cdDaysStr, sizeof(g_cdDaysStr), "%s", dbuf);
  }
  d.setTextSize(6);
  d.setTextColor(TFT_BLACK, TFT_BLACK);
  d.drawString(g_cdClockStr, d.width() / 2, 156);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(cbuf, d.width() / 2, 156);
  snprintf(g_cdClockStr, sizeof(g_cdClockStr), "%s", cbuf);
  g_cdTick++;
}

// ------------------------------------------------- screen 6: MACHINES
// The chain, checked live by the bridge (GET /machines): Allie answers for
// herself, Penguin is genuinely probed, G1 is static until he lands.
// Dots: green = checked ok, yellow = in transit, grey = not checked,
// red = checked and down. (struct MachineRow itself is defined at the top.)
static MachineRow g_machines[3];
static int g_machineCount = 0;
static bool g_machinesOk = false;

// Fetch + parse into scratch, commit only on success (v1.21 cache rule).
// On commit, g_machChangedMask marks which rows differ so the refresh
// path repaints only those rows. Returns 1 = new data, 0 = identical,
// -1 = failed.
static uint8_t g_machChangedMask = 0;

static int fetchMachines() {
  String json;
  if (!fetchBridgeJson("/machines", json)) return -1;
  MachineRow tmp[3];
  int pos = 0;
  int count = 0;
  char buf[64];
  while (count < 3) {
    MachineRow &m = tmp[count];
    int p = jsonStr(json, "name", pos, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(m.name, sizeof(m.name), "%s", buf);
    p = jsonStr(json, "state", p, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(m.state, sizeof(m.state), "%s", buf);
    p = jsonStr(json, "line", p, buf, sizeof(buf));
    if (p < 0) break;
    snprintf(m.line, sizeof(m.line), "%s", buf);
    pos = p;
    count++;
  }
  if (count != 3) {
    Serial.printf("MACHINES_FETCH ok=0 rows=%d\n", count);
    return -1;
  }
  uint8_t mask = 0;
  if (!g_machinesOk) mask = 0x7;
  else {
    for (int i = 0; i < 3; i++) {
      if (strcmp(tmp[i].name, g_machines[i].name) || strcmp(tmp[i].state, g_machines[i].state)
          || strcmp(tmp[i].line, g_machines[i].line))
        mask |= (1 << i);
    }
  }
  Serial.printf("MACHINES_FETCH ok=1 g1=%s penguin=%s allie=%s changed_mask=%u\n",
                tmp[0].state, tmp[1].state, tmp[2].state, mask);
  if (!mask) return 0;
  memcpy(g_machines, tmp, sizeof(tmp));
  g_machineCount = 3;
  g_machinesOk = true;
  g_machChangedMask = mask;
  return 1;
}

static uint32_t machineDotColor(const char *state) {
  if (!strcmp(state, "ok")) return TFT_GREEN;
  if (!strcmp(state, "transit")) return TFT_YELLOW;
  if (!strcmp(state, "down")) return TFT_RED;
  return TFT_DARKGREY;  // unknown / not checked
}

// One machine row, painted in its own band (rows sit at 52 + i*46 and
// never reach the REFRESH button at y=192). Used by the full draw and
// by refresh partials, which repaint changed rows only.
static void drawMachineRow(int i) {
  auto &d = M5.Display;
  int yTop = 52 + i * 46;
  d.fillRect(8, yTop, 304, 42, TFT_BLACK);
  char rank[4];
  snprintf(rank, sizeof(rank), "#%d", i + 1);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.setTextColor(TFT_GOLD, TFT_BLACK);
  d.drawString(rank, 14, yTop);
  d.fillCircle(56, yTop + 11, 9, machineDotColor(g_machines[i].state));
  d.setTextSize(2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(g_machines[i].name, 74, yTop);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(g_machines[i].line, 74, yTop + 21);
  d.setTextDatum(middle_center);
}

static void drawMachines() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  drawScreenTitle("MACHINES");
  if (!g_machinesOk) {
    d.setTextColor(TFT_ORANGE, TFT_BLACK);
    d.setTextDatum(middle_center);
    d.setTextSize(3);
    d.drawString("Can't reach Allie", d.width() / 2, 120);
  } else {
    for (int i = 0; i < g_machineCount; i++) drawMachineRow(i);
  }
  // big REFRESH button (between the edge nav zones)
  d.fillRoundRect(58, 192, 204, 40, 10, TFT_BLUE);
  d.setTextColor(TFT_WHITE, TFT_BLUE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("REFRESH", d.width() / 2, 212);
  drawNavArrows();
}

// ---------------------------------------------------------------- navigation
// v1.21: a switch is render-only. Bridge-fed screens paint instantly
// from last-known data and flag a background refresh; loop() fetches
// later and repaints only what changed. NAV_DONE reports the switch
// cost in ms so the serial log proves no hop waits on the network.
static void gotoScreen(int s) {
  if (s == g_screen) return;
  if (!screenVisible(s)) {
    Serial.printf("GOTO_SKIP idx=%d name=%s (retired)\n", s, kScreenNames[s]);
    return;
  }
  uint32_t navT0 = millis();
  g_screen = s;
  g_dragY = -1;
  g_cdDragY = -1;
  Serial.printf("SCREEN idx=%d name=%s\n", g_screen, kScreenNames[g_screen]);
  switch (g_screen) {
    case SCR_STATUS: drawStatus(); break;
    case SCR_TALK:   drawTalk();   break;
    case SCR_TIME:   g_lastShownSec = -1; drawTime(); break;
    case SCR_CREW:
      g_crewScroll = 0;
      drawCrew();
      g_pendCrew = true;
      Serial.printf("CREW_SCROLL max=%d\n", g_crewContentH > 240 ? g_crewContentH - 240 : 0);
      break;
    case SCR_COUNTDOWN:
      g_cdLastSec = -1;
      g_cdPage = 0;
      g_cdScroll = 0;
      drawCountdown();
      g_pendHack = true;
      Serial.printf("CD_REMAIN sec=%ld\n", countdownRemaining());
      break;
    case SCR_MACHINES:
      drawMachines();
      g_pendMachines = true;
      break;
    default: break;
  }
  g_lastNavMs = millis() - navT0;
  g_navCount++;
  Serial.printf("NAV_DONE idx=%d name=%s ms=%lu\n", g_screen, kScreenNames[g_screen],
                (unsigned long)g_lastNavMs);
}

static void handleTap(int x, int y) {
  auto &d = M5.Display;
  if (x < 48) { gotoScreen(navStep(g_screen, -1)); return; }
  if (x > d.width() - 48) { gotoScreen(navStep(g_screen, 1)); return; }
  if (g_screen == SCR_MACHINES && x >= 58 && x <= 262 && y >= 192 && y <= 232) {
    Serial.println("MACHINES_REFRESH tap");
    g_pendMachines = true;  // loop() fetches; changed rows repaint in place
    return;
  }
  if (g_screen == SCR_COUNTDOWN && x >= 58 && x <= 262 && y >= 196 && y <= 240) {
    g_cdPage = g_cdPage == 0 ? 1 : 0;
    g_cdScroll = 0;
    Serial.printf("CD_PAGE page=%d (tap)\n", g_cdPage);
    drawCountdown();
    return;
  }
  if (g_screen == SCR_TALK) {
    Serial.println("TALK_TAP -- voice exchange");
    voiceExchange(VOICE_TALK_CLIP, false);
    g_talkOk = g_lastPressOk;
    g_talkResult = true;
    g_talkResultUntil = millis() + 2500;
    drawTalk();
  }
}

// ------------------------------------------------- serial calibration commands
// MICG <n>   -- set mic magnification (0..255) live
// MICCHK     -- 3s ambient measurement at the current magnification:
//               MICCHK mag=<n> peak=<p> rms=<r> rail=<hits> samples=<n>
static String g_serLine;

static void micMeasure() {
  // The MIC screen is gone (v1.21) but this calibration harness stays:
  // it measures through the same ES7210 path the ears build will use.
  applyMicConfig();
  M5.Mic.begin();
  static int16_t buf[1024];
  int32_t peak = 0;
  int64_t sumsq = 0, total = 0, rail = 0;
  bool rec = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    if (!rec) rec = M5.Mic.record(buf, 1024, 16000);
    if (rec && !M5.Mic.isRecording()) {
      rec = false;
      for (int i = 0; i < 1024; i++) {
        int v = buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
        if (v >= 32700) rail++;
        sumsq += (int64_t)buf[i] * buf[i];
        total++;
      }
    }
    delay(5);
  }
  M5.Mic.end();
  long rms = total ? (long)sqrt((double)sumsq / (double)total) : 0;
  Serial.printf("MICCHK mag=%u peak=%ld rms=%ld rail=%lld samples=%lld\n",
                g_micMag, (long)peak, rms, (long long)rail, (long long)total);
}

static void handleSerialCommand(const String &line) {
  if (line.startsWith("GOTO ")) {
    int n = line.substring(5).toInt();
    if (n >= 0 && n < SCR_COUNT) gotoScreen(n);
    else Serial.printf("GOTO_BAD idx=%d\n", n);
  } else if (line.startsWith("SCROLLTO ")) {
    setCrewScroll(line.substring(9).toInt(), true);
  } else if (line.startsWith("CDPAGE ")) {
    g_cdPage = line.substring(7).toInt() ? 1 : 0;
    g_cdScroll = 0;
    Serial.printf("CD_PAGE page=%d\n", g_cdPage);
    if (g_screen == SCR_COUNTDOWN) drawCountdown();
  } else if (line.startsWith("MICG ")) {
    int n = line.substring(5).toInt();
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    g_micMag = (uint8_t)n;
    Serial.printf("MIC_GAIN set=%u\n", g_micMag);
  } else if (line == "MICCHK") {
    micMeasure();
  } else if (line == "STATS") {
    Serial.printf("STATS nav=%d last_nav_ms=%lu cd_full=%d cd_tick=%d time_full=%d time_tick=%d crew_repaint=%d crew_skip=%d mach_row_paint=%d mach_skip=%d\n",
                  g_navCount, (unsigned long)g_lastNavMs, g_cdFull, g_cdTick,
                  g_timeFull, g_timeTick, g_crewRepaint, g_crewSkip,
                  g_machRowPaint, g_machSkip);
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(255);  // owner order: speaker at maximum
  Serial.begin(115200);
  delay(200);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(220);
  Serial.println();
  Serial.println("POTION v1.21 boot");
  drawJoining();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("potion-cores3");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("Wi-Fi join started");
  configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.nist.gov");
  Serial.println("SCREEN idx=0 name=STATUS");
}

void loop() {
  M5.update();
  uint32_t now = millis();

  // serial calibration commands (MICG / MICCHK)
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_serLine.length()) { handleSerialCommand(g_serLine); g_serLine = ""; }
    } else {
      g_serLine += c;
    }
  }

  // touch: edge zones navigate, center taps act on the screen
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) handleTap(t.x, t.y);
  }

  // CREW screen: touch-drag scrolling (center area; edges still navigate)
  if (g_screen == SCR_CREW && !g_faceUp) {
    if (M5.Touch.getCount() > 0) {
      auto t = M5.Touch.getDetail();
      if (t.wasPressed() && t.x >= 48 && t.x <= 272) {
        g_dragY = t.y;
        g_dragStartScroll = g_crewScroll;
      } else if (g_dragY >= 0 && !t.wasPressed()) {
        setCrewScroll(g_dragStartScroll + (g_dragY - t.y), false);
      }
    } else if (g_dragY >= 0) {
      if (g_crewScroll != g_dragStartScroll)
        Serial.printf("CREW_SCROLL y=%d (drag)\n", g_crewScroll);
      g_dragY = -1;
    }
  }

  // Wi-Fi state, v0 cadence
  if (now - g_lastCheck >= 500) {
    g_lastCheck = now;
    bool conn = (WiFi.status() == WL_CONNECTED);
    if (conn && !g_shownConnected) {
      g_shownConnected = true;
      if (g_screen == SCR_STATUS) drawConnected();
      Serial.printf("WIFI_CONNECTED ip=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      if (!g_bootTested) {
        g_bootTested = true;
        voiceExchange(VOICE_HELLO_CLIP, true);  // boot hello: /hello event + route obeyed, once per boot
      }
    } else if (!conn && g_shownConnected) {
      g_shownConnected = false;
      if (g_screen == SCR_STATUS) drawJoining();
      Serial.println("WIFI_LOST -- status screen back to joining");
    } else if (!conn && g_screen == SCR_STATUS) {
      g_dots = (g_dots + 1) % 4;
      drawJoiningDots();
    }
  }

  // per-screen periodic work
  if (g_screen == SCR_TIME) {
    struct tm tmv;
    if (getLocalTime(&tmv, 5) && tmv.tm_sec != g_lastShownSec) {
      g_lastShownSec = tmv.tm_sec;
      tickTime();
    }
  }
  if (g_screen == SCR_COUNTDOWN) {
    if (countdownRetired()) {
      Serial.println("COUNTDOWN retired -- leaving the screen");
      gotoScreen(SCR_STATUS);
    } else if (g_cdPage == 0) {
      long rem = countdownRemaining();
      int sec = rem >= 0 ? (int)(rem % 60) : -1;
      if (rem >= 0 && sec != g_cdLastSec) {
        g_cdLastSec = sec;
        tickCountdown();
      }
      if (now - g_cdLastLog >= 5000) {
        g_cdLastLog = now;
        Serial.printf("CD_REMAIN sec=%ld\n", rem);
      }
    }
  }

  // COUNTDOWN page 2 (what's-left list): same drag-scroll pattern as CREW
  if (g_screen == SCR_COUNTDOWN && g_cdPage == 1 && !g_faceUp) {
    if (M5.Touch.getCount() > 0) {
      auto t = M5.Touch.getDetail();
      if (t.wasPressed() && t.x >= 48 && t.x <= 272 && t.y < 196) {
        g_cdDragY = t.y;
        g_cdDragStartScroll = g_cdScroll;
      } else if (g_cdDragY >= 0 && !t.wasPressed()) {
        setCdScroll(g_cdDragStartScroll + (g_cdDragY - t.y), false);
      }
    } else if (g_cdDragY >= 0) {
      if (g_cdScroll != g_cdDragStartScroll)
        Serial.printf("CD_SCROLL y=%d (drag)\n", g_cdScroll);
      g_cdDragY = -1;
    }
  }
  if (g_talkResult && (int32_t)(now - g_talkResultUntil) >= 0) {
    g_talkResult = false;
    if (g_screen == SCR_TALK) drawTalk();
  }

  // ------------------------------------------------- v1.21 deferred refresh
  // Fetches run HERE, never inside a screen switch. Fresh data repaints
  // only what changed; identical data repaints nothing; a failed fetch
  // keeps the last-known content on screen.
  if (g_pendCrew) {
    g_pendCrew = false;
    int r = fetchCrew();
    if (r == 0) {
      g_crewSkip++;
    } else if (r == 1 && g_screen == SCR_CREW) {
      g_crewScroll = 0;
      drawCrew();
      g_crewRepaint++;
      Serial.printf("CREW_SCROLL max=%d (refresh)\n", g_crewContentH > 240 ? g_crewContentH - 240 : 0);
    }
  }
  if (g_pendHack) {
    g_pendHack = false;
    int r = fetchHackathons();
    if (r == 1 && g_screen == SCR_COUNTDOWN && g_cdPage == 1) drawCountdown();
  }
  if (g_pendMachines) {
    g_pendMachines = false;
    bool hadData = g_machinesOk;
    int r = fetchMachines();
    if (r == 0) {
      g_machSkip++;
    } else if (r == 1 && g_screen == SCR_MACHINES) {
      if (!hadData) {
        drawMachines();  // first data: replace the "Can't reach Allie" state
        g_machRowPaint += 3;
      } else {
        for (int i = 0; i < 3; i++) {
          if (g_machChangedMask & (1 << i)) { drawMachineRow(i); g_machRowPaint++; }
        }
      }
    }
  }

  // 5s heartbeat (v0 line + screen field)
  if (now - g_lastBeat >= 5000) {
    g_lastBeat = now;
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("POTION_STATUS connected=1 ip=%s rssi=%d uptime_s=%lu screen=%s\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
                    (unsigned long)(now / 1000), kScreenNames[g_screen]);
    } else {
      Serial.printf("POTION_STATUS connected=0 status=%d uptime_s=%lu screen=%s\n",
                    (int)WiFi.status(), (unsigned long)(now / 1000), kScreenNames[g_screen]);
    }
  }
  delay(10);
}

