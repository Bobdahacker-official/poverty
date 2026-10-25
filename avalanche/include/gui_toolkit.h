#ifndef AVALANCHE_GUI_TOOLKIT_H
#define AVALANCHE_GUI_TOOLKIT_H
#include "avalanche.h"
void gui_toolkit_assert(const char *, const char *, unsigned);
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_COMMAND_USERDATA
#define NK_ZERO_COMMAND_MEMORY
#define NK_MEMCPY memcpy
#define NK_MEMSET memset
#define NK_ASSERT(expr)                                                        \
  do {                                                                         \
    if (!(expr))                                                               \
      gui_toolkit_assert(#expr, __FILE__, __LINE__);                           \
  } while (0)
#include "../vendor/nuklear/nuklear.h"
#endif
