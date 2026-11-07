#include "UiGfx.h"

#include "st7789.h"

#include <math.h>
#include <string.h>

uint16_t ui_blend(uint16_t background, uint16_t color, int alpha) {
  int r0 = background >> 11, g0 = (background >> 5) & 63, b0 = background & 31;
  int r1 = color >> 11, g1 = (color >> 5) & 63, b1 = color & 31;
  int r = r0 + (((r1 - r0) * alpha) >> 8), g = g0 + (((g1 - g0) * alpha) >> 8), b = b0 + (((b1 - b0) * alpha) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void blendPixel(int x, int y, uint16_t color, int alpha) {
  if (x < 0 || y < 0 || alpha <= 0)
    return;
  st7789_pixel(x, y, alpha >= 255 ? color : ui_blend(st7789_get_pixel(x, y), color, alpha));
}

static const UiGlyph *findGlyph(char c, const UiFont &font) {
  const char *hit = strchr(font.chars, c);
  return c && hit ? &font.glyphs[hit - font.chars] : nullptr;
}

int ui_text_width(const char *text, const UiFont &font) {
  int w = 0;
  for (; *text; text++)
    if (const UiGlyph *g = findGlyph(*text, font))
      w += g->advance;
  return w;
}

int ui_text(int x, int y, const char *text, const UiFont &font, uint16_t color) {
  for (; *text; text++) {
    const UiGlyph *g = findGlyph(*text, font);
    if (!g)
      continue;
    const uint8_t *bits = font.bits + g->offset;
    for (int i = 0; i < g->w * g->h; i++) {
      int a = (i & 1) ? bits[i >> 1] & 15 : bits[i >> 1] >> 4;
      if (a)
        blendPixel(x + g->x + i % g->w, y + g->y + i / g->w, color, a * 17);
    }
    x += g->advance;
  }
  return x;
}

void ui_text_right(int right, int y, const char *text, const UiFont &font, uint16_t color) {
  ui_text(right - ui_text_width(text, font), y, text, font, color);
}

void ui_text_center(int center, int y, const char *text, const UiFont &font, uint16_t color) {
  ui_text(center - ui_text_width(text, font) / 2, y, text, font, color);
}

void ui_fill(int x, int y, int w, int h, uint16_t color) {
  if (w > 0 && h > 0)
    draw_filled_rectangle(x, y, w, h, color);
}

void ui_fill_round(int x, int y, int w, int h, int radius, uint16_t color) {
  if (w <= 0 || h <= 0)
    return;
  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;
  ui_fill(x, y + radius, w, h - 2 * radius, color);
  ui_fill(x + radius, y, w - 2 * radius, radius, color);
  ui_fill(x + radius, y + h - radius, w - 2 * radius, radius, color);
  for (int j = 0; j < radius; j++)
    for (int i = 0; i < radius; i++) {
      // Coverage of this corner pixel by the circle, from its distance to the centre.
      float dx = radius - i - 0.5f, dy = radius - j - 0.5f;
      float cover = radius + 0.5f - sqrtf(dx * dx + dy * dy);
      int a = cover <= 0 ? 0 : (cover >= 1 ? 255 : (int)(cover * 255));
      blendPixel(x + i, y + j, color, a);
      blendPixel(x + w - 1 - i, y + j, color, a);
      blendPixel(x + i, y + h - 1 - j, color, a);
      blendPixel(x + w - 1 - i, y + h - 1 - j, color, a);
    }
}

void ui_bar(int x, int y, int w, int h, float value, uint16_t track, uint16_t color) {
  ui_fill_round(x, y, w, h, h / 2, track);
  int fill = (int)(value * w + 0.5f);
  if (fill > 0)
    ui_fill_round(x, y, fill < h ? h : fill, h, h / 2, color);
}

void ui_bar_centered(int x, int y, int w, int h, float value, uint16_t track, uint16_t color) {
  ui_fill_round(x, y, w, h, h / 2, track);
  int mid = x + w / 2, end = x + (int)(value * w + 0.5f);
  int from = end < mid ? end : mid, to = end < mid ? mid : end;
  ui_fill_round(from, y, to - from < h ? h : to - from, h, h / 2, color);
  ui_fill(mid, y - 1, 1, h + 2, color);
}

void ui_dot(int cx, int cy, int radius, uint16_t color) {
  ui_fill_round(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, radius + 1, color);
}
