#define NK_IMPLEMENTATION
#include "gui_toolkit.h"
#include "system.h"
void gui_toolkit_assert(const char *expression, const char *file,
                        unsigned line) {
  console_write("GUI assertion: ");
  console_write(expression);
  console_write(" in ");
  console_write(file);
  console_putc(':');
  print_uint(line);
  console_putc('\n');
  for (;;)
    __asm__ volatile("cli; hlt");
}
