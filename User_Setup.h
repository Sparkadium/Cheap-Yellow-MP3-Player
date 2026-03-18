// ============================================================
// User_Setup.h — TFT_eSPI config for ESP32-2432S028 (CYD)
// ============================================================
// Copy this file to your TFT_eSPI library folder, replacing
// the existing User_Setup.h:
//
//   Arduino/libraries/TFT_eSPI/User_Setup.h
//
// Or on Windows:
//   Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
// ============================================================

// ── Display driver ──
#define ILI9341_DRIVER

// ── Display size ──
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ── ESP32-2432S028 (CYD) TFT pin mapping ──
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1  // connected to EN (reset)
#define TFT_BL   21  // backlight (set HIGH to enable)

// ── SPI frequency ──
#define SPI_FREQUENCY       40000000  // 40 MHz
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ── Color order ──
// Some CYD batches use BGR, some use RGB.
// If your colors look wrong (red↔blue swapped), 
// comment this line out or switch to TFT_RGB_ORDER.
#define TFT_RGB_ORDER TFT_BGR

// ── Font ──
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
