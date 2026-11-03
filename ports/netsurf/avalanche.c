/* Avalanche surface for NetSurf's MIT-licensed libnsfb framebuffer library. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "magenta.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"
struct avalanche_surface {
  nsfb_event_t pending[16];
  unsigned count, head, buttons;
};
static int defaults(nsfb_t *fb) {
  fb->width = 800;
  fb->height = 600;
  fb->format = NSFB_FMT_XRGB8888;
  return select_plotters(fb) ? 0 : -1;
}
static int geometry(nsfb_t *fb, int width, int height,
                    enum nsfb_format_e format) {
  (void)format;
  if (width > 0)
    fb->width = width;
  if (height > 0)
    fb->height = height;
  fb->format = NSFB_FMT_XRGB8888;
  if (fb->width < 160 || fb->width > 2048 || fb->height < 100 ||
      fb->height > 1536)
    return -1;
  select_plotters(fb);
  fb->linelen = fb->width * 4;
  if (fb->ptr) {
    void *data = realloc(fb->ptr, (size_t)fb->linelen * fb->height);
    if (!data)
      return -1;
    fb->ptr = data;
  }
  return 0;
}
static int initialise(nsfb_t *fb) {
  fb->linelen = fb->width * 4;
  fb->ptr = calloc(fb->height, fb->linelen);
  fb->surface_priv = calloc(1, sizeof(struct avalanche_surface));
  if (!fb->ptr || !fb->surface_priv) {
    free(fb->ptr);
    free(fb->surface_priv);
    fb->ptr = NULL;
    fb->surface_priv = NULL;
    return -1;
  }
  struct av_gui_open open = {.width = fb->width, .height = fb->height};
  strcpy(open.title, "NetSurf");
  if (syscall(4096, &open) < 0) {
    free(fb->ptr);
    free(fb->surface_priv);
    fb->ptr = NULL;
    fb->surface_priv = NULL;
    return -1;
  }
  return 0;
}
static int finalise(nsfb_t *fb) {
  syscall(4099, 0);
  free(fb->ptr);
  free(fb->surface_priv);
  fb->ptr = NULL;
  fb->surface_priv = NULL;
  return 0;
}
static int update(nsfb_t *fb, nsfb_bbox_t *box) {
  int x = box ? box->x0 : 0, y = box ? box->y0 : 0;
  int right = box ? box->x1 : fb->width, bottom = box ? box->y1 : fb->height;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (right > fb->width)
    right = fb->width;
  if (bottom > fb->height)
    bottom = fb->height;
  if (right <= x || bottom <= y)
    return 0;
  struct av_gui_draw draw = {
      .x = x,
      .y = y,
      .width = right - x,
      .height = bottom - y,
      .stride = fb->width,
      .pixels = (uintptr_t)(fb->ptr + y * fb->linelen + x * 4)};
  return syscall(4097, &draw) < 0 ? -1 : 0;
}
static int cursor(nsfb_t *fb, struct nsfb_cursor_s *shape) {
  /* Magenta draws the pointer once, above every application surface. */
  (void)fb;
  (void)shape;
  return 1;
}
static void push(struct avalanche_surface *s, enum nsfb_event_type_e type,
                 int code) {
  if (s->count == 16)
    return;
  nsfb_event_t *event = &s->pending[(s->head + s->count++) % 16];
  memset(event, 0, sizeof(*event));
  event->type = type;
  event->value.keycode = code;
}
static bool input(nsfb_t *fb, nsfb_event_t *event, int timeout) {
  struct avalanche_surface *s = fb->surface_priv;
  struct timespec begin, now;
  clock_gettime(CLOCK_MONOTONIC, &begin);
  while (!s->count) {
    struct av_gui_event in;
    int result = syscall(4098, &in);
    if (result < 0) {
      memset(event, 0, sizeof(*event));
      event->type = NSFB_EVENT_CONTROL;
      event->value.controlcode = NSFB_CONTROL_QUIT;
      return true;
    }
    if (result > 0) {
      if (in.type == AV_GUI_QUIT) {
        memset(event, 0, sizeof(*event));
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;
      }
      if (in.type == AV_GUI_MOUSE) {
        push(s, NSFB_EVENT_MOVE_ABSOLUTE, 0);
        s->pending[(s->head + s->count - 1) % 16].value.vector.x = in.a;
        s->pending[(s->head + s->count - 1) % 16].value.vector.y = in.b;
        for (unsigned bit = 0; bit < 3; bit++)
          if ((s->buttons ^ in.c) & (1u << bit))
            push(s,
                 (in.c & (1u << bit)) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP,
                 bit == 0   ? NSFB_KEY_MOUSE_1
                 : bit == 1 ? NSFB_KEY_MOUSE_3
                            : NSFB_KEY_MOUSE_2);
        s->buttons = in.c;
      } else if (in.type == AV_GUI_TEXT || in.type == AV_GUI_KEY) {
        int key = in.a;
        if (in.type == AV_GUI_TEXT && key == 10)
          key = NSFB_KEY_RETURN;
        if (in.type == AV_GUI_KEY) {
          switch (key) {
          case 0x4b:
            key = NSFB_KEY_LEFT;
            break;
          case 0x4d:
            key = NSFB_KEY_RIGHT;
            break;
          case 0x48:
            key = NSFB_KEY_UP;
            break;
          case 0x50:
            key = NSFB_KEY_DOWN;
            break;
          case 0x47:
            key = NSFB_KEY_HOME;
            break;
          case 0x4f:
            key = NSFB_KEY_END;
            break;
          case 0x49:
            key = NSFB_KEY_PAGEUP;
            break;
          case 0x51:
            key = NSFB_KEY_PAGEDOWN;
            break;
          case 0x53:
            key = NSFB_KEY_DELETE;
            break;
          default:
            key = 0;
          }
        }
        if (key) {
          push(s, NSFB_EVENT_KEY_DOWN, key);
          push(s, NSFB_EVENT_KEY_UP, key);
        }
      }
      continue;
    }
    if (!timeout)
      return false;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (timeout > 0 && (now.tv_sec - begin.tv_sec) * 1000 +
                               (now.tv_nsec - begin.tv_nsec) / 1000000 >=
                           timeout)
      return false;
    const struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
  }
  *event = s->pending[s->head];
  s->head = (s->head + 1) % 16;
  s->count--;
  return true;
}
static const nsfb_surface_rtns_t avalanche_rtns = {.defaults = defaults,
                                                   .initialise = initialise,
                                                   .finalise = finalise,
                                                   .geometry = geometry,
                                                   .input = input,
                                                   .update = update,
                                                   .cursor = cursor};
NSFB_SURFACE_DEF(avalanche, NSFB_SURFACE_AVALANCHE, &avalanche_rtns)
