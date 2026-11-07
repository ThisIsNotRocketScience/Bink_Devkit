#pragma once
// Drawing for the LCD's UI, on top of the framebuffer in st7789.cpp: anti-aliased text and rounded shapes.
#include <stdint.h>

struct UiGlyph {
  uint32_t offset;  // into UiFont::bits, 4 bits a pixel, rows packed
  uint8_t w, h;
  int8_t x, y;      // from the pen, y from the top of the line
  uint8_t advance;
};

struct UiFont {
  const UiGlyph *glyphs;
  const uint8_t *bits;
  const char *chars; // the characters glyphs[] holds, in order
  int count;
  int height;
};

uint16_t ui_blend(uint16_t background, uint16_t color, int alpha); // alpha 0..255

int ui_text_width(const char *text, const UiFont &font);
// x, y: top left of the line. Returns the x the text ended at.
int ui_text(int x, int y, const char *text, const UiFont &font, uint16_t color);
void ui_text_right(int right, int y, const char *text, const UiFont &font, uint16_t color);
void ui_text_center(int center, int y, const char *text, const UiFont &font, uint16_t color);

void ui_fill(int x, int y, int w, int h, uint16_t color);
void ui_fill_round(int x, int y, int w, int h, int radius, uint16_t color);
// 0..1 of a slim rounded track.
void ui_bar(int x, int y, int w, int h, float value, uint16_t track, uint16_t color);
// The same from the middle, for -1..1 values stored as 0..1.
void ui_bar_centered(int x, int y, int w, int h, float value, uint16_t track, uint16_t color);
void ui_dot(int cx, int cy, int radius, uint16_t color);
