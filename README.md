# ShuffleCYD

A Bluetooth MP3 player built on the ESP32-2432S028 (CYD — Cheap Yellow Display), inspired by the iPod Shuffle.

## Features

- **Bluetooth A2DP** — streams to any Bluetooth speaker or headphones
- **Album Browser** — two-level touch browser: album folders → tracks within
- **Spectrum Visualizer** — 8-band frequency analyzer with peak hold
- **Track Progress Bar** — elapsed/total time with visual indicator
- **Near-Gapless Playback** — minimal silence between tracks
- **Three Shuffle Modes** — Off / All Tracks / Within Album folder
- **Touch Volume Control** — full-width slider
- **Background Playback** — music keeps playing while browsing albums

## Hardware

- **Board:** ESP32-2432S028 (CYD) — ILI9341 320×240 + XPT2046 resistive touch
- **Storage:** MicroSD card (FAT32) with MP3 or WAV files
- **Audio:** Bluetooth A2DP to any BT speaker or headphones

No extra wiring. Just the CYD board, an SD card with music, and a Bluetooth speaker.

## SD Card Layout

Organize your music in folders — each folder becomes an album in the browser:

```
SD Card
├── Music/
│   ├── Album One/
│   │   ├── 01 Track.mp3
│   │   ├── 02 Track.mp3
│   │   └── ...
│   ├── Album Two/
│   │   ├── 01 Song.mp3
│   │   └── ...
│   └── Loose Track.mp3      ← root-level files go under "All Tracks"
└── Another Album/
    ├── song1.mp3
    └── song2.wav
```

The browser shows a **[ All Tracks ]** entry at the top that lists every track across all folders.

## Setup

### 1. Install Libraries

Install via Arduino Library Manager:

- **ESP8266Audio** by Earle Philhower
- **ESP32-A2DP** by Phil Schatzmann
- **TFT_eSPI** by Bodmer
- **XPT2046_Touchscreen** by Paul Stoffregen

### 2. Configure TFT_eSPI

Copy `User_Setup.h` from this repo into your TFT_eSPI library folder, **replacing** the existing one:

| OS | Path |
|----|------|
| Windows | `Documents\Arduino\libraries\TFT_eSPI\User_Setup.h` |
| macOS | `~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h` |
| Linux | `~/Arduino/libraries/TFT_eSPI/User_Setup.h` |

There are **3 known CYD display variants**. Uncomment the one driver that matches your board:

| Board | USB ports | Driver |
|-------|-----------|--------|
| v1 original | 1× Micro-USB | `ILI9341_DRIVER` |
| v1 alt controller | 1× Micro-USB | `ILI9341_2_DRIVER` |
| v2/v3 newer | USB-C + Micro | `ST7789_2_DRIVER` |

### 3. Arduino IDE Settings

- **Board:** ESP32 Dev Module
- **Partition Scheme:** Huge APP (3MB No OTA/1MB SPIFFS)
- **Upload Speed:** 921600
- **Flash Frequency:** 80MHz

The **Huge APP** partition is required — the BT stack + MP3 decoder + TFT won't fit otherwise.

### 4. Configure

Edit `ShuffleCYD.ino`:

**Bluetooth speaker name** (required):
```cpp
static const char* BT_SPEAKER_NAME = "Your Speaker Name";
```
Must match your speaker's exact name (case-sensitive). Check your phone's BT settings.

**Color inversion** (if colors look negative/inverted):
```cpp
static const bool INVERT_DISPLAY = true;
```

### 5. Prepare Music

Files must be **real MP3 or WAV**. Files downloaded from YouTube as AAC-in-MP3-container won't decode. Convert with ffmpeg:

```bash
ffmpeg -i input.mp3 -codec:a libmp3lame -b:a 128k output.mp3
```

Batch convert (PowerShell):
```powershell
mkdir converted
Get-ChildItem *.mp3 | ForEach-Object { ffmpeg -y -i $_.FullName -codec:a libmp3lame -b:a 128k "converted\$($_.Name)" }
```

### 6. Upload and Pair

1. Flash the sketch
2. Put your BT speaker in **pairing mode** (hold BT button until LED flashes)
3. Disconnect the speaker from your phone first
4. CYD auto-connects and starts playing

## Controls

### Main Screen

```
┌──────────────────────────────────┐
│ BT status          Track 3/45   │
├──────────────────────────────────┤
│         Track Name              │
├──────────────────────────────────┤
│  ▮▮ ▮▮ ▮▮ ▮▮ ▮▮ ▮▮ ▮▮ ▮▮      │  Visualizer
├──────────────────────────────────┤
│  0:42 ━━━━●━━━━━━━━━━━  3:21   │  Progress
├────────┬──────────┬─────────────┤
│  |◄◄   │  ▶ / ❚❚  │    ►►|     │  Transport
├────────┴──────────┴─────────────┤
│ SHF:ALL  [BROWSE]               │
│ ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │  Volume
└──────────────────────────────────┘
```

- **|◄◄** — Previous track
- **▶ / ❚❚** — Play / Pause
- **►►|** — Next track
- **SHF** — Cycle shuffle: OFF → ALL → ALB → OFF
- **BROWSE** — Open album browser
- **Volume bar** — Tap to set

### Album Browser

```
┌──────────────────────────────────┐
│ < Back      Albums    5 albums  │
├──────────────────────────────────┤
│ * [ All Tracks ]              > │
│ # Electronic                  > │
│ # Indie Rock                  > │
│ # Synthwave                   > │
│ # Trip Hop                    > │
├──────────────────────────────────┤
│  [UP]    > playing      [DOWN]  │
└──────────────────────────────────┘
```

- **Tap an album** → shows its tracks
- **Tap a track** → plays it, returns to main screen
- **< Back** → album list → main screen
- **UP / DOWN** — scroll pages
- Now-playing indicator shows at the bottom while music plays

### Shuffle Modes

| Mode | Behavior |
|------|----------|
| **OFF** | Plays tracks in order within the current album |
| **ALL** | Shuffles all tracks across all folders |
| **ALB** | Randomly picks tracks within the same folder |

## Troubleshooting

| Problem | Fix |
|---------|-----|
| White/blank screen | Copy `User_Setup.h` to TFT_eSPI library folder |
| Colors swapped (red↔blue) | Toggle `TFT_BGR` / `TFT_RGB` in User_Setup.h |
| Colors inverted (negative) | Set `INVERT_DISPLAY = true` in the sketch |
| No BT connection | Speaker name must be exact and case-sensitive; speaker must be in pairing mode; disconnect from phone first |
| Songs skip instantly | Files are AAC not MP3 — re-encode with ffmpeg |
| Partition error | Set partition to "Huge APP (3MB No OTA)" |
| Touch not responding | CYD touch is on HSPI — handled in code, but check pin defines match your board |

### Touch Calibration

If touch coordinates feel off, adjust:

```cpp
#define TS_MINX 300
#define TS_MAXX 3900
#define TS_MINY 300
#define TS_MAXY 3900
```

## Architecture

```
SD Card (MP3/WAV files in album folders)
    │
    ├─→ Flat playlist[] (all tracks, recursive scan)
    ├─→ albumNames[] (unique folder names for browser)
    │
    ↓
ESP8266Audio Decoder (MP3 or WAV → PCM)
    ↓
RingBufOutput::ConsumeSample()
    ↓
Ring Buffer (4096 frames, 16KB)  ←→  Visualizer tap (64 mono samples)
    ↓
BT A2DP Callback (pulls frames from ring buffer)
    ↓
Bluetooth Speaker
```

The decoder runs on the main core in `loop()`. The BT A2DP stack runs on core 0 in its own FreeRTOS task and pulls PCM frames via callback. The ring buffer decouples the two.

## Credits

Built by [sparkadium](https://github.com/sparkadium).
&
Built by [malaq88](https://github.com/malaq88/).

## License

MIT
