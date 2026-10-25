#include "gui_canvas.h"
#include "font.h"
#include "system.h"
static uint32_t pixels[1024 * 768], wallpaper[1024 * 768], scratch[1024 * 768];
static unsigned width, height;
static int clip_x, clip_y, clip_right, clip_bottom;
static uint32_t opened_at[16];
static bool opened[16];
static unsigned command_alpha = 255;
static uint64_t previous_commands[32768];
static size_t previous_size;
static unsigned wallpaper_revision, previous_wallpaper;
static bool previous_glass, cache_valid, scene_repaint;
static uint32_t cursor_under[32 * 32];
static int cursor_x, cursor_y, presented_x, presented_y;
static bool cursor_drawn, cursor_moved;
static void copy_pixels(uint32_t *destination, const uint32_t *source,
                        size_t count) {
  size_t words = count / 2;
  __asm__ volatile("rep movsq"
                   : "+D"(destination), "+S"(source), "+c"(words)::"memory");
  if (count & 1)
    *destination = *source;
}
bool gui_canvas_changed(struct nk_context *ctx, bool glass) {
  nk__begin(ctx);
  const uint8_t *commands = nk_buffer_memory_const(&ctx->memory);
  size_t size = ctx->memory.allocated;
  bool changed = !cache_valid || size != previous_size ||
                 wallpaper_revision != previous_wallpaper ||
                 glass != previous_glass || size > sizeof(previous_commands);
  if (!changed) {
    const uint64_t *words = (const void *)commands;
    for (size_t i = 0; i < size / 8; i++)
      if (words[i] != previous_commands[i]) {
        changed = true;
        break;
      }
    for (size_t i = size & ~(size_t)7; !changed && i < size; i++)
      changed = commands[i] != ((const uint8_t *)previous_commands)[i];
  }
  if (size <= sizeof(previous_commands)) {
    if (changed)
      memcpy(previous_commands, commands, size);
    cache_valid = true;
  } else
    cache_valid = false;
  previous_size = size;
  previous_wallpaper = wallpaper_revision;
  previous_glass = glass;
  for (unsigned i = 1; i < 16; i++)
    if (opened[i] && (uint32_t)(clock_ticks() - opened_at[i]) <= 20)
      changed = true;
  return changed;
}
void gui_canvas_open(unsigned id) {
  if (id < 16) {
    opened[id] = true;
    opened_at[id] = clock_ticks();
  }
}
static uint32_t color(struct nk_color c) {
  return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
}
static uint32_t blend(uint32_t a, uint32_t b, unsigned alpha) {
  if (alpha == 255)
    return b;
  unsigned inverse = 256 - alpha;
  uint32_t red_blue =
      (((a & 0xff00ff) * inverse + (b & 0xff00ff) * alpha) >> 8) & 0xff00ff;
  uint32_t green =
      (((a & 0x00ff00) * inverse + (b & 0x00ff00) * alpha) >> 8) & 0x00ff00;
  return red_blue | green;
}
static void point(int x, int y, struct nk_color c) {
  if (x < clip_x || y < clip_y || x >= clip_right || y >= clip_bottom || !c.a)
    return;
  if (command_alpha != 255)
    c.a = c.a * command_alpha / 255;
  if (!c.a)
    return;
  pixels[y * width + x] = blend(pixels[y * width + x], color(c), c.a);
}
static int corner_inset(int y, int w, int h, int radius) {
  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;
  int dy = y < radius ? radius - y : y >= h - radius ? y - (h - radius - 1) : 0;
  int inset = 0;
  while ((radius - inset) * (radius - inset) + dy * dy > radius * radius)
    inset++;
  return inset;
}
static void span(int left, int right, int y, struct nk_color c) {
  if (left < clip_x)
    left = clip_x;
  if (right > clip_right)
    right = clip_right;
  if (left >= right || !c.a)
    return;
  unsigned alpha = c.a * command_alpha / 255;
  uint32_t value = color(c), *row = pixels + y * width;
  if (alpha == 255)
    for (int x = left; x < right; x++)
      row[x] = value;
  else if (alpha)
    for (int x = left; x < right; x++)
      row[x] = blend(row[x], value, alpha);
}
static void rectangle(int x, int y, int w, int h, int radius, struct nk_color c,
                      int stroke) {
  if (w <= 0 || h <= 0)
    return;
  int top = y < clip_y ? clip_y : y;
  int bottom = y + h > clip_bottom ? clip_bottom : y + h;
  for (int row = top; row < bottom; row++) {
    int inset = corner_inset(row - y, w, h, radius);
    int left = x + inset, right = x + w - inset;
    if (stroke && w > stroke * 2 && h > stroke * 2 && row >= y + stroke &&
        row < y + h - stroke) {
      int inner = corner_inset(row - y - stroke, w - stroke * 2, h - stroke * 2,
                               radius > stroke ? radius - stroke : 0);
      span(left, x + stroke + inner, row, c);
      span(x + w - stroke - inner, right, row, c);
    } else
      span(left, right, row, c);
  }
}
static void line(int x0, int y0, int x1, int y1, unsigned thickness,
                 struct nk_color c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, error = dx + dy;
  if (!thickness)
    thickness = 1;
  for (;;) {
    int radius = thickness / 2;
    for (unsigned j = 0; j < thickness; j++)
      for (unsigned i = 0; i < thickness; i++)
        point(x0 - radius + i, y0 - radius + j, c);
    if (x0 == x1 && y0 == y1)
      break;
    int twice = error * 2;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
  }
}
static void ellipse(int x, int y, int w, int h, unsigned stroke,
                    struct nk_color c) {
  if (w <= 0 || h <= 0)
    return;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      int64_t dx = i * 2 - w + 1, dy = j * 2 - h + 1;
      bool outside = dx * dx * h * h + dy * dy * w * w > (int64_t)w * w * h * h;
      if (outside)
        continue;
      if (stroke && w > (int)stroke * 2 && h > (int)stroke * 2) {
        int a = w - stroke * 2, b = h - stroke * 2;
        if (dx * dx * b * b + dy * dy * a * a < (int64_t)a * a * b * b)
          continue;
      }
      point(x + i, y + j, c);
    }
}
static void polygon(const struct nk_vec2i *points, unsigned count,
                    struct nk_color c) {
  if (count < 3 || count > 128)
    return;
  int top = points[0].y, bottom = top;
  for (unsigned i = 1; i < count; i++) {
    if (points[i].y < top)
      top = points[i].y;
    if (points[i].y > bottom)
      bottom = points[i].y;
  }
  if (top < clip_y)
    top = clip_y;
  if (bottom > clip_bottom)
    bottom = clip_bottom;
  for (int y = top; y < bottom; y++) {
    int crossings[128];
    unsigned n = 0;
    for (unsigned i = 0, j = count - 1; i < count; j = i++) {
      int y1 = points[i].y, y2 = points[j].y;
      if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y))
        crossings[n++] = points[i].x + (int64_t)(y - y1) *
                                           (points[j].x - points[i].x) /
                                           (y2 - y1);
    }
    for (unsigned i = 1; i < n; i++) {
      int value = crossings[i];
      unsigned j = i;
      while (j && crossings[j - 1] > value) {
        crossings[j] = crossings[j - 1];
        j--;
      }
      crossings[j] = value;
    }
    for (unsigned i = 0; i + 1 < n; i += 2)
      for (int x = crossings[i]; x < crossings[i + 1]; x++)
        point(x, y, c);
  }
}
static void blur(int x, int y, int w, int h) {
  if (w < 180 || h < 100)
    return;
  copy_pixels(scratch, pixels, width * height);
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) {
      if (i < 4 || j < 4 || i + 4 >= (int)width || j + 4 >= (int)height)
        continue;
      unsigned r = 0, g = 0, b = 0;
      for (int dy = -4; dy <= 4; dy += 4)
        for (int dx = -4; dx <= 4; dx += 4) {
          uint32_t sample = scratch[(j + dy) * width + i + dx];
          r += sample >> 16 & 255;
          g += sample >> 8 & 255;
          b += sample & 255;
        }
      point(i, j, nk_rgba(r / 9, g / 9, b / 9, 200));
    }
}
void gui_canvas_init(unsigned w, unsigned h) {
  width = w;
  height = h;
  clip_x = clip_y = 0;
  clip_right = width;
  clip_bottom = height;
  cache_valid = cursor_drawn = cursor_moved = false;
}
void gui_canvas_wallpaper(uint32_t accent, unsigned variant) {
  wallpaper_revision++;
  uint32_t top = variant == 1 ? 0x17181e : variant == 2 ? 0x10222d : 0x181322;
  uint32_t bottom = variant == 1   ? 0x2e3039
                    : variant == 2 ? 0x142e42
                                   : 0x30213a;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      uint32_t base = blend(top, bottom, y * 255 / height);
      int xx = (int)x - (int)width / 2, yy = (int)y - (int)height / 2;
      if (variant == 1) {
        int slab = (xx + yy / 2 + 4096) / 100;
        unsigned edge = (xx + yy / 2 + 4096) % 100;
        base = blend(base, slab & 1 ? 0x090a0f : 0x50515b, slab & 1 ? 35 : 12);
        if (edge < 2)
          base = blend(base, 0x000000, 110);
      } else {
        int u = xx + yy / 2, v = yy - xx / 4;
        int64_t q = (int64_t)u * u * 4096 / ((int64_t)width * width / 5) +
                    (int64_t)v * v * 4096 / ((int64_t)height * height / 7);
        int distance = q > 4096 ? q - 4096 : 4096 - q;
        if (distance < 2500)
          base = blend(base, accent, (2500 - distance) * 145 / 2500);
        if (distance < 380)
          base = blend(base, 0xffddf1, (380 - distance) * 140 / 380);
        int diagonal = xx - yy / 2 + (int)width / 4;
        if (diagonal < 0)
          diagonal = -diagonal;
        if (diagonal < 90)
          base = blend(base, accent, (90 - diagonal) * 45 / 90);
      }
      wallpaper[y * width + x] = base;
    }
}
void gui_canvas_begin(void) {
  command_alpha = 255;
  copy_pixels(pixels, wallpaper, width * height);
  scene_repaint = true;
  cursor_drawn = false;
  clip_x = clip_y = 0;
  clip_right = width;
  clip_bottom = height;
}
void gui_canvas_shadow(struct nk_rect r) {
  int x = r.x, y = r.y + 7, w = r.w, h = r.h;
  for (int j = y - 18; j < y + h + 18; j++)
    for (int i = x - 18; i < x + w + 18; i++) {
      if (j >= y && j < y + h && i >= x && i < x + w) {
        i = x + w - 1;
        continue;
      }
      int dx = i < x        ? x - i
               : i >= x + w ? i - x - w + 1
                            : 0,
          dy = j < y        ? y - j
               : j >= y + h ? j - y - h + 1
                            : 0;
      int distance = dx > dy ? dx + dy / 2 : dy + dx / 2;
      if (distance < 18)
        point(i, j, nk_rgba(0, 0, 0, (18 - distance) * (18 - distance) / 10));
    }
}
float gui_font_width(nk_handle data, float height, const char *string,
                     int length) {
  (void)data;
  unsigned total = 0;
  for (int i = 0; i < length; i++)
    total += font_scaled_advance((unsigned char)string[i] >= 32 &&
                                         (unsigned char)string[i] < 127
                                     ? string[i]
                                     : '?',
                                 (unsigned)height);
  return total;
}
static void text(const struct nk_command_text *t) {
  int x = t->x, y = t->y;
  unsigned scaled = t->height > 0 ? (unsigned)t->height : 16;
  if (scaled > 64)
    scaled = 64;
  for (int i = 0; i < t->length; i++) {
    char c = t->string[i];
    if ((unsigned char)c < 32 || (unsigned char)c >= 127)
      c = '?';
    unsigned glyph_width, glyph_height, base = scaled >= 20 ? 24 : 16;
    const uint8_t *glyph = font_scaled(c, scaled, &glyph_width, &glyph_height);
    if (glyph && scaled == base) {
      for (unsigned row = 0; row < glyph_height; row++)
        for (unsigned col = 0; col < glyph_width; col++) {
          unsigned alpha = glyph[row * glyph_width + col];
          if (alpha) {
            struct nk_color color = t->foreground;
            color.a = color.a * alpha / 255;
            point(x + col, y + row, color);
          }
        }
    } else if (glyph)
      for (unsigned row = 0; row < glyph_height * scaled / base; row++)
        for (unsigned col = 0; col < glyph_width * scaled / base; col++) {
          unsigned alpha =
              glyph[(row * base / scaled) * glyph_width + col * base / scaled];
          if (alpha) {
            struct nk_color color = t->foreground;
            color.a = color.a * alpha / 255;
            point(x + col, y + row, color);
          }
        }
    x += font_scaled_advance(c, scaled);
  }
}
static float sine(float x) {
  while (x > 3.14159265f)
    x -= 6.2831853f;
  while (x < -3.14159265f)
    x += 6.2831853f;
  float squared = x * x;
  return x * (1.0f - squared / 6.0f + squared * squared / 120.0f -
              squared * squared * squared / 5040.0f);
}
void gui_canvas_commands(struct nk_context *ctx, bool glass) {
  const struct nk_command *command;
  bool blurred = false;
  uint32_t ticks = clock_ticks();
  nk_foreach(command, ctx) {
    unsigned id = command->userdata.id;
    uint32_t elapsed = id < 16 && opened[id] ? ticks - opened_at[id] : 18;
    command_alpha = id && elapsed < 18 ? elapsed * 255 / 18 : 255;
    switch (command->type) {
    case NK_COMMAND_NOP:
      break;
    case NK_COMMAND_SCISSOR: {
      const struct nk_command_scissor *s = (const void *)command;
      clip_x = s->x < 0 ? 0 : s->x;
      clip_y = s->y < 0 ? 0 : s->y;
      clip_right = s->x + s->w > (int)width ? (int)width : s->x + s->w;
      clip_bottom = s->y + s->h > (int)height ? (int)height : s->y + s->h;
      break;
    }
    case NK_COMMAND_LINE: {
      const struct nk_command_line *s = (const void *)command;
      line(s->begin.x, s->begin.y, s->end.x, s->end.y, s->line_thickness,
           s->color);
      break;
    }
    case NK_COMMAND_RECT: {
      const struct nk_command_rect *s = (const void *)command;
      rectangle(s->x, s->y, s->w, s->h, s->rounding, s->color,
                s->line_thickness);
      break;
    }
    case NK_COMMAND_RECT_FILLED: {
      const struct nk_command_rect_filled *s = (const void *)command;
      if (glass && !blurred && s->color.a < 255 && s->w > 350 && s->h > 180) {
        blur(s->x, s->y, s->w, s->h);
        blurred = true;
      }
      rectangle(s->x, s->y, s->w, s->h, s->rounding, s->color, 0);
      break;
    }
    case NK_COMMAND_RECT_MULTI_COLOR: {
      const struct nk_command_rect_multi_color *s = (const void *)command;
      for (unsigned j = 0; j < s->h; j++)
        for (unsigned i = 0; i < s->w; i++) {
          uint32_t top =
              blend(color(s->left), color(s->top), s->w ? i * 255 / s->w : 0);
          uint32_t bottom = blend(color(s->bottom), color(s->right),
                                  s->w ? i * 255 / s->w : 0);
          uint32_t rgb = blend(top, bottom, s->h ? j * 255 / s->h : 0);
          point(s->x + i, s->y + j,
                nk_rgb((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
        }
      break;
    }
    case NK_COMMAND_CIRCLE: {
      const struct nk_command_circle *s = (const void *)command;
      ellipse(s->x, s->y, s->w, s->h, s->line_thickness, s->color);
      break;
    }
    case NK_COMMAND_CIRCLE_FILLED: {
      const struct nk_command_circle_filled *s = (const void *)command;
      ellipse(s->x, s->y, s->w, s->h, 0, s->color);
      break;
    }
    case NK_COMMAND_TRIANGLE: {
      const struct nk_command_triangle *s = (const void *)command;
      line(s->a.x, s->a.y, s->b.x, s->b.y, s->line_thickness, s->color);
      line(s->b.x, s->b.y, s->c.x, s->c.y, s->line_thickness, s->color);
      line(s->c.x, s->c.y, s->a.x, s->a.y, s->line_thickness, s->color);
      break;
    }
    case NK_COMMAND_TRIANGLE_FILLED: {
      const struct nk_command_triangle_filled *s = (const void *)command;
      struct nk_vec2i p[3] = {s->a, s->b, s->c};
      polygon(p, 3, s->color);
      break;
    }
    case NK_COMMAND_POLYGON:
    case NK_COMMAND_POLYLINE: {
      const struct nk_command_polygon *s = (const void *)command;
      for (unsigned i = 1; i < s->point_count; i++)
        line(s->points[i - 1].x, s->points[i - 1].y, s->points[i].x,
             s->points[i].y, s->line_thickness, s->color);
      if (command->type == NK_COMMAND_POLYGON && s->point_count)
        line(s->points[s->point_count - 1].x, s->points[s->point_count - 1].y,
             s->points[0].x, s->points[0].y, s->line_thickness, s->color);
      break;
    }
    case NK_COMMAND_POLYGON_FILLED: {
      const struct nk_command_polygon_filled *s = (const void *)command;
      polygon(s->points, s->point_count, s->color);
      break;
    }
    case NK_COMMAND_CURVE: {
      const struct nk_command_curve *s = (const void *)command;
      int x = s->begin.x, y = s->begin.y;
      for (unsigned i = 1; i <= 24; i++) {
        float t = i / 24.0f, u = 1 - t;
        int nx = u * u * u * s->begin.x + 3 * u * u * t * s->ctrl[0].x +
                 3 * u * t * t * s->ctrl[1].x + t * t * t * s->end.x;
        int ny = u * u * u * s->begin.y + 3 * u * u * t * s->ctrl[0].y +
                 3 * u * t * t * s->ctrl[1].y + t * t * t * s->end.y;
        line(x, y, nx, ny, s->line_thickness, s->color);
        x = nx;
        y = ny;
      }
      break;
    }
    case NK_COMMAND_ARC:
    case NK_COMMAND_ARC_FILLED: {
      const struct nk_command_arc *s = (const void *)command;
      bool filled = command->type == NK_COMMAND_ARC_FILLED;
      unsigned r;
      float start, end;
      struct nk_color c;
      int cx, cy;
      unsigned thickness;
      if (filled) {
        const struct nk_command_arc_filled *f = (const void *)command;
        r = f->r;
        start = f->a[0];
        end = f->a[1];
        c = f->color;
        cx = f->cx;
        cy = f->cy;
        thickness = 1;
      } else {
        r = s->r;
        start = s->a[0];
        end = s->a[1];
        c = s->color;
        cx = s->cx;
        cy = s->cy;
        thickness = s->line_thickness;
      }
      int x = cx + sine(start + 1.5707963f) * r, y = cy + sine(start) * r;
      for (unsigned i = 1; i <= 48; i++) {
        float angle = start + (end - start) * i / 48;
        int nx = cx + sine(angle + 1.5707963f) * r, ny = cy + sine(angle) * r;
        if (filled) {
          struct nk_vec2i p[3] = {{cx, cy}, {x, y}, {nx, ny}};
          polygon(p, 3, c);
        } else
          line(x, y, nx, ny, thickness, c);
        x = nx;
        y = ny;
      }
      break;
    }
    case NK_COMMAND_TEXT:
      text((const void *)command);
      break;
    case NK_COMMAND_IMAGE: {
      const struct nk_command_image *s = (const void *)command;
      const uint32_t *source = s->img.handle.ptr;
      if (source && s->img.w && s->img.h && s->w && s->h)
        for (unsigned y = 0; y < s->h; y++)
          for (unsigned x = 0; x < s->w; x++) {
            unsigned sx =
                s->img.region[0] +
                x * (s->img.region[2] ? s->img.region[2] : s->img.w) / s->w;
            unsigned sy =
                s->img.region[1] +
                y * (s->img.region[3] ? s->img.region[3] : s->img.h) / s->h;
            if (sx < s->img.w && sy < s->img.h) {
              uint32_t value = source[sy * s->img.w + sx];
              point(s->x + x, s->y + y,
                    nk_rgba((value >> 16 & 255) * s->col.r / 255,
                            (value >> 8 & 255) * s->col.g / 255,
                            (value & 255) * s->col.b / 255,
                            (value >> 24) * s->col.a / 255));
            }
          }
      break;
    }
    case NK_COMMAND_CUSTOM: {
      const struct nk_command_custom *s = (const void *)command;
      if (s->callback)
        s->callback(pixels, s->x, s->y, s->w, s->h, s->callback_data);
      break;
    }
    }
  }
  command_alpha = 255;
  clip_x = clip_y = 0;
  clip_right = width;
  clip_bottom = height;
}
void gui_canvas_cursor(int x, int y) {
  if (cursor_drawn && cursor_x == x && cursor_y == y)
    return;
  if (cursor_drawn)
    for (unsigned row = 0; row < 32 && cursor_y + row < height; row++)
      for (unsigned col = 0; col < 32 && cursor_x + col < width; col++)
        pixels[(cursor_y + row) * width + cursor_x + col] =
            cursor_under[row * 32 + col];
  cursor_x = x;
  cursor_y = y;
  for (unsigned row = 0; row < 32 && y + row < height; row++)
    for (unsigned col = 0; col < 32 && x + col < width; col++)
      cursor_under[row * 32 + col] = pixels[(y + row) * width + x + col];
  cursor_drawn = cursor_moved = true;
  struct nk_vec2i border[7] = {
      {x, y},           {x + 1, y + 23},  {x + 7, y + 17}, {x + 12, y + 27},
      {x + 16, y + 25}, {x + 11, y + 15}, {x + 20, y + 14}};
  polygon(border, 7, nk_rgba(15, 15, 22, 240));
  struct nk_vec2i inside[7] = {
      {x + 2, y + 4},   {x + 3, y + 19}, {x + 8, y + 13}, {x + 13, y + 23},
      {x + 14, y + 23}, {x + 9, y + 12}, {x + 15, y + 12}};
  polygon(inside, 7, nk_rgb(250, 250, 255));
}
void gui_canvas_present(void) {
  if (scene_repaint)
    graphics_present(pixels, width, height);
  else if (cursor_moved) {
    graphics_present_region(pixels, width, height, presented_x, presented_y, 32,
                            32);
    graphics_present_region(pixels, width, height, cursor_x, cursor_y, 32, 32);
  }
  presented_x = cursor_x;
  presented_y = cursor_y;
  scene_repaint = cursor_moved = false;
}
void gui_canvas_icon(struct nk_command_buffer *b, struct nk_rect r,
                     unsigned kind, struct nk_color c) {
  float x = r.x + (r.w - 24) / 2, y = r.y + (r.h - 24) / 2;
  if (kind == 0) {
    nk_stroke_rect(b, nk_rect(x, y + 2, 24, 20), 4, 2, c);
    nk_stroke_line(b, x + 5, y + 8, x + 9, y + 12, 2, c);
    nk_stroke_line(b, x + 9, y + 12, x + 5, y + 16, 2, c);
    nk_stroke_line(b, x + 12, y + 16, x + 19, y + 16, 2, c);
  } else if (kind == 1) {
    nk_fill_rect(b, nk_rect(x, y + 5, 24, 17), 3, c);
    nk_fill_rect(b, nk_rect(x + 1, y + 2, 11, 7), 2, c);
    nk_stroke_line(b, x + 3, y + 10, x + 21, y + 10, 2,
                   nk_rgba(255, 255, 255, 140));
  } else if (kind == 2) {
    nk_stroke_rect(b, nk_rect(x + 3, y, 18, 24), 3, 2, c);
    for (unsigned i = 0; i < 3; i++)
      nk_stroke_line(b, x + 7, y + 7 + i * 5, x + 17, y + 7 + i * 5, 2, c);
  } else if (kind == 3) {
    for (unsigned i = 0; i < 3; i++)
      nk_fill_rect(b, nk_rect(x + 2 + i * 8, y + 14 - i * 5, 5, 10 + i * 5), 2,
                   c);
  } else if (kind == 4) {
    nk_stroke_circle(b, nk_rect(x, y, 24, 24), 2, c);
    nk_stroke_circle(b, nk_rect(x + 7, y, 10, 24), 1, c);
    nk_stroke_line(b, x + 1, y + 12, x + 23, y + 12, 2, c);
  } else if (kind == 5) {
    nk_stroke_rect(b, nk_rect(x + 2, y + 3, 20, 19), 3, 2, c);
    nk_stroke_line(b, x + 2, y + 8, x + 22, y + 8, 2, c);
    nk_stroke_line(b, x + 10, y + 4, x + 10, y + 12, 2, c);
  } else if (kind == 6) {
    nk_stroke_circle(b, nk_rect(x + 4, y + 4, 16, 16), 3, c);
    for (unsigned i = 0; i < 4; i++) {
      float a = i * 1.5707963f;
      nk_stroke_line(b, x + 12 + sine(a) * 8, y + 12 + sine(a + 1.5707963f) * 8,
                     x + 12 + sine(a) * 12, y + 12 + sine(a + 1.5707963f) * 12,
                     3, c);
    }
    nk_stroke_circle(b, nk_rect(x + 9, y + 9, 6, 6), 2, c);
  } else if (kind == 8) {
    nk_stroke_circle(b, nk_rect(x + 3, y + 4, 18, 18), 2, c);
    nk_stroke_line(b, x + 12, y + 1, x + 12, y + 12, 3, c);
  } else {
    for (unsigned row = 0; row < 2; row++)
      for (unsigned col = 0; col < 2; col++)
        nk_fill_rect(b, nk_rect(x + col * 13, y + row * 13, 10, 10), 3, c);
  }
}
