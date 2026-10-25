#include "lib.h"
static unsigned append_number(char *s, unsigned pos, unsigned n) {
  char b[12];
  unsigned i = 0;
  do {
    b[i++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (i)
    s[pos++] = b[--i];
  return pos;
}
int main(void) {
  char name[256];
  args_u(name, sizeof(name));
  if(ueq(name,"sleep")) return av_call(AV_SLEEP,3000,0,0,0);
  unsigned pid = (unsigned)av_call(AV_GETPID, 0, 0, 0, 0);
  for (unsigned round = 0; round < 5; round++) {
    volatile unsigned work = 0;
    for (unsigned i = 0; i < 30000000; i++)
      work += i ^ round;
    char line[128];
    unsigned n = 0;
    const char prefix[] = "worker ";
    for (unsigned i = 0; i < sizeof(prefix) - 1; i++)
      line[n++] = prefix[i];
    for (unsigned i = 0; name[i] && i < 32; i++)
      line[n++] = name[i];
    const char tag[] = " pid=";
    for (unsigned i = 0; i < sizeof(tag) - 1; i++)
      line[n++] = tag[i];
    n = append_number(line, n, pid);
    const char r[] = " round=";
    for (unsigned i = 0; i < sizeof(r) - 1; i++)
      line[n++] = r[i];
    n = append_number(line, n, round);
    line[n++] = '\n';
    av_call(AV_WRITE, (uintptr_t)line, n, 0, 0);
  }
  return 0;
}
