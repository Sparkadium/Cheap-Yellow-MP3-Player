// ============================================================
// ShuffleCYD v2 — MP3 Player for ESP32-2432S028 (CYD)
// ============================================================
// Features: BT audio, spectrum visualizer, progress bar,
//           playlist browser, near-gapless playback
//
// Libraries: ESP8266Audio, ESP32-A2DP, TFT_eSPI, XPT2046
// Partition: "Huge APP (3MB No OTA)"
// ============================================================

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>

#include "BluetoothA2DPSource.h"

// ── Hardware pins ───────────────────────────────────────────
#define SD_CS     5
#define TOUCH_CS  33
#define TOUCH_IRQ 36
#define TFT_BL    21
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

SPIClass touchSPI(HSPI);

// ── BOOT button (GPIO 0) — press to lock/unlock screen ─────
#define BOOT_BTN 0
static bool screenLocked = false;
static unsigned long lastBootPress = 0;

// ── Config ──────────────────────────────────────────────────
static const char* BT_SPEAKER_NAME = "YOUR SPEAKER NAME HERE";

#define TS_MINX 300
#define TS_MAXX 3900
#define TS_MINY 300
#define TS_MAXY 3900

// ── Ring buffer: 4K frames = 16KB ───────────────────────────
#define RING_SIZE 4096
static int16_t ring[RING_SIZE * 2];
static volatile size_t rbHead = 0;
static volatile size_t rbTail = 0;

static inline size_t rbAvail() {
  size_t h = rbHead, t = rbTail;
  return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}

// ── Visualizer sample capture ───────────────────────────────
#define VIZ_SAMPLES 64
static int16_t vizBuf[VIZ_SAMPLES];
static volatile int vizPos = 0;
static float vizBars[8] = {0};
static float vizPeak[8] = {0};

// ── Ring buffer AudioOutput ─────────────────────────────────
class RingBufOutput : public AudioOutput {
public:
  float _gain = 0.7f;

  virtual bool begin() { return true; }
  virtual bool stop()  { return true; }
  virtual bool ConsumeSample(int16_t sample[2]) {
    size_t next = (rbHead + 1) % RING_SIZE;
    if (next == rbTail) return false;

    int32_t l = (int32_t)(sample[0] * _gain);
    int32_t r = (int32_t)(sample[1] * _gain);
    ring[rbHead * 2]     = constrain(l, -32768, 32767);
    ring[rbHead * 2 + 1] = constrain(r, -32768, 32767);
    rbHead = next;

    // Capture mono mix for visualizer
    int16_t mono = (sample[0] / 2) + (sample[1] / 2);
    vizBuf[vizPos] = mono;
    vizPos = (vizPos + 1) % VIZ_SAMPLES;

    return true;
  }

  void setGain(float g) { _gain = g; }
};

// ── Playlist ────────────────────────────────────────────────
#define MAX_TRACKS 128
static char* playlist[MAX_TRACKS];
static int   trackCount   = 0;
static int   currentTrack = 0;
static int   shuffleOrder[MAX_TRACKS];

// Shuffle: OFF = sequential, TRACKS = random all, ALBUM = random within folder
enum ShuffleMode { SHUFFLE_OFF, SHUFFLE_TRACKS, SHUFFLE_ALBUM };
static ShuffleMode shuffleMode = SHUFFLE_TRACKS; // default ON like before

// Color inversion — set true if your CYD shows inverted/negative colors
static const bool INVERT_DISPLAY = false;

// ── Audio engine ────────────────────────────────────────────
RingBufOutput     *audioOut  = nullptr;
AudioFileSourceSD *audioFile = nullptr;
AudioGeneratorMP3 *mp3       = nullptr;
AudioGeneratorWAV *wav       = nullptr;

enum AudioType { AUDIO_NONE, AUDIO_MP3, AUDIO_WAV };
AudioType currentType = AUDIO_NONE;

// ── Player state ────────────────────────────────────────────
enum PlayerState { STATE_STOPPED, STATE_PLAYING, STATE_PAUSED };
PlayerState playerState = STATE_STOPPED;

// ── Bluetooth (declared early — needed by volume and LED functions below) ──
BluetoothA2DPSource a2dp;

// ── Volume (percentage-based, controls both software gain and A2DP HW vol) ──
static int volumePercent = 70;
static void applyVolumePercent() {
  volumePercent = constrain(volumePercent, 0, 100);
  if (audioOut) audioOut->setGain((float)volumePercent / 100.0f);
  a2dp.set_volume((int)((volumePercent * 127) / 100));
}

// ── RGB LED (CYD back LED: R=4, G=16, B=17, active LOW) ────
#define RGB_LED_RED   4
#define RGB_LED_GREEN 16
#define RGB_LED_BLUE  17
static unsigned long rgbLastMs = 0;
static bool rgbBlinkPhase = false;
static bool rgbPrevBt = false;
static bool rgbPrevPlaying = false;

static void rgbLedInit() {
  pinMode(RGB_LED_RED, OUTPUT);
  pinMode(RGB_LED_GREEN, OUTPUT);
  pinMode(RGB_LED_BLUE, OUTPUT);
  digitalWrite(RGB_LED_RED, HIGH);
  digitalWrite(RGB_LED_GREEN, HIGH);
  digitalWrite(RGB_LED_BLUE, HIGH);
}

static void rgbLedUpdate() {
  unsigned long now = millis();
  bool bt = a2dp.is_connected();
  bool playing = (playerState == STATE_PLAYING);
  if (bt != rgbPrevBt || playing != rgbPrevPlaying) {
    rgbPrevBt = bt; rgbPrevPlaying = playing;
    rgbLastMs = now; rgbBlinkPhase = false;
    digitalWrite(RGB_LED_RED, HIGH);
    digitalWrite(RGB_LED_GREEN, HIGH);
    digitalWrite(RGB_LED_BLUE, HIGH);
  }
  if (bt && playing) {
    if (now - rgbLastMs >= 450) { rgbLastMs = now; rgbBlinkPhase = !rgbBlinkPhase; }
    digitalWrite(RGB_LED_RED, HIGH);
    digitalWrite(RGB_LED_GREEN, rgbBlinkPhase ? LOW : HIGH);
    digitalWrite(RGB_LED_BLUE, rgbBlinkPhase ? HIGH : LOW);
  } else if (!bt) {
    if (now - rgbLastMs >= 280) { rgbLastMs = now; rgbBlinkPhase = !rgbBlinkPhase; }
    digitalWrite(RGB_LED_GREEN, HIGH);
    digitalWrite(RGB_LED_RED, rgbBlinkPhase ? LOW : HIGH);
    digitalWrite(RGB_LED_BLUE, rgbBlinkPhase ? HIGH : LOW);
  }
}

// ── Playback timeline (wall-clock based, survives pause/resume) ──
static unsigned long trackWallStartMs   = 0;
static unsigned long accumulatedPauseMs = 0;
static unsigned long pauseBeganMs       = 0;
static uint32_t      cachedDurationSec  = 0;
static uint32_t      cachedWavRateHz    = 0;

static uint32_t elapsedPlaybackSec() {
  if (playerState == STATE_STOPPED || trackWallStartMs == 0) return 0;
  if (playerState == STATE_PAUSED && pauseBeganMs >= trackWallStartMs)
    return min(cachedDurationSec, (uint32_t)((pauseBeganMs - trackWallStartMs - accumulatedPauseMs) / 1000ul));
  if (playerState == STATE_PLAYING)
    return min(cachedDurationSec, (uint32_t)((millis() - trackWallStartMs - accumulatedPauseMs) / 1000ul));
  return 0;
}

static void formatTimeHMS(char* buf, size_t n, uint32_t sec) {
  uint32_t m = sec / 60u;
  uint32_t s = sec % 60u;
  snprintf(buf, n, "%d:%02d", (int)m, (int)s);
}

/** Pump the decoder during blocking operations to prevent BT audio underruns. */
static void audioPumpPlayingMax(int maxLoops) {
  if (playerState != STATE_PLAYING) return;
  const int target = (RING_SIZE * 3) / 4;
  int loops = 0;
  while ((int)rbAvail() < target && loops < maxLoops) {
    bool ok = false;
    if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) ok = mp3->loop();
    else if (currentType == AUDIO_WAV && wav && wav->isRunning()) ok = wav->loop();
    if (!ok) break;
    loops++;
  }
}

// ── Screen state ────────────────────────────────────────────
enum ScreenMode { SCREEN_MAIN, SCREEN_BROWSE };
ScreenMode screenMode = SCREEN_MAIN;
int browseScroll = 0;
int browseItemH = 24;
int browseVisible = 7;

// Two-level browser: albums → tracks
enum BrowseLevel { BROWSE_ALBUMS, BROWSE_TRACKS };
BrowseLevel browseLevel = BROWSE_ALBUMS;

#define MAX_ALBUMS 32
#define MAX_ALBUM_NAME 48
static char albumNames[MAX_ALBUMS][MAX_ALBUM_NAME];
static int albumCount = 0;
static int browseAlbumIdx = -1; // which album we're viewing tracks for
static char browseAlbumPath[64]; // e.g. "/Music/AlbumName"

// Tracks within the currently browsed album
#define MAX_BROWSE_TRACKS 64
static int browseTrackIndices[MAX_BROWSE_TRACKS]; // indices into playlist[]
static int browseTrackCount = 0;

// ── Bookmarks (audiobook resume) ────────────────────────────
// Saved to /bookmarks.dat on SD card. One entry per album.
// Format per line: albumName|trackIndex|byteOffset|totalTracks
struct Bookmark {
  char albumName[MAX_ALBUM_NAME];
  int trackIdx;       // track index within album
  uint32_t bytePos;   // byte offset in the file
  int totalTracks;     // total tracks in album (for progress display)
  bool completed;      // true if all tracks finished
};

#define MAX_BOOKMARKS 32
static Bookmark bookmarks[MAX_BOOKMARKS];
static int bookmarkCount = 0;
static unsigned long lastBookmarkSaveMs = 0;
static const char* BOOKMARK_FILE = "/bookmarks.dat";

// Which album is currently playing (for bookmark tracking)
static char playingAlbumName[MAX_ALBUM_NAME] = {0};
static int playingAlbumTrackOffset = 0; // index of first track of this album in playlist[]

// Forward declarations for bookmark functions
void loadBookmarks();
void saveBookmarks();
Bookmark* findBookmark(const char* albumName);
void updateBookmark(const char* albumName, int trackIdx, uint32_t bytePos, int totalTracks, bool completed);
int getAlbumTrackIndex(); // current track's index within its album

// ── Bluetooth ───────────────────────────────────────────────
volatile bool btConnected = false;
unsigned long lastBTCheck = 0;
bool lastBTState = false;

int32_t btCallback(Frame *frame, int32_t count) {
  int32_t avail = (int32_t)rbAvail();
  int32_t toSend = min(count, avail);
  for (int32_t i = 0; i < toSend; i++) {
    frame[i].channel1 = ring[rbTail * 2];
    frame[i].channel2 = ring[rbTail * 2 + 1];
    rbTail = (rbTail + 1) % RING_SIZE;
  }
  for (int32_t i = toSend; i < count; i++) {
    frame[i].channel1 = 0;
    frame[i].channel2 = 0;
  }
  return count;
}

// ── Display / Touch ─────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ── Main screen layout ──
//  y:  0-16  Top bar (BT + track counter)
//  y: 18-38  Track name
//  y: 40-88  Visualizer (48px)
//  y: 90-104 Progress bar + time
//  y: 108-172 Transport buttons
//  y: 176-240 Bottom bar (SHF, Browse, Vol)

#define TOP_H       17
#define TITLE_Y     18
#define TITLE_H     22
#define VIZ_Y       42
#define VIZ_H       46
#define PROG_Y      90
#define PROG_H      16
#define BTN_Y       108
#define BTN_H       64
#define BTN_PREV_X  0
#define BTN_PREV_W  80
#define BTN_PLAY_X  80
#define BTN_PLAY_W  160
#define BTN_NEXT_X  240
#define BTN_NEXT_W  80
#define BAR_Y       176

// Colors
#define COL_BG       TFT_BLACK
#define COL_TEXT     TFT_WHITE
#define COL_DIM      tft.color565(100, 100, 100)
#define COL_ACCENT   tft.color565(50, 205, 50)
#define COL_BT_ON    tft.color565(80, 140, 255)
#define COL_BT_OFF   tft.color565(200, 60, 60)
#define COL_BTN      tft.color565(30, 30, 30)
#define COL_BTN_ACT  tft.color565(50, 50, 50)
#define COL_VOL_BG   tft.color565(40, 40, 40)
#define COL_VOL_FG   tft.color565(80, 160, 240)
#define COL_PROG_BG  tft.color565(40, 40, 40)
#define COL_PROG_FG  tft.color565(50, 205, 50)
#define COL_BROWSE_BG   tft.color565(20, 20, 20)
#define COL_BROWSE_SEL  tft.color565(40, 60, 40)
#define COL_BROWSE_HDR  tft.color565(30, 30, 50)

// Timing
unsigned long lastTouchTime = 0;
#define TOUCH_DEBOUNCE 250
unsigned long lastVizDraw = 0;
unsigned long lastProgDraw = 0;

// Visualizer bar colors (gradient from green to magenta)
uint16_t vizColors[8];

// ── Forward declarations ────────────────────────────────────
void scanSD();
void scanAlbums();
void loadBrowseAlbumTracks(const char* albumName);
void generateShuffleOrder();
void startTrack(int index, bool gapless = false);
void stopTrack();
void nextTrack(bool gapless = false);
void prevTrack();
void togglePause();
void drawMainScreen();
void drawTopBar();
void drawTrackName();
void drawVisualizer();
void drawProgressBar();
void drawTransport();
void drawBottomBar();
void drawBrowseScreen();
void drawBrowseList();
void handleTouchMain();
void handleTouchBrowse();
void computeViz();
bool isMP3(const char* fn);
bool isWAV(const char* fn);
const char* getFilename(const char* path);
void getDisplayName(const char* path, char* out, int maxLen);
int resolveTrack(int index);
float getProgress();
void formatTime(int seconds, char* buf);

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ShuffleCYD v2 starting...");

  rgbLedInit();

  // BOOT button for screen lock
  pinMode(BOOT_BTN, INPUT_PULLUP);

  // Precompute visualizer gradient colors
  for (int i = 0; i < 8; i++) {
    int r = map(i, 0, 7, 30, 255);
    int g = map(i, 0, 7, 220, 50);
    int b = map(i, 0, 7, 80, 220);
    vizColors[i] = tft.color565(r, g, b);
  }

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Display
  tft.init();
  tft.setRotation(1);
  if (INVERT_DISPLAY) tft.invertDisplay(true);
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextSize(2);
  tft.setCursor(40, 50);
  tft.print("ShuffleCYD v2");
  tft.setTextSize(1);
  tft.setCursor(40, 80);
  tft.print("Initializing...");

  // SD
  SPI.begin();
  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED);
    tft.setCursor(40, 100);
    tft.setTextSize(2);
    tft.print("SD Failed!");
    while (1) delay(1000);
  }
  Serial.println("[OK] SD");

  // Audio output
  audioOut = new RingBufOutput();
  applyVolumePercent();

  // Scan music
  scanSD();
  if (trackCount == 0) {
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(40, 100);
    tft.setTextSize(2);
    tft.print("No music!");
    while (1) delay(1000);
  }
  Serial.printf("[OK] %d tracks\n", trackCount);
  scanAlbums();
  loadBookmarks();
  generateShuffleOrder();

  // Decode first track
  tft.setCursor(40, 100);
  tft.print("Decoding...");
  startTrack((shuffleMode == SHUFFLE_TRACKS) ? shuffleOrder[0] : 0);

  // Touch
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);
  Serial.println("[OK] Touch");

  // Bluetooth
  tft.setCursor(40, 120);
  tft.setTextSize(1);
  tft.printf("BT: %s", BT_SPEAKER_NAME);

  a2dp.set_auto_reconnect(true);
  a2dp.set_volume(127);
  a2dp.start(BT_SPEAKER_NAME, btCallback);

  unsigned long t0 = millis();
  while (!a2dp.is_connected() && millis() - t0 < 15000) {
    rgbLedUpdate();
    if (playerState == STATE_PLAYING) {
      if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) mp3->loop();
      else if (currentType == AUDIO_WAV && wav && wav->isRunning()) wav->loop();
    }
    delay(50);
  }
  btConnected = a2dp.is_connected();
  lastBTState = btConnected;
  Serial.printf("[%s] BT\n", btConnected ? "OK" : "WAITING");

  drawMainScreen();
  Serial.println("=== RUNNING ===");
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════
void loop() {
  rgbLedUpdate();

  // ── Feed decoder ──
  if (playerState == STATE_PLAYING) {
    bool decoderAlive = false;
    if (currentType == AUDIO_MP3 && mp3) decoderAlive = mp3->isRunning();
    else if (currentType == AUDIO_WAV && wav) decoderAlive = wav->isRunning();

    if (decoderAlive) {
      int loops = 0;
      bool loopFailed = false;
      while (rbAvail() < (RING_SIZE * 3 / 4) && loops < 256) {
        bool ok = false;
        if (currentType == AUDIO_MP3 && mp3) ok = mp3->loop();
        else if (currentType == AUDIO_WAV && wav) ok = wav->loop();
        if (!ok) { loopFailed = true; break; }
        loops++;
      }

      // loop() returned false = decoder finished or errored
      if (loopFailed) {
        // Explicitly stop so isRunning() goes false
        if (mp3 && mp3->isRunning()) mp3->stop();
        if (wav && wav->isRunning()) wav->stop();
        Serial.println("Decoder finished, waiting for buffer drain...");
      }
    } else {
      // Decoder is stopped — wait for ring buffer to drain then advance
      if (rbAvail() < 200) {
        // Check if this was the last track in the album — mark complete
        if (playingAlbumName[0] != '\0') {
          int aIdx = getAlbumTrackIndex();
          int aTotal = getAlbumTrackCount();
          if (aIdx >= aTotal - 1) {
            // Last track of album finished
            updateBookmark(playingAlbumName, aIdx, 0, aTotal, true);
            saveBookmarks();
            Serial.printf("Album \"%s\" completed!\n", playingAlbumName);
          }
        }
        Serial.println("Track done, gapless next");
        nextTrack(true);
      }
    }
  }

  // ── Throttled UI updates ──
  unsigned long now = millis();

  // Visualizer: ~25 FPS (skip when screen locked)
  if (!screenLocked && screenMode == SCREEN_MAIN && playerState == STATE_PLAYING &&
      now - lastVizDraw > 40) {
    lastVizDraw = now;
    computeViz();
    drawVisualizer();
  }

  // Progress bar: ~4 FPS (skip when screen locked)
  if (!screenLocked && screenMode == SCREEN_MAIN && playerState == STATE_PLAYING &&
      now - lastProgDraw > 250) {
    lastProgDraw = now;
    drawProgressBar();
  }

  // Bookmark auto-save: every 30s while playing
  if (playerState == STATE_PLAYING && now - lastBookmarkSaveMs > 30000) {
    lastBookmarkSaveMs = now;
    saveCurrentBookmark();
  }

  // BT check: every 3s
  if (now - lastBTCheck > 3000) {
    lastBTCheck = now;
    bool cur = a2dp.is_connected();
    if (cur != lastBTState) {
      btConnected = cur;
      lastBTState = cur;
      if (screenMode == SCREEN_MAIN) drawTopBar();
    }
  }

  // Touch: throttled
  static unsigned long lastUI = 0;
  if (now - lastUI < 50) return;
  lastUI = now;

  // ── BOOT button: toggle screen lock ──
  if (digitalRead(BOOT_BTN) == LOW && now - lastBootPress > 500) {
    lastBootPress = now;
    screenLocked = !screenLocked;
    if (screenLocked) {
      // Turn off backlight, skip all touch/display
      digitalWrite(TFT_BL, LOW);
      Serial.println("Screen locked");
    } else {
      // Turn backlight on, redraw
      digitalWrite(TFT_BL, HIGH);
      Serial.println("Screen unlocked");
      if (screenMode == SCREEN_MAIN) drawMainScreen();
      else drawBrowseScreen();
    }
    // Wait for button release
    while (digitalRead(BOOT_BTN) == LOW) {
      audioPumpPlayingMax(64);
      delay(10);
    }
  }

  // Skip touch when screen is locked
  if (screenLocked) return;

  if (screenMode == SCREEN_MAIN) handleTouchMain();
  else handleTouchBrowse();
}

// ═════════════════════════════════════════════════════════════
//  VISUALIZER — 8-band DFT from 64 mono samples
// ═════════════════════════════════════════════════════════════
// Frequency bands (approximate at 44100Hz, N=64):
// Bin k = k * 44100/64 = k * 689 Hz
// Band 0: bin 0-1  (0-1378 Hz)     bass
// Band 1: bin 1-2  (689-2067 Hz)   low-mid
// Band 2: bin 2-3  (1378-2756 Hz)  mid
// Band 3: bin 3-5  (2067-4134 Hz)  upper-mid
// Band 4: bin 5-7  (3445-5512 Hz)  presence
// Band 5: bin 7-10 (4823-7580 Hz)  brilliance
// Band 6: bin 10-16 (6891-11025 Hz) high
// Band 7: bin 16-28 (11025-19292 Hz) air

static const int bandStart[8] = {0, 1, 2, 3, 5, 7, 10, 16};
static const int bandEnd[8]   = {1, 2, 3, 5, 7, 10, 16, 28};

void computeViz() {
  // Copy samples (avoid issues with concurrent writes)
  int16_t samples[VIZ_SAMPLES];
  int pos = vizPos; // snapshot
  for (int i = 0; i < VIZ_SAMPLES; i++) {
    samples[i] = vizBuf[(pos + i) % VIZ_SAMPLES];
  }

  // Compute DFT magnitude for each band
  for (int b = 0; b < 8; b++) {
    float sumMag = 0;
    for (int k = bandStart[b]; k <= bandEnd[b] && k < VIZ_SAMPLES / 2; k++) {
      float re = 0, im = 0;
      for (int n = 0; n < VIZ_SAMPLES; n++) {
        float angle = 2.0f * M_PI * k * n / VIZ_SAMPLES;
        re += samples[n] * cosf(angle);
        im -= samples[n] * sinf(angle);
      }
      float mag = sqrtf(re * re + im * im) / VIZ_SAMPLES;
      sumMag += mag;
    }
    int bins = bandEnd[b] - bandStart[b] + 1;
    float avg = sumMag / bins;

    // Normalize: typical music peaks around 5000-15000
    float norm = avg / 8000.0f;
    if (norm > 1.0f) norm = 1.0f;

    // Smooth: fast attack, slow decay
    if (norm > vizBars[b])
      vizBars[b] = vizBars[b] * 0.3f + norm * 0.7f;
    else
      vizBars[b] = vizBars[b] * 0.85f + norm * 0.15f;

    // Peak hold with decay
    if (vizBars[b] > vizPeak[b]) vizPeak[b] = vizBars[b];
    else vizPeak[b] *= 0.96f;
  }
}

// ═════════════════════════════════════════════════════════════
//  SD SCANNER
// ═════════════════════════════════════════════════════════════
void addFile(const char* path) {
  if (trackCount >= MAX_TRACKS) return;
  playlist[trackCount++] = strdup(path);
}

void scanDir(File dir, int depth) {
  if (depth > 2) return;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      scanDir(entry, depth + 1);
    } else {
      const char* name = entry.name();
      if (isMP3(name) || isWAV(name)) addFile(entry.path());
    }
    entry.close();
  }
}

void scanSD() {
  File root = SD.open("/");
  scanDir(root, 0);
  root.close();

  // Sort playlist by path so tracks within each folder play in order
  // (FAT32 directory order is not guaranteed alphabetical)
  for (int i = 0; i < trackCount - 1; i++) {
    for (int j = 0; j < trackCount - 1 - i; j++) {
      if (strcmp(playlist[j], playlist[j + 1]) > 0) {
        char* tmp = playlist[j];
        playlist[j] = playlist[j + 1];
        playlist[j + 1] = tmp;
      }
    }
  }
}

/** Extract unique folder names from the flat playlist as "albums". */
void scanAlbums() {
  albumCount = 0;
  for (int i = 0; i < trackCount && albumCount < MAX_ALBUMS; i++) {
    const char* path = playlist[i];
    // Find the parent folder: e.g. "/Music/Album/track.mp3" → "/Music/Album"
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash || lastSlash == path) continue; // root-level file, skip for albums

    int folderLen = (int)(lastSlash - path);

    // Extract just the folder display name (last component)
    // e.g. "/Music/Album" → "Album"
    const char* folderStart = path;
    for (int j = folderLen - 1; j >= 0; j--) {
      if (path[j] == '/') { folderStart = path + j + 1; break; }
    }
    int nameLen = (int)(lastSlash - folderStart);
    if (nameLen <= 0 || nameLen >= MAX_ALBUM_NAME) continue;

    // Check if we already have this album
    char candidate[MAX_ALBUM_NAME];
    strncpy(candidate, folderStart, nameLen);
    candidate[nameLen] = '\0';

    // Skip "System Volume Information" and similar
    if (strcmp(candidate, "System Volume Information") == 0) continue;

    bool exists = false;
    for (int a = 0; a < albumCount; a++) {
      if (strcmp(albumNames[a], candidate) == 0) { exists = true; break; }
    }
    if (!exists) {
      strncpy(albumNames[albumCount], candidate, MAX_ALBUM_NAME - 1);
      albumNames[albumCount][MAX_ALBUM_NAME - 1] = '\0';
      albumCount++;
    }
  }

  // Also add a "All Tracks" virtual album at the start
  // (shift everything down)
  if (albumCount < MAX_ALBUMS - 1) {
    for (int i = albumCount; i > 0; i--) {
      strncpy(albumNames[i], albumNames[i-1], MAX_ALBUM_NAME);
    }
    strncpy(albumNames[0], "[ All Tracks ]", MAX_ALBUM_NAME);
    albumCount++;
  }

  Serial.printf("[OK] %d albums\n", albumCount);
}

/** Find playlist indices for tracks belonging to a folder name. */
void loadBrowseAlbumTracks(const char* albumName) {
  browseTrackCount = 0;

  // "All Tracks" = show everything
  if (strcmp(albumName, "[ All Tracks ]") == 0) {
    for (int i = 0; i < trackCount && browseTrackCount < MAX_BROWSE_TRACKS; i++) {
      browseTrackIndices[browseTrackCount++] = i;
    }
    return;
  }

  for (int i = 0; i < trackCount && browseTrackCount < MAX_BROWSE_TRACKS; i++) {
    const char* path = playlist[i];
    // Check if this track's parent folder matches
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash) continue;

    // Walk backwards to find the folder name
    const char* folderStart = path;
    int folderLen = (int)(lastSlash - path);
    for (int j = folderLen - 1; j >= 0; j--) {
      if (path[j] == '/') { folderStart = path + j + 1; break; }
    }
    int nameLen = (int)(lastSlash - folderStart);

    if (nameLen == (int)strlen(albumName) &&
        strncmp(folderStart, albumName, nameLen) == 0) {
      browseTrackIndices[browseTrackCount++] = i;
    }
  }
}

// ═════════════════════════════════════════════════════════════
//  BOOKMARKS — audiobook progress saved to SD
// ═════════════════════════════════════════════════════════════
void loadBookmarks() {
  bookmarkCount = 0;
  File f = SD.open(BOOKMARK_FILE, FILE_READ);
  if (!f) return;

  while (f.available() && bookmarkCount < MAX_BOOKMARKS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    Bookmark& b = bookmarks[bookmarkCount];
    // Parse: albumName|trackIdx|bytePos|totalTracks|completed
    int p1 = line.indexOf('|');
    if (p1 < 0) continue;
    int p2 = line.indexOf('|', p1 + 1);
    if (p2 < 0) continue;
    int p3 = line.indexOf('|', p2 + 1);
    if (p3 < 0) continue;
    int p4 = line.indexOf('|', p3 + 1);

    String name = line.substring(0, p1);
    strncpy(b.albumName, name.c_str(), MAX_ALBUM_NAME - 1);
    b.albumName[MAX_ALBUM_NAME - 1] = '\0';
    b.trackIdx = line.substring(p1 + 1, p2).toInt();
    b.bytePos = (uint32_t)line.substring(p2 + 1, p3).toInt();
    b.totalTracks = line.substring(p3 + 1, p4 > 0 ? p4 : line.length()).toInt();
    b.completed = (p4 > 0) ? (line.substring(p4 + 1).toInt() != 0) : false;

    bookmarkCount++;
  }
  f.close();
  Serial.printf("[OK] Loaded %d bookmarks\n", bookmarkCount);
}

void saveBookmarks() {
  File f = SD.open(BOOKMARK_FILE, FILE_WRITE);
  if (!f) { Serial.println("[ERR] Can't write bookmarks"); return; }

  for (int i = 0; i < bookmarkCount; i++) {
    Bookmark& b = bookmarks[i];
    f.printf("%s|%d|%lu|%d|%d\n", b.albumName, b.trackIdx,
            (unsigned long)b.bytePos, b.totalTracks, b.completed ? 1 : 0);
  }
  f.close();
}

Bookmark* findBookmark(const char* albumName) {
  for (int i = 0; i < bookmarkCount; i++) {
    if (strcmp(bookmarks[i].albumName, albumName) == 0) return &bookmarks[i];
  }
  return nullptr;
}

void updateBookmark(const char* albumName, int trackIdx, uint32_t bytePos, int totalTracks, bool completed) {
  Bookmark* b = findBookmark(albumName);
  if (!b) {
    if (bookmarkCount >= MAX_BOOKMARKS) return; // full
    b = &bookmarks[bookmarkCount++];
    strncpy(b->albumName, albumName, MAX_ALBUM_NAME - 1);
    b->albumName[MAX_ALBUM_NAME - 1] = '\0';
  }
  b->trackIdx = trackIdx;
  b->bytePos = bytePos;
  b->totalTracks = totalTracks;
  b->completed = completed;
}

/** Figure out current track's index within its album folder. */
int getAlbumTrackIndex() {
  if (playingAlbumName[0] == '\0' || trackCount == 0) return 0;

  // Count how many tracks before currentTrack belong to the same album
  int actual = resolveTrack(currentTrack);
  const char* curPath = playlist[actual];
  const char* lastSlash = strrchr(curPath, '/');
  if (!lastSlash) return 0;
  int folderLen = (int)(lastSlash - curPath);

  int albumIdx = 0;
  for (int i = 0; i < trackCount; i++) {
    if (strncmp(playlist[i], curPath, folderLen) == 0 && playlist[i][folderLen] == '/') {
      if (i == actual) return albumIdx;
      albumIdx++;
    }
  }
  return 0;
}

/** Count total tracks in the same folder as the current track. */
int getAlbumTrackCount() {
  int actual = resolveTrack(currentTrack);
  const char* curPath = playlist[actual];
  const char* lastSlash = strrchr(curPath, '/');
  if (!lastSlash) return 1;
  int folderLen = (int)(lastSlash - curPath);

  int count = 0;
  for (int i = 0; i < trackCount; i++) {
    if (strncmp(playlist[i], curPath, folderLen) == 0 && playlist[i][folderLen] == '/')
      count++;
  }
  return count;
}

/** Save current playback position as a bookmark. */
void saveCurrentBookmark() {
  if (playingAlbumName[0] == '\0') return;
  if (playerState == STATE_STOPPED) return;

  uint32_t pos = audioFile ? audioFile->getPos() : 0;
  int aIdx = getAlbumTrackIndex();
  int aTotal = getAlbumTrackCount();

  updateBookmark(playingAlbumName, aIdx, pos, aTotal, false);
  saveBookmarks();
}

/** Extract album name from a playlist path for bookmark tracking. */
void setPlayingAlbumFromPath(const char* path) {
  const char* lastSlash = strrchr(path, '/');
  if (!lastSlash || lastSlash == path) {
    playingAlbumName[0] = '\0';
    return;
  }
  int folderLen = (int)(lastSlash - path);
  const char* folderStart = path;
  for (int j = folderLen - 1; j >= 0; j--) {
    if (path[j] == '/') { folderStart = path + j + 1; break; }
  }
  int nameLen = (int)(lastSlash - folderStart);
  if (nameLen <= 0 || nameLen >= MAX_ALBUM_NAME) { playingAlbumName[0] = '\0'; return; }
  strncpy(playingAlbumName, folderStart, nameLen);
  playingAlbumName[nameLen] = '\0';
}

// ═════════════════════════════════════════════════════════════
//  SHUFFLE
// ═════════════════════════════════════════════════════════════
void generateShuffleOrder() {
  for (int i = 0; i < trackCount; i++) shuffleOrder[i] = i;
  for (int i = trackCount - 1; i > 0; i--) {
    int j = random(0, i + 1);
    int tmp = shuffleOrder[i];
    shuffleOrder[i] = shuffleOrder[j];
    shuffleOrder[j] = tmp;
  }
}

// ═════════════════════════════════════════════════════════════
//  PLAYBACK
// ═════════════════════════════════════════════════════════════
int resolveTrack(int index) {
  // SHUFFLE_TRACKS: use the shuffled order
  // SHUFFLE_ALBUM / SHUFFLE_OFF: sequential (album shuffle is handled in nextTrack)
  return (shuffleMode == SHUFFLE_TRACKS) ? shuffleOrder[index % trackCount] : (index % trackCount);
}

void stopTrack() {
  if (mp3 && mp3->isRunning()) mp3->stop();
  if (wav && wav->isRunning()) wav->stop();
  if (mp3)       { delete mp3;       mp3 = nullptr; }
  if (wav)       { delete wav;       wav = nullptr; }
  if (audioFile) { delete audioFile; audioFile = nullptr; }
  currentType = AUDIO_NONE;
}

// Seek target for resume — set before calling startTrack, 0 = no seek
static uint32_t resumeSeekPos = 0;

void startTrack(int index, bool gapless) {
  // Save bookmark for the track we're leaving
  if (playerState == STATE_PLAYING || playerState == STATE_PAUSED) {
    saveCurrentBookmark();
  }

  stopTrack();

  // For gapless: don't flush ring buffer, let remaining audio drain
  if (!gapless) {
    rbHead = 0;
    rbTail = 0;
  }

  currentTrack = index % trackCount;
  int actual = resolveTrack(currentTrack);
  const char* path = playlist[actual];
  Serial.printf("Playing [%d]: %s%s\n", actual, path, gapless ? " (gapless)" : "");

  // Track which album is playing for bookmarks
  setPlayingAlbumFromPath(path);

  audioFile = new AudioFileSourceSD(path);

  if (isWAV(path)) {
    wav = new AudioGeneratorWAV();
    wav->begin(audioFile, audioOut);
    currentType = AUDIO_WAV;
  } else {
    mp3 = new AudioGeneratorMP3();
    mp3->begin(audioFile, audioOut);
    currentType = AUDIO_MP3;
  }

  // Seek to resume position if set
  if (resumeSeekPos > 0 && audioFile) {
    Serial.printf("Resuming at byte %lu\n", (unsigned long)resumeSeekPos);
    audioFile->seek(resumeSeekPos, SEEK_SET);
    resumeSeekPos = 0;
  }

  playerState = STATE_PLAYING;

  // Init playback timing
  trackWallStartMs = millis();
  accumulatedPauseMs = 0;
  pauseBeganMs = 0;
  cachedWavRateHz = 0;
  cachedDurationSec = 0;
  if (currentType == AUDIO_MP3 && audioFile) {
    uint32_t sz = audioFile->getSize();
    cachedDurationSec = (sz > 0) ? (sz / 16000u) : 0; // ~128kbps estimate
  }

  // Pre-fill buffer (less aggressively for gapless since buffer has data)
  int target = gapless ? RING_SIZE / 4 : RING_SIZE / 2;
  int prefill = 0;
  while ((int)rbAvail() < target && prefill < 4096) {
    bool ok = false;
    if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) ok = mp3->loop();
    else if (currentType == AUDIO_WAV && wav && wav->isRunning()) ok = wav->loop();
    if (!ok) break;
    prefill++;
  }
  Serial.printf("Pre-filled buf=%d\n", (int)rbAvail());

  if (screenMode == SCREEN_MAIN) {
    drawTrackName();
    drawTransport();
    drawProgressBar();
  }
}

void nextTrack(bool gapless) {
  if (shuffleMode == SHUFFLE_ALBUM && trackCount > 1) {
    // Find current track's folder
    int actual = resolveTrack(currentTrack);
    const char* curPath = playlist[actual];
    const char* lastSlash = strrchr(curPath, '/');
    int folderLen = lastSlash ? (int)(lastSlash - curPath) : 0;

    // Collect indices of tracks in the same folder
    int sameFolder[MAX_TRACKS];
    int sameFolderCount = 0;
    for (int i = 0; i < trackCount && sameFolderCount < MAX_TRACKS; i++) {
      if (folderLen > 0 && strncmp(playlist[i], curPath, folderLen) == 0 &&
          playlist[i][folderLen] == '/') {
        sameFolder[sameFolderCount++] = i;
      }
    }

    if (sameFolderCount > 1) {
      // Pick a random track from same folder, avoid current
      int pick = random(0, sameFolderCount);
      if (sameFolder[pick] == actual) pick = (pick + 1) % sameFolderCount;
      // Find which shuffleOrder index maps to this track
      // Simplest: just play it directly by its playlist index
      startTrack(sameFolder[pick], gapless);
      return;
    }
  }
  startTrack(currentTrack + 1, gapless);
}

void prevTrack() {
  startTrack(currentTrack > 0 ? currentTrack - 1 : trackCount - 1);
}

void togglePause() {
  if (playerState == STATE_PLAYING) {
    playerState = STATE_PAUSED;
    pauseBeganMs = millis();
    saveCurrentBookmark();
  } else if (playerState == STATE_PAUSED) {
    if (pauseBeganMs) {
      accumulatedPauseMs += (millis() - pauseBeganMs);
      pauseBeganMs = 0;
    }
    playerState = STATE_PLAYING;
  }
  if (screenMode == SCREEN_MAIN) drawTransport();
}

// ── Progress helpers ──
float getProgress() {
  if (!audioFile) return 0;
  uint32_t pos = audioFile->getPos();
  uint32_t sz  = audioFile->getSize();
  if (sz == 0) return 0;
  return (float)pos / (float)sz;
}

void formatTime(int seconds, char* buf) {
  int m = seconds / 60;
  int s = seconds % 60;
  sprintf(buf, "%d:%02d", m, s);
}

// ═════════════════════════════════════════════════════════════
//  MAIN SCREEN DRAWING
// ═════════════════════════════════════════════════════════════
void drawMainScreen() {
  tft.fillScreen(COL_BG);
  drawTopBar();
  audioPumpPlayingMax(128);
  drawTrackName();
  drawVisualizer();
  audioPumpPlayingMax(128);
  drawProgressBar();
  drawTransport();
  drawBottomBar();
  audioPumpPlayingMax(256);
}

void drawTopBar() {
  tft.fillRect(0, 0, 320, TOP_H, COL_BG);

  // BT status
  tft.setTextSize(1);
  if (btConnected) {
    tft.setTextColor(COL_BT_ON, COL_BG);
    tft.setCursor(4, 4);
    tft.print("BT");
  } else {
    tft.setTextColor(COL_BT_OFF, COL_BG);
    tft.setCursor(4, 4);
    tft.print("BT..");
  }

  // Track counter
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(240, 4);
  tft.printf("Track %d/%d", currentTrack + 1, trackCount);
}

void drawTrackName() {
  tft.fillRect(0, TITLE_Y, 320, TITLE_H, COL_BG);

  char display[28];
  getDisplayName(playlist[resolveTrack(currentTrack)], display, 27);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextSize(2);
  int16_t tw = strlen(display) * 12;
  int16_t tx = max(4, (320 - tw) / 2);
  tft.setCursor(tx, TITLE_Y + 2);
  tft.print(display);
}

void drawVisualizer() {
  // Draw 8 bars with peaks
  int barW = 34;
  int gap  = 4;
  int startX = (320 - (barW * 8 + gap * 7)) / 2;

  for (int i = 0; i < 8; i++) {
    int x = startX + i * (barW + gap);
    int barH = (int)(vizBars[i] * VIZ_H);
    int peakH = (int)(vizPeak[i] * VIZ_H);

    barH = constrain(barH, 0, VIZ_H);
    peakH = constrain(peakH, 0, VIZ_H);

    // Clear bar area
    tft.fillRect(x, VIZ_Y, barW, VIZ_H, COL_BG);

    // Draw bar from bottom up
    if (barH > 0) {
      tft.fillRect(x, VIZ_Y + VIZ_H - barH, barW, barH, vizColors[i]);
    }

    // Peak indicator (thin white line)
    if (peakH > 1) {
      int peakY = VIZ_Y + VIZ_H - peakH;
      tft.drawFastHLine(x, peakY, barW, COL_TEXT);
    }
  }
}

void drawProgressBar() {
  int barX = 45;
  int barW = 230;
  int barH = 6;
  int barY = PROG_Y + 6;

  tft.fillRect(0, PROG_Y, 320, PROG_H, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);

  uint32_t el = elapsedPlaybackSec();
  float prog = 0;

  char eBuf[12], tBuf[12];
  formatTimeHMS(eBuf, sizeof(eBuf), el);
  if (cachedDurationSec > 0) {
    formatTimeHMS(tBuf, sizeof(tBuf), cachedDurationSec);
    prog = (float)el / (float)cachedDurationSec;
    if (prog > 1.0f) prog = 1.0f;
  } else {
    strncpy(tBuf, "--:--", sizeof(tBuf));
  }

  tft.setCursor(4, PROG_Y + 3);
  tft.print(eBuf);
  tft.setCursor(280, PROG_Y + 3);
  tft.print(tBuf);

  // Bar
  tft.fillRoundRect(barX, barY, barW, barH, 2, COL_PROG_BG);
  int fillW = (int)(barW * prog);
  if (fillW > 0) tft.fillRoundRect(barX, barY, fillW, barH, 2, COL_PROG_FG);
  int dotX = barX + fillW;
  tft.fillCircle(constrain(dotX, barX, barX + barW), barY + barH / 2, 4, COL_ACCENT);
}

void drawTransport() {
  // Prev
  tft.fillRoundRect(BTN_PREV_X + 5, BTN_Y, BTN_PREV_W - 10, BTN_H, 8, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(3);
  tft.setCursor(BTN_PREV_X + 20, BTN_Y + 18);
  tft.print("|<");

  // Play/Pause
  uint16_t pc = (playerState == STATE_PLAYING) ? COL_BTN_ACT : COL_BTN;
  tft.fillRoundRect(BTN_PLAY_X + 5, BTN_Y, BTN_PLAY_W - 10, BTN_H, 8, pc);
  tft.setTextColor(COL_TEXT, pc);
  tft.setTextSize(3);
  tft.setCursor(BTN_PLAY_X + 42, BTN_Y + 18);
  tft.print(playerState == STATE_PLAYING ? "| |" : " >");

  // Next
  tft.fillRoundRect(BTN_NEXT_X + 5, BTN_Y, BTN_NEXT_W - 10, BTN_H, 8, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(3);
  tft.setCursor(BTN_NEXT_X + 13, BTN_Y + 18);
  tft.print(">|");
}

void drawBottomBar() {
  tft.fillRect(0, BAR_Y, 320, 64, COL_BG);

  // Row 1: Shuffle mode + Browse button
  tft.setTextSize(1);
  uint16_t shfCol = (shuffleMode != SHUFFLE_OFF) ? COL_ACCENT : COL_DIM;
  tft.fillRoundRect(4, BAR_Y + 1, 72, 22, 4, COL_BTN);
  tft.setTextColor(shfCol, COL_BTN);
  tft.setCursor(8, BAR_Y + 7);
  if (shuffleMode == SHUFFLE_OFF)    tft.print("SHF: OFF");
  else if (shuffleMode == SHUFFLE_TRACKS) tft.print("SHF: ALL");
  else                                tft.print("SHF: ALB");

  // Browse button
  tft.fillRoundRect(80, BAR_Y + 1, 80, 22, 4, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(1);
  tft.setCursor(97, BAR_Y + 7);
  tft.print("BROWSE");

  // Row 2: Volume +/- buttons
  tft.fillRoundRect(8, BAR_Y + 28, 60, 22, 4, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(2);
  tft.setCursor(28, BAR_Y + 32);
  tft.print("-");

  tft.fillRoundRect(252, BAR_Y + 28, 60, 22, 4, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(272, BAR_Y + 32);
  tft.print("+");

  // Volume bar (center, display only)
  int vx = 76, vw = 168, vy = BAR_Y + 32, vh = 14;
  tft.fillRoundRect(vx, vy, vw, vh, 3, COL_VOL_BG);
  int fw = (int)(vw * volumePercent / 100);
  if (fw > 0) tft.fillRoundRect(vx, vy, fw, vh, 3, COL_VOL_FG);

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(140, BAR_Y + 50);
  tft.printf("Vol: %d%%", volumePercent);
}

// ═════════════════════════════════════════════════════════════
//  BROWSE SCREEN (two-level: albums → tracks)
// ═════════════════════════════════════════════════════════════
void drawBrowseScreen() {
  tft.fillScreen(COL_BG);

  // Header
  tft.fillRect(0, 0, 320, 26, COL_BROWSE_HDR);
  tft.setTextColor(COL_TEXT, COL_BROWSE_HDR);
  tft.setTextSize(2);
  tft.setCursor(60, 5);

  if (browseLevel == BROWSE_ALBUMS) {
    tft.print("Albums");
    // Back → main screen
    tft.fillRoundRect(4, 3, 50, 20, 4, COL_BTN);
    tft.setTextColor(COL_TEXT, COL_BTN);
    tft.setTextSize(1);
    tft.setCursor(12, 9);
    tft.print("< Back");
    // Count
    tft.setTextColor(COL_DIM, COL_BROWSE_HDR);
    tft.setCursor(230, 10);
    tft.printf("%d albums", albumCount);
  } else {
    // Show album name in header
    char hdr[24];
    strncpy(hdr, albumNames[browseAlbumIdx], 20);
    hdr[20] = '\0';
    tft.print(hdr);
    // Back → album list
    tft.fillRoundRect(4, 3, 50, 20, 4, COL_BTN);
    tft.setTextColor(COL_TEXT, COL_BTN);
    tft.setTextSize(1);
    tft.setCursor(8, 9);
    tft.print("< Albs");
    // Count
    tft.setTextColor(COL_DIM, COL_BROWSE_HDR);
    tft.setCursor(230, 10);
    tft.printf("%d trks", browseTrackCount);

    // Resume button if bookmark exists
    Bookmark* bm = (browseAlbumIdx > 0) ? findBookmark(albumNames[browseAlbumIdx]) : nullptr;
    if (bm && !bm->completed && bm->trackIdx < browseTrackCount) {
      tft.fillRoundRect(80, 218, 160, 20, 4, tft.color565(50, 80, 50));
      tft.setTextColor(COL_ACCENT, tft.color565(50, 80, 50));
      tft.setTextSize(1);
      tft.setCursor(92, 224);
      tft.printf("RESUME (trk %d)", bm->trackIdx + 1);
    }
  }

  // Scroll buttons
  tft.fillRoundRect(4, 218, 60, 20, 4, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(1);
  tft.setCursor(22, 224);
  tft.print("UP");

  tft.fillRoundRect(256, 218, 60, 20, 4, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(268, 224);
  tft.print("DOWN");

  // Now playing indicator
  if (playerState == STATE_PLAYING || playerState == STATE_PAUSED) {
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.setTextSize(1);
    tft.setCursor(80, 224);
    tft.print(playerState == STATE_PLAYING ? "> playing" : "|| paused");
  }

  drawBrowseList();
  audioPumpPlayingMax(384);
}

void drawBrowseList() {
  int listY = 28;
  int listH = 188;
  tft.fillRect(0, listY, 320, listH, COL_BG);

  if (browseLevel == BROWSE_ALBUMS) {
    // Show album folders with progress
    for (int i = 0; i < browseVisible && (browseScroll + i) < albumCount; i++) {
      int idx = browseScroll + i;
      int y = listY + i * browseItemH;

      tft.fillRect(0, y, 320, browseItemH - 2, COL_BROWSE_BG);
      tft.setTextSize(1);

      // Folder icon
      tft.setTextColor(COL_DIM, COL_BROWSE_BG);
      tft.setCursor(6, y + 7);
      tft.print(idx == 0 ? "*" : "#");

      // Album name
      tft.setTextColor(COL_TEXT, COL_BROWSE_BG);
      tft.setCursor(18, y + 7);
      tft.print(albumNames[idx]);

      // Show bookmark progress (skip "All Tracks" entry)
      if (idx > 0) {
        Bookmark* bm = findBookmark(albumNames[idx]);
        if (bm) {
          tft.setCursor(248, y + 7);
          if (bm->completed) {
            tft.setTextColor(COL_ACCENT, COL_BROWSE_BG);
            tft.print("Done");
          } else {
            tft.setTextColor(tft.color565(180, 180, 80), COL_BROWSE_BG);
            tft.printf("%d/%d", bm->trackIdx + 1, bm->totalTracks);
          }
        } else {
          tft.setTextColor(COL_DIM, COL_BROWSE_BG);
          tft.setCursor(268, y + 7);
          tft.print("New");
        }
      }

      tft.setTextColor(COL_DIM, COL_BROWSE_BG);
      tft.setCursor(304, y + 7);
      tft.print(">");
    }
  } else {
    // Show tracks in selected album
    for (int i = 0; i < browseVisible && (browseScroll + i) < browseTrackCount; i++) {
      int idx = browseScroll + i;
      int playlistIdx = browseTrackIndices[idx];
      int y = listY + i * browseItemH;

      // Is this the currently playing track?
      bool isCurrent = (playlistIdx == resolveTrack(currentTrack));

      tft.fillRect(0, y, 320, browseItemH - 2,
                   isCurrent ? COL_BROWSE_SEL : COL_BROWSE_BG);
      uint16_t bg = isCurrent ? COL_BROWSE_SEL : COL_BROWSE_BG;

      // Track number
      tft.setTextColor(COL_DIM, bg);
      tft.setTextSize(1);
      tft.setCursor(6, y + 7);
      tft.printf("%02d", idx + 1);

      // Track name
      char name[42];
      getDisplayName(playlist[playlistIdx], name, 41);
      tft.setTextColor(isCurrent ? COL_ACCENT : COL_TEXT, bg);
      tft.setCursor(28, y + 3);
      tft.print(name);

      // Playing indicator
      if (isCurrent && playerState == STATE_PLAYING) {
        tft.setTextColor(COL_ACCENT, bg);
        tft.setCursor(304, y + 7);
        tft.print(">");
      }
    }
  }
}

// ═════════════════════════════════════════════════════════════
//  TOUCH — MAIN SCREEN
// ═════════════════════════════════════════════════════════════
void handleTouchMain() {
  if (!ts.touched()) return;
  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE) return;
  lastTouchTime = now;

  TS_Point p = ts.getPoint();
  int16_t tx = map(p.x, TS_MINX, TS_MAXX, 0, 320);
  int16_t ty = map(p.y, TS_MINY, TS_MAXY, 0, 240);
  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);

  // Transport buttons
  if (ty >= BTN_Y && ty < BTN_Y + BTN_H) {
    if      (tx < BTN_PREV_X + BTN_PREV_W) prevTrack();
    else if (tx < BTN_PLAY_X + BTN_PLAY_W) togglePause();
    else                                     nextTrack();
    return;
  }

  // Bottom bar
  if (ty >= BAR_Y) {
    // Shuffle toggle
    if (tx < 70 && ty < BAR_Y + 26) {
      // Cycle shuffle mode
      if (shuffleMode == SHUFFLE_OFF) {
        shuffleMode = SHUFFLE_TRACKS;
        generateShuffleOrder();
      } else if (shuffleMode == SHUFFLE_TRACKS) {
        shuffleMode = SHUFFLE_ALBUM;
      } else {
        shuffleMode = SHUFFLE_OFF;
      }
      drawBottomBar();
      return;
    }

    // Browse button
    if (tx >= 80 && tx < 160 && ty < BAR_Y + 26) {
      screenMode = SCREEN_BROWSE;
      browseLevel = BROWSE_ALBUMS;
      browseScroll = 0;
      drawBrowseScreen();
      return;
    }

    // Volume -/+ (row 2)
    if (ty >= BAR_Y + 28) {
      if (tx < 80) {
        volumePercent -= 10;
        applyVolumePercent();
        drawBottomBar();
      } else if (tx >= 240) {
        volumePercent += 10;
        applyVolumePercent();
        drawBottomBar();
      }
      return;
    }
  }
}

// ═════════════════════════════════════════════════════════════
//  TOUCH — BROWSE SCREEN
// ═════════════════════════════════════════════════════════════
void handleTouchBrowse() {
  if (!ts.touched()) return;
  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE) return;
  lastTouchTime = now;

  TS_Point p = ts.getPoint();
  int16_t tx = map(p.x, TS_MINX, TS_MAXX, 0, 320);
  int16_t ty = map(p.y, TS_MINY, TS_MAXY, 0, 240);
  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);

  // Back button
  if (ty < 26 && tx < 60) {
    if (browseLevel == BROWSE_TRACKS) {
      // Go back to album list
      browseLevel = BROWSE_ALBUMS;
      browseScroll = 0;
      drawBrowseScreen();
    } else {
      // Go back to main screen
      screenMode = SCREEN_MAIN;
      drawMainScreen();
    }
    return;
  }

  // Scroll up
  if (ty >= 218 && tx < 80) {
    browseScroll = max(0, browseScroll - browseVisible);
    drawBrowseList();
    return;
  }

  // Scroll down
  if (ty >= 218 && tx > 240) {
    int maxItems = (browseLevel == BROWSE_ALBUMS) ? albumCount : browseTrackCount;
    browseScroll = min(maxItems - browseVisible, browseScroll + browseVisible);
    if (browseScroll < 0) browseScroll = 0;
    drawBrowseList();
    return;
  }

  // Resume button (center of bottom bar, only in track view)
  if (ty >= 218 && tx >= 80 && tx <= 240 && browseLevel == BROWSE_TRACKS) {
    Bookmark* bm = (browseAlbumIdx > 0) ? findBookmark(albumNames[browseAlbumIdx]) : nullptr;
    if (bm && !bm->completed && bm->trackIdx < browseTrackCount) {
      int playlistIdx = browseTrackIndices[bm->trackIdx];
      currentTrack = playlistIdx;
      resumeSeekPos = bm->bytePos;
      startTrack(playlistIdx);
      screenMode = SCREEN_MAIN;
      drawMainScreen();
    }
    return;
  }

  // Tap on list (y: 28 to 216)
  if (ty >= 28 && ty < 216) {
    int tapped = (ty - 28) / browseItemH;
    int idx = browseScroll + tapped;

    if (browseLevel == BROWSE_ALBUMS) {
      // Tapped an album → show its tracks
      if (idx < albumCount) {
        browseAlbumIdx = idx;
        loadBrowseAlbumTracks(albumNames[idx]);
        browseLevel = BROWSE_TRACKS;
        // Auto-scroll to bookmarked track if available
        Bookmark* bm = (idx > 0) ? findBookmark(albumNames[idx]) : nullptr;
        if (bm && !bm->completed && bm->trackIdx < browseTrackCount) {
          browseScroll = max(0, bm->trackIdx - 2); // show bookmark in context
        } else {
          browseScroll = 0;
        }
        drawBrowseScreen();
      }
    } else {
      // Tapped a track → play it, go to main
      if (idx < browseTrackCount) {
        int playlistIdx = browseTrackIndices[idx];
        currentTrack = playlistIdx;
        resumeSeekPos = 0; // start from beginning when tapping explicitly
        startTrack(playlistIdx);
        screenMode = SCREEN_MAIN;
        drawMainScreen();
      }
    }
    return;
  }
}

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════
bool isMP3(const char* fn) {
  int l = strlen(fn);
  return l >= 4 && strcasecmp(fn + l - 4, ".mp3") == 0;
}

bool isWAV(const char* fn) {
  int l = strlen(fn);
  return l >= 4 && strcasecmp(fn + l - 4, ".wav") == 0;
}

const char* getFilename(const char* path) {
  const char* name = strrchr(path, '/');
  return name ? name + 1 : path;
}

void getDisplayName(const char* path, char* out, int maxLen) {
  const char* name = getFilename(path);
  strncpy(out, name, maxLen);
  out[maxLen] = '\0';
  // Strip extension
  char* dot = strrchr(out, '.');
  if (dot) *dot = '\0';
  // Strip common suffixes like " (128kbit_AAC)"
  char* paren = strrchr(out, '(');
  if (paren && paren > out) {
    // Trim trailing space before paren
    char* trim = paren - 1;
    while (trim > out && *trim == ' ') trim--;
    *(trim + 1) = '\0';
  }
  // Truncate with ellipsis
  if ((int)strlen(out) > maxLen - 3) {
    out[maxLen - 4] = '.';
    out[maxLen - 3] = '.';
    out[maxLen - 2] = '.';
    out[maxLen - 1] = '\0';
  }
}
