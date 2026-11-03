#include <magenta/app.h>
static uint32_t pixels[320 * 200];
int main(void) {
  const struct av_gui_open window = {320, 200, "SDK window"};
  if (mg_window_open(&window) < 0)
    return 1;
  for (unsigned y = 0; y < 200; y++)
    for (unsigned x = 0; x < 320; x++)
      pixels[y * 320 + x] =
          0xff000000u | (x * 255 / 320) << 16 | (y * 180 / 200) << 8 | 100;
  struct av_gui_draw draw = {
      .width = 320, .height = 200, .stride = 320, .pixels = (uintptr_t)pixels};
  if (mg_window_draw(&draw) < 0)
    return 2;
  for (;;) {
    struct av_gui_event event;
    int result = mg_window_event(&event);
    if (result < 0)
      return 3;
    if (result > 0 && event.type == AV_GUI_QUIT)
      break;
    if (result > 0 && event.type == AV_GUI_MOUSE && (event.c & 1)) {
      unsigned x = event.a, y = event.b;
      if (x < 320 && y < 200) {
        for (unsigned j = y; j < y + 8 && j < 200; j++)
          for (unsigned i = x; i < x + 8 && i < 320; i++)
            pixels[j * 320 + i] = 0xffffffff;
        if (mg_window_draw(&draw) < 0)
          return 4;
      }
    }
    mg_sleep(1);
  }
  mg_window_close();
  return 0;
}
