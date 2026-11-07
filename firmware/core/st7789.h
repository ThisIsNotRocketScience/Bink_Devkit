#pragma once
// The 1.14 inch ST7789 panel, landscape: a framebuffer, and st7789_display to bring the panel up to date with it.
#include <stdint.h>

constexpr int kLcdW = 240, kLcdH = 135;

#define RGB(R, G, B) ((((R) >> 3) << 11) | (((G) >> 2) << 5) | ((B) >> 3))

struct St7789Traffic {
  uint32_t frames, rects, pixels; // totals since boot
};

void st7789_begin();
// Sends what changed since the last call, as a short list of tight rectangles.
void st7789_display();
const St7789Traffic &st7789_traffic();
void st7789_forget_panel(); // the next frame goes out whole

void st7789_clear_screen(uint16_t hwColor);
void st7789_pixel(int x, int y, uint16_t color);
uint16_t st7789_get_pixel(int x, int y);
void draw_filled_rectangle(int x, int y, int w, int h, uint16_t color);
