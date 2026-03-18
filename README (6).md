# ShuffleCYD

An iPod Shuffle-inspired Bluetooth MP3 player built on the ESP32-2432S028 (CYD — Cheap Yellow Display).

![ESP32 CYD](https://github.com/sparkadium/ShuffleCYD)

## Features

- **Bluetooth A2DP** — streams to any Bluetooth speaker or headphones
- **Spectrum Visualizer** — 8-band frequency analyzer with peak hold
- **Track Progress Bar** — elapsed/total time with visual indicator
- **Playlist Browser** — scrollable touch-based track list
- **Near-Gapless Playback** — minimal gap between tracks
- **Shuffle / Sequential** — toggle with on-screen button
- **Touch Volume Control** — full-width slider

## Hardware

- **Board:** ESP32-2432S028 (CYD) — ILI9341 320×240 display + XPT2046 touch
- **Storage:** MicroSD card with MP3 or WAV files
- **Audio:** Bluetooth A2DP to any BT speaker

No extra wiring needed — just the CYD board, an SD card with music, and a Bluetooth speaker.

## Setup

### 1. Install Libraries

Install these via Arduino Library Manager:

- **ESP8266Audio** by Earle Philhower
- **ESP32-A2DP** by Phil Schatzmann  
- **TFT_eSPI** by Bodmer
- **XPT2046_Touchscreen** by Paul Stoffregen

### 2. Configure TFT_eSPI

**This is the most common source of issues.** Copy `User_Setup.h` from this repo into your TFT_eSPI library folder, replacing the existing one:

```
Arduino/libraries/TFT_eSPI/User_Setup.h
```

On Windows this is typically:
```
C:\Users\<you>\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
```

> **If you get a white screen**, this step was missed or the file wasn't replaced correctly.

> **If colors are swapped** (red and blue reversed), edit `User_Setup.h` and change `TFT_BGR` to `TFT_RGB` (or vice versa). Different CYD batches vary.

### 3. Arduino IDE Settings

- **Board:** ESP32 Dev Module
- **Partition Scheme:** Huge APP (3MB No OTA/1MB SPIFFS)
- **Upload Speed:** 921600
- **Flash Frequency:** 80MHz

The "Huge APP" partition is required — the Bluetooth stack + MP3 decoder + TFT driver won't fit in the default partition.

### 4. Set Your Speaker Name

Edit `ShuffleCYD.ino` and change this line near the top:

```cpp
static const char* BT_SPEAKER_NAME = "Anker SoundCore";
```

Replace with your speaker's exact Bluetooth name (case-sensitive). Check your phone's Bluetooth settings if unsure.

### 5. Prepare SD Card

- Format as FAT32
- Create a `/Music/` folder (or put files in root)
- Add `.mp3` or `.wav` files

**Important:** Files must be actual MP3 or WAV format. Files downloaded from YouTube as AAC-in-MP3-container will not work. To convert:

```bash
ffmpeg -i input.mp3 -codec:a libmp3lame -b:a 128k output.mp3
```

Or batch convert in PowerShell:
```powershell
Get-ChildItem *.mp3 | ForEach-Object { ffmpeg -y -i $_.FullName -codec:a libmp3lame -b:a 128k "converted\$($_.Name)" }
```

### 6. Upload & Pair

1. Flash the sketch
2. Put your BT speaker in pairing mode
3. The CYD will connect automatically and start playing

## Touch Controls

### Main Screen
- **|<** — Previous track
- **▶ / ❚❚** — Play / Pause  
- **>|** — Next track
- **SHF** — Toggle shuffle mode
- **BROWSE** — Open playlist browser
- **Volume bar** — Tap to set volume

### Browse Screen
- **Tap a track** — Play it and return to main screen
- **UP / DOWN** — Scroll through playlist
- **< Back** — Return to main screen

## Troubleshooting

| Problem | Fix |
|---------|-----|
| White screen | Copy `User_Setup.h` to TFT_eSPI library folder |
| Colors swapped | Toggle `TFT_BGR` / `TFT_RGB` in User_Setup.h |
| No BT connection | Check speaker name is exact (case-sensitive), ensure speaker is in pairing mode and not connected to another device |
| Songs skip instantly | Files are AAC, not MP3 — re-encode with ffmpeg |
| Compilation error about partition | Set Partition Scheme to "Huge APP (3MB No OTA)" |
| Touch not working | The CYD touch is on HSPI — this is handled in code, but check your CYD variant matches the pin definitions |

## Touch Calibration

If touch coordinates feel off, adjust these values in the sketch:

```cpp
#define TS_MINX 300
#define TS_MAXX 3900
#define TS_MINY 300
#define TS_MAXY 3900
```

## Architecture

```
SD Card (MP3/WAV)
    ↓
ESP8266Audio Decoder (MP3/WAV → PCM)
    ↓
RingBufOutput::ConsumeSample()
    ↓
Ring Buffer (4096 frames, 16KB)  ←→  Visualizer tap (64 samples)
    ↓
BT A2DP Callback (pulls frames)
    ↓
Bluetooth Speaker
```

The decoder runs on the main core in `loop()`. The Bluetooth A2DP stack runs on core 0 in its own FreeRTOS task and pulls PCM frames from the ring buffer via callback.

## License

MIT
