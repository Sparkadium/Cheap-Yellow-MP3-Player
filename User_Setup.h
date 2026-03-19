// ============================================================
// User_Setup.h — TFT_eSPI config for ALL ESP32-2432S028 (CYD)
// ============================================================
//
// INSTALL: Copy this file to your TFT_eSPI library folder,
//          replacing the existing User_Setup.h:
//
//   Arduino/libraries/TFT_eSPI/User_Setup.h
//
//   Windows: Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
//   macOS:   ~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
//   Linux:   ~/Arduino/libraries/TFT_eSPI/User_Setup.h
//
// ============================================================
//
// WHICH CYD DO YOU HAVE?
//
// There are 3 known display driver variants of the CYD board.
// The pins are all the same — only the driver chip differs.
// Uncomment ONE driver line below that matches your board.
//
// ┌──────────────────────────────────────────────────────────┐
// │ Board variant         │ USB ports     │ Driver to use    │
// ├──────────────────────────────────────────────────────────┤
// │ v1 (original)         │ 1× Micro-USB  │ ILI9341_DRIVER   │
// │ v1 (alt. controller)  │ 1× Micro-USB  │ ILI9341_2_DRIVER │
// │ v2 / v3 (newer)       │ USB-C + Micro │ ST7789_2_DRIVER  │
// └──────────────────────────────────────────────────────────┘
//
// HOW TO TELL:
//   - Count your USB ports. Two ports (USB-C + Micro) = ST7789.
//   - One Micro-USB port = try ILI9341_DRIVER first.
//   - If you get a white screen with ILI9341_DRIVER, switch
//     to ILI9341_2_DRIVER.
//
// ============================================================

// ── STEP 1: Uncomment ONE driver ────────────────────────────
// Try them in this order if your screen is white/blank:

#define ILI9341_DRIVER          // Most common — single Micro-USB CYD
//#define ILI9341_2_DRIVER      // Alt driver — some single Micro-USB CYDs
//#define ST7789_2_DRIVER       // Dual USB (USB-C + Micro-USB) CYDs

// ── STEP 2: Color order ─────────────────────────────────────
// If your reds and blues are swapped, change BGR to RGB or
// vice versa.

#define TFT_RGB_ORDER TFT_BGR  // Most CYDs use BGR
//#define TFT_RGB_ORDER TFT_RGB  // Some newer CYDs use RGB

// ── STEP 3: Invert display (ST7789 only) ────────────────────
// If using ST7789_2_DRIVER and colors look inverted/washed out,
// uncomment this line:

//#define TFT_INVERSION_ON

// ════════════════════════════════════════════════════════════
// EVERYTHING BELOW IS THE SAME FOR ALL CYD VARIANTS
// (You should not need to change anything below this line)
// ════════════════════════════════════════════════════════════

// ── Display size ──
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ── CYD TFT pin mapping (shared across all variants) ──
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1   // Connected to EN (reset)
#define TFT_BL   21   // Backlight — set HIGH to enable

// Note: The CYD touch controller (XPT2046) is on a SEPARATE
// SPI bus (HSPI) with its own pins. It is NOT configured here
// because ShuffleCYD handles it in code. For reference:
//   Touch CLK  = GPIO 25
//   Touch MISO = GPIO 39
//   Touch MOSI = GPIO 32
//   Touch CS   = GPIO 33
//   Touch IRQ  = GPIO 36

// ── SPI frequencies ──
#define SPI_FREQUENCY       40000000   // 40 MHz for display
#define SPI_READ_FREQUENCY  20000000   // 20 MHz for reads
#define SPI_TOUCH_FREQUENCY  2500000   //  2.5 MHz for touch

// ── Fonts ──
#define LOAD_GLCD    // Default Adafruit 5×7 font
#define LOAD_FONT2   // Small 16px
#define LOAD_FONT4   // Medium 26px
#define LOAD_FONT6   // Large num font
#define LOAD_FONT7   // 7-segment num font
#define LOAD_FONT8   // Large num font
#define LOAD_GFXFF   // FreeFonts
#define SMOOTH_FONT  // Anti-aliased font support
