#include "font.h"
#include "system.h"
#define ACCENT 0xe6007e
#define WINDOWS 6
static uint32_t surface[1024 * 768];
static uint32_t backdrop[1024 * 768];
static unsigned opacity = 255;
static unsigned width, height;
static struct window {
  int x, y, w, h;
  bool open;
  uint32_t opened;
  char output[4096];
} windows[WINDOWS];
static const char *titles[WINDOWS] = {"Terminal", "Files",   "Text editor",
                                      "Tasks",    "Network", "Packages"};
static int order[WINDOWS], focus, drag = -1, drag_x, drag_y;
static char line[256], filename[32] = "notes", editor[4096];
static unsigned line_length, editor_length;
static const char *save_state = "CTRL-S SAVE";
static char notification[256];
static uint32_t notification_until;
static void notify(const char *message) {
  unsigned n = 0;
  while (message[n] && n + 1 < sizeof(notification)) {
    notification[n] = message[n];
    n++;
  }
  notification[n] = 0;
  notification_until = clock_ticks() + 300;
  console_write("[Magenta] ");
  console_write(notification);
  console_putc('\n');
}
static bool rounded(int, int, int, int, int);
static uint32_t blend(uint32_t background, uint32_t foreground,
                      unsigned alpha) {
  unsigned r = ((background >> 16 & 255) * (255 - alpha) +
                (foreground >> 16 & 255) * alpha) /
               255;
  unsigned g = ((background >> 8 & 255) * (255 - alpha) +
                (foreground >> 8 & 255) * alpha) /
               255;
  unsigned b =
      ((background & 255) * (255 - alpha) + (foreground & 255) * alpha) / 255;
  return r << 16 | g << 8 | b;
}
static void point(int x, int y, uint32_t color, unsigned alpha) {
  if (x >= 0 && y >= 0 && (unsigned)x < width && (unsigned)y < height)
    surface[y * width + x] =
        blend(surface[y * width + x], color, alpha * opacity / 255);
}
static void blur(int x, int y, int w, int h) {
  memcpy(backdrop, surface, width * height * sizeof(uint32_t));
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) {
      if (i < 3 || j < 3 || i + 3 >= (int)width || j + 3 >= (int)height ||
          !rounded(i - x, j - y, w, h, 14))
        continue;
      unsigned red = 0, green = 0, blue = 0;
      for (int dy = -3; dy <= 3; dy += 3)
        for (int dx = -3; dx <= 3; dx += 3) {
          uint32_t c = backdrop[(j + dy) * width + i + dx];
          red += c >> 16 & 255;
          green += c >> 8 & 255;
          blue += c & 255;
        }
      point(i, j, (red / 9) << 16 | (green / 9) << 8 | (blue / 9), 180);
    }
}
static bool rounded(int x, int y, int w, int h, int radius) {
  if (x < 0 || y < 0 || x >= w || y >= h)
    return false;
  int dx = x < radius ? radius - x : x >= w - radius ? x - (w - radius - 1) : 0;
  int dy = y < radius ? radius - y : y >= h - radius ? y - (h - radius - 1) : 0;
  return dx * dx + dy * dy <= radius * radius;
}
static void panel(int x, int y, int w, int h, int radius, uint32_t color,
                  unsigned alpha) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      if (rounded(i, j, w, h, radius))
        point(x + i, y + j, color, alpha);
}
static void text(int x, int y, const char *s, uint32_t color, int maxx,
                 int maxy) {
  int start = x;
  for (; *s && y + 14 < maxy; s++) {
    if (*s == '\n' || x + 12 >= maxx) {
      x = start;
      y += 16;
      if (*s == '\n')
        continue;
    }
    const uint8_t *alpha = font_character(*s);
    if (alpha) {
      for (unsigned row = 0; row < 18; row++)
        for (unsigned col = 0; col < 12; col++)
          if (alpha[row * 12 + col])
            point(x + col, y + row, color, alpha[row * 12 + col]);
    } else
      for (unsigned row = 0; row < 7; row++) {
        unsigned bits = graphics_glyph(*s, row);
        for (unsigned col = 0; col < 5; col++)
          if (bits & (1u << (4 - col)))
            for (unsigned sy = 0; sy < 2; sy++)
              for (unsigned sx = 0; sx < 2; sx++)
                point(x + col * 2 + sx, y + row * 2 + sy, color, 255);
      }
    x += 12;
  }
}
static void refresh(unsigned n) {
  char cmd[64];
  const char *source = n == 1   ? "ls"
                       : n == 3 ? "ps"
                       : n == 4 ? "net"
                       : n == 5 ? "pkg list"
                                : NULL;
  if (source) {
    memcpy(cmd, source, strlen(source) + 1);
    kernel_capture(cmd, windows[n].output, sizeof(windows[n].output));
  }
}
static void raise_window(int n) {
  int position = 0;
  while (position < WINDOWS && order[position] != n)
    position++;
  for (int i = position; i < WINDOWS - 1; i++)
    order[i] = order[i + 1];
  order[WINDOWS - 1] = n;
  focus = n;
  if (!windows[n].open)
    windows[n].opened = clock_ticks();
  windows[n].open = true;
  refresh(n);
  notify(titles[n]);
}
static void render(int mouse_x, int mouse_y) {
  opacity = 255;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      unsigned glow = (width - x) * (height - y) * 45 / (width * height);
      surface[y * width + x] =
          (20 + glow) << 16 | (10 + glow / 5) << 8 | (30 + glow);
    }
  text(30, 25, "MAGENTA", 0xffffff, width, height);
  text(30, 47, "ESC: TEXT DESKTOP", 0xb99bb5, width, height);
  for (int z = 0; z < WINDOWS; z++) {
    int n = order[z];
    struct window *w = &windows[n];
    if (!w->open)
      continue;
    unsigned elapsed = clock_ticks() - w->opened;
    opacity = elapsed >= 15 ? 255 : elapsed * 255 / 15;
    blur(w->x, w->y, w->w, w->h);
    for (int radius = 16; radius > 0; radius -= 2)
      panel(w->x - radius, w->y - radius + 8, w->w + radius * 2,
            w->h + radius * 2, 20, 0, 7);
    panel(w->x, w->y, w->w, w->h, 14, 0xc39fbb, 65);
    panel(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 13, 0x201420, 235);
    for (int y = 0; y < 34; y++) {
      uint32_t color = blend(ACCENT, 0xffa7d8, 90 - y * 2);
      for (int x = 0; x < w->w; x++)
        if (rounded(x, y, w->w, w->h, 13))
          point(w->x + x, w->y + y, color, n == focus ? 225 : 110);
    }
    text(w->x + 14, w->y + 10, titles[n], 0xffffff, w->x + w->w - 35,
         w->y + 34);
    text(w->x + w->w - 26, w->y + 10, "X", 0xffffff, width, height);
    if (n == 2) {
      text(w->x + 12, w->y + 43, filename, 0xff9fd2, w->x + w->w, w->y + w->h);
      text(w->x + 12, w->y + 67, editor, 0xf9e5f6, w->x + w->w - 12,
           w->y + w->h - 30);
      text(w->x + 12, w->y + w->h - 24, save_state, 0xff9fd2, w->x + w->w,
           height);
    } else {
      text(w->x + 12, w->y + 46, w->output, 0xf9e5f6, w->x + w->w - 12,
           w->y + w->h - 36);
      if (n == 0)
        text(w->x + 12, w->y + w->h - 26, line, 0xff9fd2, w->x + w->w - 12,
             height);
      if (n == 1)
        text(w->x + 12, w->y + w->h - 26, "CLICK FILE TO EDIT", 0xff9fd2,
             w->x + w->w, height);
    }
  }
  opacity = 255;
  panel(12, height - 52, width - 24, 42, 14, 0xe1b5d7, 30);
  for (int n = 0; n < WINDOWS; n++) {
    int spacing = (width - 48) / WINDOWS;
    int x = 24 + n * spacing;
    panel(x, height - 46, spacing - 8, 30, 10, n == focus ? ACCENT : 0x73506b,
          windows[n].open ? 210 : 100);
    text(x + 8, height - 38, titles[n], 0xffffff, x + spacing - 4, height);
  }
  if ((int32_t)(notification_until - clock_ticks()) > 0) {
    panel(width - 340, 16, 320, 64, 12, 0x573149, 230);
    text(width - 326, 28, notification, 0xffffff, width - 26, 78);
  }
  for (int y = 0; y < 18; y++)
    for (int x = 0; x <= y / 2; x++)
      point(mouse_x + x, mouse_y + y, 0xffffff, 255);
  graphics_present(surface, width, height);
}
int gui_run(void) {
  if (!graphics_size(&width, &height))
    return -19;
  if (!font_init())
    return -22;
  for (int n = 0; n < WINDOWS; n++) {
    order[n] = n;
    windows[n].x = 70 + n * 24;
    windows[n].y = 80 + n * 20;
    windows[n].w = width > 800 ? 650 : 490;
    windows[n].h = height > 600 ? 450 : 300;
    windows[n].open = false;
  }
  focus = 0;
  drag = -1;
  line_length = editor_length = 0;
  line[0] = editor[0] = 0;
  raise_window(0);
  memcpy(windows[0].output, "Avalanche command terminal. Type help.\n",
         sizeof("Avalanche command terminal. Type help.\n"));
  console_write("Magenta graphical desktop ready\n");
  uint8_t previous = 0;
  uint32_t last = clock_ticks();
  uint32_t last_refresh=last;
  while (true) {
    devices_poll();
    net_poll();
    user_poll();
    user_yield_kernel();
    if((uint32_t)(clock_ticks()-last_refresh)>=100) {
      if(windows[3].open) refresh(3);
      if(windows[4].open) refresh(4);
      last_refresh=clock_ticks();
    }
    int mx, my;
    uint8_t buttons;
    mouse_state(&mx, &my, &buttons);
    if (mx >= (int)width)
      mx = width - 1;
    if (my >= (int)height)
      my = height - 1;
    if ((buttons & 1) && !(previous & 1)) {
      if (my >= (int)height - 52) {
        int n = (mx - 24) / ((width - 48) / WINDOWS);
        if (mx >= 24 && n < WINDOWS)
          raise_window(n);
      } else
        for (int z = WINDOWS - 1; z >= 0; z--) {
          int n = order[z];
          struct window *w = &windows[n];
          if (!w->open || !rounded(mx - w->x, my - w->y, w->w, w->h, 14))
            continue;
          raise_window(n);
          if (my < w->y + 34) {
            if (mx > w->x + w->w - 35)
              w->open = false;
            else {
              drag = n;
              drag_x = mx - w->x;
              drag_y = my - w->y;
            }
          } else if (n == 1) {
            unsigned index = (my - w->y - 46) / 16;
            uint32_t size;
            char candidate[32], content[4096];
            bool found = false;
            for (unsigned slot = 0, ordinal = 0; slot < 32; slot++)
              if (fs_entry(slot, candidate, &size) && ordinal++ == index) {
                found = true;
                break;
              }
            if (found && size < sizeof(editor)) {
              int count = fs_read(candidate, content, sizeof(content) - 1);
              for (int i = 0; i < count; i++)
                if ((content[i] < 32 && content[i] != '\n' &&
                     content[i] != '\t') ||
                    (uint8_t)content[i] > 126) {
                  count = -1;
                  break;
                }
              if (count >= 0) {
                memcpy(filename, candidate, strlen(candidate) + 1);
                memcpy(editor, content, count);
                editor_length = count;
                editor[count] = 0;
                save_state = "CTRL-S SAVE";
                raise_window(2);
              }
            }
          } else if (n == 5) {
            static const char *names[] = {"hello", "cat", "worker", "udp",
                                          "http"};
            unsigned index = (my - w->y - 46) / 16;
            if (index < 5) {
              char command[64] = "pkg install ";
              memcpy(command + 12, names[index], strlen(names[index]) + 1);
              kernel_capture(command, w->output, sizeof(w->output));
              notify(w->output);
            }
          }
          break;
        }
    }
    if (!(buttons & 1))
      drag = -1;
    if (drag >= 0) {
      struct window *w = &windows[drag];
      w->x = mx - drag_x;
      w->y = my - drag_y;
      if (w->x < 0)
        w->x = 0;
      if (w->y < 0)
        w->y = 0;
      if (w->x > (int)width - 80)
        w->x = width - 80;
      if (w->y > (int)height - 90)
        w->y = height - 90;
    }
    previous = buttons;
    char c = console_getchar();
    if (c == 27)
      break;
    int shortcut = c == 20   ? 0
                   : c == 6  ? 1
                   : c == 5  ? 2
                   : c == 11 ? 3
                   : c == 14 ? 4
                   : c == 16 ? 5
                             : -1;
    if (shortcut >= 0) {
      raise_window(shortcut);
      c = 0;
    }
    if (c && windows[focus].open) {
      if (focus == 0) {
        if (c == '\n') {
          line[line_length] = 0;
          kernel_capture(line, windows[0].output, sizeof(windows[0].output));
          line_length = 0;
        } else if (c == '\b' && line_length)
          line_length--;
        else if (c >= 32 && line_length + 1 < sizeof(line))
          line[line_length++] = c;
        line[line_length] = 0;
      } else if (focus == 2) {
        if (c == 19) {
          int r = fs_write(filename, editor, editor_length);
          notify(r >= 0 ? "File saved" : "File save failed");
          save_state =
              r >= 0 ? "SAVED  |  CTRL-S SAVE" : "SAVE FAILED  |  CTRL-S RETRY";
          if (r >= 0)
            refresh(1);
        } else if (c == '\b' && editor_length)
          editor_length--;
        else if ((c == '\n' || c >= 32) && editor_length + 1 < sizeof(editor))
          editor[editor_length++] = c;
        editor[editor_length] = 0;
      } else if (c == 'r')
        refresh(focus);
    }
    if ((uint32_t)(clock_ticks() - last) >= 4) {
      render(mx, my);
      last = clock_ticks();
    }
    __asm__ volatile("hlt");
  }
  console_clear();
  return 0;
}
