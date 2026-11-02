#include "lib.h"

static volatile unsigned char initialized[192 * 1024] = {
    [0] = 17, [65535] = 41, [131072] = 83, [192 * 1024 - 1] = 127};
static volatile unsigned char workspace[16 * 1024 * 1024];

int main(void) {
  if (initialized[0] != 17 || initialized[65535] != 41 ||
      initialized[131072] != 83 || initialized[sizeof(initialized) - 1] != 127)
    return 1;
  for (unsigned i = 0; i < sizeof(workspace); i += 4096) {
    if (workspace[i])
      return 2;
    workspace[i] = (unsigned char)((i / 4096) * 13 + 7);
  }
  workspace[sizeof(workspace) - 1] = 219;
  for (unsigned i = 0; i < sizeof(workspace); i += 4096)
    if (workspace[i] != (unsigned char)((i / 4096) * 13 + 7))
      return 3;
  if (workspace[sizeof(workspace) - 1] != 219)
    return 4;
  puts_u("LARGE EXECUTABLE AND 16 MIB DATA OK\n");
  av_call(AV_SLEEP, 1000, 0, 0, 0);
  return 0;
}
