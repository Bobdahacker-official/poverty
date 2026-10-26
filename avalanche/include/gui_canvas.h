#ifndef AVALANCHE_GUI_CANVAS_H
#define AVALANCHE_GUI_CANVAS_H
#include "gui_toolkit.h"
void gui_canvas_init(unsigned, unsigned);
void gui_canvas_scale(float);
void gui_canvas_wallpaper(uint32_t, unsigned);
void gui_canvas_begin(void);
bool gui_canvas_changed(struct nk_context *, bool);
void gui_canvas_shadow(struct nk_rect);
void gui_canvas_gloss(struct nk_command_buffer *, struct nk_rect, unsigned);
void gui_canvas_open(unsigned);
void gui_canvas_commands(struct nk_context *, bool);
void gui_canvas_cursor(int, int);
void gui_canvas_present(void);
void gui_canvas_icon(struct nk_command_buffer *, struct nk_rect, unsigned,
                     struct nk_color);
float gui_font_width(nk_handle, float, const char *, int);
#endif
