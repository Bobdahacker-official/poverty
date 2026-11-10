#include "client.h"
#include "magenta.h"
struct gui_client gui_clients[GUI_CLIENTS];
static bool enabled;
void client_enable(bool value) { enabled = value; }
void client_destroy(int pid) {
  for (unsigned i = 0; i < GUI_CLIENTS; i++)
    if (gui_clients[i].pixels && gui_clients[i].pid == pid) {
      struct gui_client *c = &gui_clients[i];
      unsigned pages = ((uint64_t)c->width * c->height * 4 + 4095) / 4096;
      for (unsigned p = 0; p < pages; p++)
        page_free((uint8_t *)c->pixels + p * 4096);
      memset(c, 0, sizeof(*c));
      gui_canvas_invalidate();
    }
}
void client_event(unsigned index, unsigned type, int a, int b, int flags) {
  if (index >= GUI_CLIENTS)
    return;
  struct gui_client *c = &gui_clients[index];
  if (!c->pixels)
    return;
  if (type == AV_GUI_QUIT)
    c->closing = true;
  bool motion = type == AV_GUI_MOUSE && c->pointer_known &&
                c->pointer_buttons == flags;
  if (motion && c->tail_motion && c->event_count) {
    unsigned tail = (c->event_head + c->event_count - 1) % 128;
    struct av_gui_event *last = &c->events[tail];
    if (last->type == AV_GUI_MOUSE && last->c == flags) {
      last->a = a;
      last->b = b;
      return;
    }
  }
  if (c->event_count == 128) {
    if (type != AV_GUI_QUIT)
      return;
    c->event_head = (c->event_head + 1) % 128;
    c->event_count--;
  }
  c->tail_motion = motion;
  if (type == AV_GUI_MOUSE) {
    c->pointer_known = true;
    c->pointer_buttons = flags;
  }
  unsigned at = (c->event_head + c->event_count++) % 128;
  c->events[at] = (struct av_gui_event){type, a, b, flags};
}
int client_call(struct process *t, unsigned operation, uint32_t address) {
  if (!enabled)
    return -19;
  struct gui_client *c = NULL;
  for (unsigned i = 0; i < GUI_CLIENTS; i++)
    if (gui_clients[i].pixels && gui_clients[i].pid == t->pid)
      c = &gui_clients[i];
  if (operation == 0) {
    struct av_gui_open request;
    if (!process_copyin(t, &request, address, sizeof(request)))
      return -14;
    if (request.width < 160 || request.width > 2048 || request.height < 100 ||
        request.height > 1536 ||
        !__builtin_memchr(request.title, 0, sizeof(request.title)))
      return -22;
    if (c)
      client_destroy(t->pid);
    for (unsigned i = 0; i < GUI_CLIENTS; i++)
      if (!gui_clients[i].pixels) {
        c = &gui_clients[i];
        break;
      }
    if (!c)
      return -24;
    uint32_t *pixels = pages_alloc(
        ((uint64_t)request.width * request.height * 4 + 4095) / 4096);
    if (!pixels)
      return -12;
    memset(c, 0, sizeof(*c));
    c->pid = t->pid;
    c->width = request.width;
    c->height = request.height;
    c->pixels = pixels;
    memcpy(c->title, request.title, sizeof(c->title));
    for (unsigned i = 0; i < c->width * c->height; i++)
      pixels[i] = 0xff20202e;
    gui_canvas_invalidate();
    return 0;
  }
  if (!c)
    return -9;
  if (operation == 3) {
    client_destroy(t->pid);
    return 0;
  }
  if (operation == 2) {
    if (!c->event_count)
      return 0;
    if (!process_copyout(t, address, &c->events[c->event_head],
                         sizeof(struct av_gui_event)))
      return -14;
    c->event_head = (c->event_head + 1) % 128;
    c->event_count--;
    return 1;
  }
  if (operation == 1) {
    struct av_gui_draw draw;
    if (!process_copyin(t, &draw, address, sizeof(draw)))
      return -14;
    if (draw.x >= c->width || draw.y >= c->height ||
        draw.width > c->width - draw.x || draw.height > c->height - draw.y ||
        draw.stride < draw.width || draw.stride > 4096 ||
        draw.pixels > UINT32_MAX)
      return -22;
    uint64_t size =
        draw.height
            ? ((uint64_t)(draw.height - 1) * draw.stride + draw.width) * 4
            : 0;
    if (size > UINT32_MAX || !process_range(t, draw.pixels, size, false))
      return -14;
    bool changed = false;
    uint32_t row[2048];
    for (unsigned y = 0; y < draw.height; y++) {
      uint32_t *out = c->pixels + (draw.y + y) * c->width + draw.x;
      if (!process_copyin(t, row, draw.pixels + (uint64_t)y * draw.stride * 4,
                          draw.width * 4)) {
        if (changed) {
          c->sequence++;
          gui_canvas_invalidate();
        }
        return -14;
      }
      for (unsigned x = 0; x < draw.width; x++) {
        uint32_t pixel = row[x] | 0xff000000;
        if (out[x] != pixel) {
          out[x] = pixel;
          changed = true;
        }
      }
    }
    if (changed) {
      c->sequence++;
      gui_canvas_invalidate();
    }
    return 0;
  }
  return -38;
}
