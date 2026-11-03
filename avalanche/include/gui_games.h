#ifndef AVALANCHE_GUI_GAMES_H
#define AVALANCHE_GUI_GAMES_H
#include "gui_toolkit.h"
void games_ui(struct nk_context *, struct nk_color text, struct nk_color muted,
              struct nk_color surface, struct nk_color accent);
bool games_ui_key(char);
void games_ui_save(void);
#endif
