#include "lib.h"
size_t ulen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}
int ueq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
void puts_u(const char *s) {
  size_t n = ulen(s);
  while (n) {
    uint32_t count = n > 4096 ? 4096 : n;
    av_call(AV_WRITE, (uint32_t)s, count, 0, 0);
    s += count;
    n -= count;
  }
}
void uint_u(uint32_t n) {
  char p[12];
  unsigned i = 0;
  do {
    p[i++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (i) {
    char c = p[--i];
    av_call(AV_WRITE, (uint32_t)&c, 1, 0, 0);
  }
}
char *word_u(char **p) {
  while (**p == ' ')
    ++*p;
  char *s = *p;
  while (**p && **p != ' ')
    ++*p;
  if (**p)
    *(*p)++ = 0;
  return s;
}
uint32_t decimal_u(const char *s) {
  uint32_t n = 0;
  if (!*s)
    return 0;
  while (*s) {
    if (*s < '0' || *s > '9' || n > (0xffffffffu - (unsigned)(*s - '0')) / 10)
      return 0;
    n = n * 10 + (unsigned)(*s++ - '0');
  }
  return n;
}
uint32_t address_u(const char *s) {
  uint32_t n = 0;
  for (unsigned k = 0; k < 4; k++) {
    if (*s < '0' || *s > '9')
      return 0;
    unsigned part = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      part = part * 10 + (unsigned)(*s++ - '0');
      if (part > 255 || ++digits > 3)
        return 0;
    }
    n = (n << 8) | part;
    if (k < 3) {
      if (*s++ != '.')
        return 0;
    } else if (*s)
      return 0;
  }
  return n;
}
