#ifndef AV_CLIENT_H
#define AV_CLIENT_H
#include "magenta.h"
#include "process.h"
#define GUI_CLIENTS 8
struct gui_client {
  int pid;
  unsigned width, height, event_head, event_count, sequence;
  uint32_t *pixels;
  char title[64];
  struct av_gui_event events[128];
  bool closing, pointer_known, tail_motion;
  int pointer_buttons;
};
extern struct gui_client gui_clients[GUI_CLIENTS];
int gui_clipboard_call(struct process *, bool, uint32_t, uint32_t);
int client_call(struct process *, unsigned, uint32_t);
void client_event(unsigned, unsigned, int, int, int);
void client_destroy(int);
void client_enable(bool);
void gui_canvas_invalidate(void);
#endif
