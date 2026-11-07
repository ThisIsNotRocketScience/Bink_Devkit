// The ST7789's bus: bit-banged SPI, panel init, window setup. The pixels are in core/st7789.cpp.
#include "DevKitHal.h"
#include "board_pins.h"
#include "pico/stdlib.h"
#include <hardware/gpio.h>
#include <stdint.h>

// The panel's 135 x 240 pixels sit inside the controller's RAM at this offset, in landscape.
static constexpr uint16_t kOffsetX = 40, kOffsetY = 53;

static constexpr uint8_t kMadctlMX = 0x40, kMadctlMV = 0x20; // landscape

static inline __attribute__((always_inline)) void shift_out(uint8_t dat) {
  for (int bit = 7; bit >= 0; bit--) {
    gpio_put(DISP_SDA, (dat >> bit) & 1);
    gpio_put(DISP_SCL, false);
    gpio_put(DISP_SCL, true);
  }
}

static void __attribute__((noinline)) spi_transfer(uint8_t dat) {
  gpio_put(DISP_CS, false);
  shift_out(dat);
  gpio_put(DISP_CS, true);
}

// Bytes onto the bus; the caller holds CS for as long as the transfer lasts.
static void spi_stream(const uint8_t *data, int count) {
  for (int i = 0; i < count; i++)
    shift_out(*data++);
}

static void command(uint8_t dat) {
  gpio_put(DISP_DC, false);
  spi_transfer(dat);
}

static void data(uint8_t dat) {
  gpio_put(DISP_DC, true);
  spi_transfer(dat);
}

static void set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t x1 = x + kOffsetX;
  uint16_t y1 = y + kOffsetY;

  command(0x2A); // CASET
  data(x1 >> 8);
  data(x1 & 0xFF);
  data((x1 + w - 1) >> 8);
  data((x1 + w - 1) & 0xFF);

  command(0x2B); // RASET
  data(y1 >> 8);
  data(y1 & 0xFF);
  data((y1 + h - 1) >> 8);
  data((y1 + h - 1) & 0xFF);

  command(0x2C); // RAMWR
}

// Panel settings as in the vendor's example for this module:
// https://www.buydisplay.com/1-14-inch-tft-ips-lcd-display-module-135x240-spi-for-arduino-raspberry-pi
void hal_display_init() {
  // DISP_BLK is left alone: SetupBacklightPWM has it, and gpio_init would take it back from the PWM.
  gpio_init(DISP_CS);
  gpio_set_dir(DISP_CS, true);
  gpio_init(DISP_DC);
  gpio_set_dir(DISP_DC, true);
  gpio_init(DISP_RES);
  gpio_set_dir(DISP_RES, true);

  gpio_init(DISP_SDA);
  gpio_set_dir(DISP_SDA, true);
  gpio_init(DISP_SCL);
  gpio_set_dir(DISP_SCL, true);

  gpio_put(DISP_RES, true);
  sleep_ms(10);
  gpio_put(DISP_RES, false);
  sleep_ms(10);
  gpio_put(DISP_RES, true);

  // --- Software Reset ---
  command(0x01); // SWRESET
  sleep_ms(120);

  // --- Sleep Out ---
  command(0x11); // SLPOUT
  sleep_ms(120);

  // --- Memory Access Control ---
  command(0x36); // MADCTL
  data(0x00);    // Portrait, RGB order (use 0x08 for BGR)

  // --- Interface Pixel Format ---
  command(0x3A); // COLMOD
  data(0x05);    // 16-bit/pixel (RGB565)

  // --- Porch Setting ---
  command(0xB2); // PORCTRL
  data(0x0C);
  data(0x0C);
  data(0x00);
  data(0x33);
  data(0x33);

  // --- Gate Control ---
  command(0xB7); // GCTRL
  data(0x35);

  // --- VCOM Setting ---
  command(0xBB); // VCOMS
  data(0x19);

  // --- LCM Control ---
  command(0xC0); // LCMCTRL
  data(0x2C);

  // --- VDV and VRH Command Enable ---
  command(0xC2); // VDVVRHEN
  data(0x01);

  // --- VRH Set ---
  command(0xC3);
  data(0x12);

  // --- VDV Set ---
  command(0xC4);
  data(0x20);

  // --- Frame Rate Control ---
  command(0xC6); // FRCTRL2
  data(0x0F);    // 60Hz

  // --- Power Control ---
  command(0xD0); // PWCTRL1
  data(0xA4);
  data(0xA1);

  // --- Positive Voltage Gamma Control ---
  command(0xE0);
  data(0xD0);
  data(0x08);
  data(0x11);
  data(0x08);
  data(0x0C);
  data(0x15);
  data(0x39);
  data(0x33);
  data(0x50);
  data(0x36);
  data(0x13);
  data(0x14);
  data(0x29);
  data(0x2D);

  // --- Negative Voltage Gamma Control ---
  command(0xE1);
  data(0xD0);
  data(0x08);
  data(0x10);
  data(0x08);
  data(0x06);
  data(0x06);
  data(0x39);
  data(0x44);
  data(0x51);
  data(0x0B);
  data(0x16);
  data(0x14);
  data(0x2F);
  data(0x31);

  // --- Display Inversion ON ---
  command(0x21);

  // --- Normal Display Mode ON ---
  command(0x13);

  // --- Display ON ---
  command(0x29);
  sleep_ms(20);

  command(0x36); // MADCTL
  data(kMadctlMV | kMadctlMX);
}


void hal_display_blit(int x, int y, int w, int h, const uint8_t *pixels, int strideBytes) {
  set_window(x, y, w, h);
  gpio_put(DISP_DC, true);
  gpio_put(DISP_CS, false);
  for (int row = 0; row < h; row++)
    spi_stream(pixels + row * strideBytes, w * 2);
  gpio_put(DISP_CS, true);
}

void hal_display_present() {}
