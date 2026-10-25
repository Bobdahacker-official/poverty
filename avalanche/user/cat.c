#include "lib.h"
static char content[65537];
int main(void) {
  char args[256];
  args_u(args, sizeof(args));
  char *rest = args, *name = word_u(&rest);
  int n = read_u(name, content, 65536);
  if (n < 0) {
    puts_u("cat: cannot read file\n");
    return 1;
  }
  content[n] = 0;
  puts_u(content);
  puts_u("\n");
  return 0;
}
