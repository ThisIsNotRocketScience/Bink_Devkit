// The LCD's framebuffer and the update rectangles. The bus itself is behind hal_display_blit.
#include "DevKitHal.h"
#include "st7789.h"
#include <string.h>

static uint8_t buffer[kLcdW * kLcdH * 2]; // RGB565, high byte first

void st7789_pixel(int x, int y, uint16_t hwColor) {
  if (x >= kLcdW || y >= kLcdH || x < 0 || y < 0)
    return;
  int idx = x * 2 + y * kLcdW * 2;
  buffer[idx++] = hwColor >> 8;
  buffer[idx] = hwColor;
}

// ── Update rectangles ────────────────────────────────────────────────────────
// The bus is the slow part: all of the screen takes 10 ms, a changed number
// well under one. The pages redraw themselves every frame into `buffer`; what
// the panel already shows is kept in `shown`, and only the difference goes out.
//
//   1. Compare per tile, and join changed tiles into rectangles: runs along a
//      row, then equal runs of the rows below. A short list: once it is full
//      the last rectangle takes in the rest.
//   2. Trim each rectangle to the pixels that really changed.
//   3. Send them straight out of the buffer and update `shown`.
//
// An unchanged frame costs the compare and sends nothing.
namespace {
constexpr int kTileW = 16, kTileH = 15;
constexpr int kTilesX = kLcdW / kTileW, kTilesY = kLcdH / kTileH;
constexpr int kStride = kLcdW * 2;
constexpr int kMaxRects = 16;

struct DirtyRect {
  int x0, y0, x1, y1; // pixels, x1 / y1 exclusive
};

uint8_t shown[sizeof(buffer)];
bool panelKnown = false; // its RAM holds noise until the first full frame
St7789Traffic traffic = {};

bool rowsDiffer(int x0, int x1, int y) {
  return memcmp(&buffer[y * kStride + x0 * 2], &shown[y * kStride + x0 * 2], (x1 - x0) * 2) != 0;
}

bool columnDiffers(int x, int y0, int y1) {
  for (int y = y0; y < y1; y++)
    if (rowsDiffer(x, x + 1, y))
      return true;
  return false;
}

bool tileChanged(int tx, int ty) {
  for (int y = ty * kTileH; y < (ty + 1) * kTileH; y++)
    if (rowsDiffer(tx * kTileW, (tx + 1) * kTileW, y))
      return true;
  return false;
}

void addRun(DirtyRect *rects, int &count, int x0, int x1, int y0, int y1) {
  for (int i = 0; i < count; i++)
    if (rects[i].x0 == x0 && rects[i].x1 == x1 && rects[i].y1 == y0) {
      rects[i].y1 = y1;
      return;
    }
  if (count < kMaxRects) {
    rects[count++] = {x0, y0, x1, y1};
    return;
  }
  DirtyRect &last = rects[count - 1];
  last = {x0 < last.x0 ? x0 : last.x0, y0 < last.y0 ? y0 : last.y0, x1 > last.x1 ? x1 : last.x1, y1 > last.y1 ? y1 : last.y1};
}

void trim(DirtyRect &r) {
  while (r.y0 < r.y1 && !rowsDiffer(r.x0, r.x1, r.y0))
    r.y0++;
  while (r.y0 < r.y1 && !rowsDiffer(r.x0, r.x1, r.y1 - 1))
    r.y1--;
  while (r.x0 < r.x1 && !columnDiffers(r.x0, r.y0, r.y1))
    r.x0++;
  while (r.x0 < r.x1 && !columnDiffers(r.x1 - 1, r.y0, r.y1))
    r.x1--;
}

void sendRect(const DirtyRect &r) {
  int w = r.x1 - r.x0, h = r.y1 - r.y0;
  hal_display_blit(r.x0, r.y0, w, h, &buffer[r.y0 * kStride + r.x0 * 2], kStride);
  for (int y = r.y0; y < r.y1; y++)
    memcpy(&shown[y * kStride + r.x0 * 2], &buffer[y * kStride + r.x0 * 2], w * 2);
  traffic.rects++;
  traffic.pixels += w * h;
}
} // namespace

const St7789Traffic &st7789_traffic() { return traffic; }
void st7789_forget_panel() { panelKnown = false; }

void st7789_display() {
  traffic.frames++;
  if (!panelKnown) {
    panelKnown = true;
    sendRect({0, 0, kLcdW, kLcdH});
    hal_display_present();
    return;
  }
  DirtyRect rects[kMaxRects];
  int count = 0;
  for (int ty = 0; ty < kTilesY; ty++) {
    for (int tx = 0; tx < kTilesX; tx++) {
      if (!tileChanged(tx, ty))
        continue;
      int end = tx + 1;
      while (end < kTilesX && tileChanged(end, ty))
        end++;
      addRun(rects, count, tx * kTileW, end * kTileW, ty * kTileH, (ty + 1) * kTileH);
      tx = end;
    }
  }
  for (int i = 0; i < count; i++) {
    trim(rects[i]);
    if (rects[i].x0 < rects[i].x1 && rects[i].y0 < rects[i].y1)
      sendRect(rects[i]);
  }
  hal_display_present();
}

void st7789_clear_screen(uint16_t hwColor) {
  uint16_t *pbuf = (uint16_t *)buffer;
  const uint16_t swapped = (uint16_t)((hwColor << 8) | (hwColor >> 8)); // the buffer is high byte first
  for (int i = 0; i < kLcdW * kLcdH; i++)
    pbuf[i] = swapped;
}

void draw_filled_rectangle(int x, int y, int w, int h, uint16_t color) {
  for (int i = x; i < x + w; i++)
    for (int j = y; j < y + h; j++)
      st7789_pixel(i, j, color);
}

uint16_t st7789_get_pixel(int x, int y) {
  if (x >= kLcdW || y >= kLcdH || x < 0 || y < 0)
    return 0;
  int idx = x * 2 + y * kLcdW * 2;
  return (buffer[idx] << 8) | buffer[idx + 1];
}

void st7789_begin() {
  hal_display_init();
  memset(buffer, 0, sizeof(buffer));
}
