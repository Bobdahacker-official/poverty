#include "font.h"
extern const uint8_t font_start[], font_end[];
static uint8_t atlas[95][FONT_WIDTH * FONT_HEIGHT];
static uint8_t large_atlas[95][27 * 30];
static unsigned metric_units[95];
static int raster_size = 16, raster_width = FONT_WIDTH,
           raster_height = FONT_HEIGHT;
static unsigned advances[95];
static uint32_t glyf, loca, cmap;
static unsigned units, glyphs;
static bool long_offsets, ready;
struct point {
  int x, y;
  bool on;
};
struct edge {
  int x1, y1, x2, y2;
};
static struct point points[2048], contour[4096];
static uint8_t flags[2048];
static struct edge edges[16384];
static unsigned edge_count;
static uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static int signed16(const uint8_t *p) { return (int16_t)read16(p); }
static uint32_t read32(const uint8_t *p) {
  return (uint32_t)read16(p) << 16 | read16(p + 2);
}
static bool range(uint64_t offset, uint64_t size) {
  return offset + size >= offset &&
         offset + size <= (uint64_t)(font_end - font_start);
}
static uint32_t table(const char *tag) {
  unsigned n = read16(font_start + 4);
  if (!range(12, (uint64_t)n * 16))
    return 0;
  for (unsigned i = 0; i < n; i++) {
    const uint8_t *p = font_start + 12 + i * 16;
    if (p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3]) {
      uint32_t offset = read32(p + 8), size = read32(p + 12);
      return range(offset, size) ? offset : 0;
    }
  }
  return 0;
}
static unsigned glyph_for(unsigned code) {
  const uint8_t *p = font_start + cmap;
  unsigned segments = read16(p + 6) / 2;
  unsigned start = 14 + segments * 2 + 2, delta = start + segments * 2,
           offset = delta + segments * 2;
  for (unsigned i = 0; i < segments; i++)
    if (code <= read16(p + 14 + i * 2)) {
      unsigned low = read16(p + start + i * 2);
      if (code < low)
        return 0;
      unsigned distance = read16(p + offset + i * 2);
      if (!distance)
        return (code + signed16(p + delta + i * 2)) & 65535;
      uint64_t at =
          (uint64_t)cmap + offset + i * 2 + distance + (code - low) * 2;
      if (!range(at, 2))
        return 0;
      unsigned glyph = read16(font_start + at);
      return glyph ? (glyph + signed16(p + delta + i * 2)) & 65535 : 0;
    }
  return 0;
}
static bool line(struct point a, struct point b) {
  if (edge_count == sizeof(edges) / sizeof(edges[0]))
    return false;
  edges[edge_count++] =
      (struct edge){a.x * raster_size * 64 / (int)units + raster_size * 2,
                    56 * raster_size - a.y * raster_size * 64 / (int)units,
                    b.x * raster_size * 64 / (int)units + raster_size * 2,
                    56 * raster_size - b.y * raster_size * 64 / (int)units};
  return true;
}
static bool outline(unsigned glyph) {
  if (glyph >= glyphs)
    return false;
  uint32_t first = long_offsets ? read32(font_start + loca + glyph * 4)
                                : read16(font_start + loca + glyph * 2) * 2;
  uint32_t end = long_offsets ? read32(font_start + loca + (glyph + 1) * 4)
                              : read16(font_start + loca + (glyph + 1) * 2) * 2;
  if (first == end)
    return true;
  if (end < first || !range((uint64_t)glyf + first, end - first) ||
      end - first < 10)
    return false;
  const uint8_t *p = font_start + glyf + first,
                *limit = font_start + glyf + end;
  int contours = signed16(p);
  if (contours < 0 || contours > 128 ||
      (unsigned)(10 + contours * 2 + 2) > end - first)
    return false;
  uint16_t ends[128];
  for (int i = 0; i < contours; i++) {
    ends[i] = read16(p + 10 + i * 2);
    if (i && ends[i] <= ends[i - 1])
      return false;
  }
  if (!contours)
    return true;
  unsigned count = ends[contours - 1] + 1;
  if (count > 2048)
    return false;
  p += 10 + contours * 2;
  unsigned instructions = read16(p);
  p += 2;
  if ((unsigned)(limit - p) < instructions)
    return false;
  p += instructions;
  for (unsigned i = 0; i < count;) {
    if (p == limit)
      return false;
    uint8_t flag = *p++;
    flags[i++] = flag;
    if (flag & 8) {
      if (p == limit)
        return false;
      unsigned repeat = *p++;
      if (repeat > count - i)
        return false;
      while (repeat--)
        flags[i++] = flag;
    }
  }
  for (unsigned axis = 0; axis < 2; axis++) {
    int coordinate = 0;
    for (unsigned i = 0; i < count; i++) {
      unsigned short_flag = axis ? 4 : 2, same_flag = axis ? 32 : 16;
      int difference = 0;
      if (flags[i] & short_flag) {
        if (p == limit)
          return false;
        difference = *p++;
        if (!(flags[i] & same_flag))
          difference = -difference;
      } else if (!(flags[i] & same_flag)) {
        if (limit - p < 2)
          return false;
        difference = signed16(p);
        p += 2;
      }
      coordinate += difference;
      if (axis)
        points[i].y = coordinate;
      else
        points[i].x = coordinate;
      points[i].on = flags[i] & 1;
    }
  }
  unsigned first_point = 0;
  for (int c = 0; c < contours; c++) {
    unsigned n = 0, last = ends[c];
    for (unsigned i = first_point; i <= last; i++) {
      struct point a = points[i], b = points[i == last ? first_point : i + 1];
      contour[n++] = a;
      if (!a.on && !b.on)
        contour[n++] = (struct point){(a.x + b.x) / 2, (a.y + b.y) / 2, true};
    }
    unsigned start = 0;
    while (start < n && !contour[start].on)
      start++;
    if (start == n)
      return false;
    unsigned walked = 0;
    struct point a = contour[start];
    while (walked < n) {
      struct point b = contour[(start + walked + 1) % n];
      if (b.on) {
        if (!line(a, b))
          return false;
        a = b;
        walked++;
      } else {
        struct point end_point = contour[(start + walked + 2) % n],
                     previous = a;
        if (!end_point.on)
          return false;
        for (int step = 1; step <= 8; step++) {
          int inverse = 8 - step;
          struct point next = {
              (inverse * inverse * a.x + 2 * inverse * step * b.x +
               step * step * end_point.x) /
                  64,
              (inverse * inverse * a.y + 2 * inverse * step * b.y +
               step * step * end_point.y) /
                  64,
              true};
          if (!line(previous, next))
            return false;
          previous = next;
        }
        a = end_point;
        walked += 2;
      }
    }
    first_point = last + 1;
  }
  return true;
}
static void raster(uint8_t *pixels) {
  memset(pixels, 0, raster_width * raster_height);
  for (int y = 0; y < raster_height; y++)
    for (int sample_y = 0; sample_y < 4; sample_y++) {
      int scan = y * 64 + sample_y * 16 + 8;
      for (int x = 0; x < raster_width; x++)
        for (int sample_x = 0; sample_x < 4; sample_x++) {
          int scan_x = x * 64 + sample_x * 16 + 8, winding = 0;
          for (unsigned i = 0; i < edge_count; i++) {
            struct edge e = edges[i];
            if ((e.y1 <= scan && e.y2 > scan) ||
                (e.y2 <= scan && e.y1 > scan)) {
              int crossing =
                  e.x1 + (int64_t)(scan - e.y1) * (e.x2 - e.x1) / (e.y2 - e.y1);
              if (crossing > scan_x)
                winding += e.y2 > e.y1 ? 1 : -1;
            }
          }
          if (winding)
            pixels[y * raster_width + x]++;
        }
    }
  for (int i = 0; i < raster_width * raster_height; i++)
    pixels[i] = pixels[i] * 255 / 16;
}
bool font_init(void) {
  if (ready)
    return true;
  raster_size = 16;
  raster_width = FONT_WIDTH;
  raster_height = FONT_HEIGHT;
  if (!range(0, 12) || read32(font_start) != 0x10000)
    return false;
  uint32_t head = table("head"), maxp = table("maxp"), mapping = table("cmap");
  uint32_t hhea = table("hhea"), metrics = table("hmtx");
  if (!hhea || !metrics || !range(hhea, 36))
    return false;
  unsigned metric_count = read16(font_start + hhea + 34);
  if (!metric_count || !range(metrics, (uint64_t)metric_count * 4))
    return false;
  glyf = table("glyf");
  loca = table("loca");
  if (!head || !maxp || !mapping || !glyf || !loca || !range(head, 54) ||
      !range(maxp, 6) || !range(mapping, 4))
    return false;
  units = read16(font_start + head + 18);
  glyphs = read16(font_start + maxp + 4);
  long_offsets = signed16(font_start + head + 50) == 1;
  if (!units || !glyphs ||
      !range(loca, (uint64_t)(glyphs + 1) * (long_offsets ? 4 : 2)))
    return false;
  unsigned maps = read16(font_start + mapping + 2);
  if (!range(mapping + 4, (uint64_t)maps * 8))
    return false;
  for (unsigned i = 0; i < maps; i++) {
    const uint8_t *entry = font_start + mapping + 4 + i * 8;
    uint64_t offset = (uint64_t)mapping + read32(entry + 4);
    if (range(offset, 14) && read16(font_start + offset) == 4) {
      unsigned length = read16(font_start + offset + 2),
               segments = read16(font_start + offset + 6) / 2;
      if (segments && length >= 16 + segments * 8 && range(offset, length)) {
        cmap = offset;
        break;
      }
    }
  }
  if (!cmap)
    return false;
  for (unsigned c = 32; c < 127; c++) {
    unsigned glyph = glyph_for(c);
    unsigned metric = glyph < metric_count ? glyph : metric_count - 1;
    metric_units[c - 32] = read16(font_start + metrics + metric * 4);
    advances[c - 32] =
        (read16(font_start + metrics + metric * 4) * 16 + units / 2) / units;
    if (!advances[c - 32])
      advances[c - 32] = 1;
    edge_count = 0;
    if (!outline(glyph))
      return false;
    raster(atlas[c - 32]);
  }
  raster_size = 24;
  raster_width = 27;
  raster_height = 30;
  for (unsigned c = 32; c < 127; c++) {
    edge_count = 0;
    if (!outline(glyph_for(c)))
      return false;
    raster(large_atlas[c - 32]);
  }
  ready = true;
  return true;
}
const uint8_t *font_character(char c) {
  return ready && c >= 32 && c < 127 ? atlas[(unsigned)c - 32] : NULL;
}
unsigned font_advance(char c) {
  return ready && c >= 32 && c < 127 ? advances[(unsigned)c - 32] : 8;
}
const uint8_t *font_scaled(char c, unsigned size, unsigned *width,
                           unsigned *height) {
  if (size >= 20) {
    *width = 27;
    *height = 30;
    return ready && c >= 32 && c < 127 ? large_atlas[(unsigned)c - 32] : NULL;
  }
  *width = FONT_WIDTH;
  *height = FONT_HEIGHT;
  return font_character(c);
}
unsigned font_scaled_advance(char c, unsigned size) {
  if (!ready || c < 32 || c >= 127)
    return size / 2;
  if (size == 16)
    return advances[(unsigned)c - 32];
  return (metric_units[(unsigned)c - 32] * size + units / 2) / units;
}
