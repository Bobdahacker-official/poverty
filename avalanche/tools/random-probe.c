#include "lib.h"
int main(void) {
  unsigned char first[32], second[32];
  int a = av_random(first, sizeof(first)),
      b = av_random(second, sizeof(second));
  if (a == -11 && b == -11) {
    puts_u("RANDOM UNAVAILABLE\n");
    return 0;
  }
  if (a != 32 || b != 32) {
    puts_u("RANDOM ERROR\n");
    return 1;
  }
  unsigned bits = 0, difference = 0;
  for (unsigned i = 0; i < 32; i++) {
    bits |= first[i];
    difference |= first[i] ^ second[i];
  }
  if (!bits || !difference) {
    puts_u("RANDOM ERROR\n");
    return 1;
  }
  puts_u("HARDWARE RANDOM OK\n");
  return 0;
}
