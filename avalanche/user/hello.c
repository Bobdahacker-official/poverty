#include "lib.h"
int main(void) {
  char args[256];
  args_u(args, sizeof(args));
  puts_u("Hello from ring 3!\nArguments: ");
  puts_u(args);
  puts_u("\nUptime ticks: ");
  uint_u((uint32_t)av_call(AV_TICKS, 0, 0, 0, 0));
  puts_u("\n");
  return 0;
}
