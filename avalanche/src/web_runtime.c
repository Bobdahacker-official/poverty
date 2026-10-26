#include "system.h"
/* Freestanding numeric conversion used by the embedded JavaScript engine. */
double strtod(const char *s, char **end) {
  const char *start = s;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    s++;
  bool negative = *s == '-';
  if (*s == '+' || *s == '-')
    s++;
  double value = 0, fraction = 0.1;
  unsigned digits = 0;
  while (*s >= '0' && *s <= '9') {
    value = value * 10 + *s++ - '0';
    digits++;
  }
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      value += (*s++ - '0') * fraction;
      fraction *= 0.1;
      digits++;
    }
  }
  if (!digits) {
    if (end)
      *end = (char *)start;
    return 0;
  }
  if (*s == 'e' || *s == 'E') {
    const char *exp = s++;
    bool minus = *s == '-';
    if (*s == '-' || *s == '+')
      s++;
    if (*s < '0' || *s > '9')
      s = exp;
    else {
      unsigned n = 0;
      while (*s >= '0' && *s <= '9') {
        if (n < 10000)
          n = n * 10 + *s - '0';
        s++;
      }
      if (n > 400)
        n = 400;
      while (n--)
        value = minus ? value / 10 : value * 10;
    }
  }
  if (end)
    *end = (char *)s;
  return negative ? -value : value;
}
double modf(double x, double *integer) {
  union {
    double d;
    uint64_t u;
  } v = {x};
  int exponent = (int)((v.u >> 52) & 2047) - 1023;
  if (exponent < 0) {
    *integer = x < 0 ? -0.0 : 0.0;
    return x;
  }
  if (exponent >= 52) {
    *integer = x;
    return exponent == 1024 && (v.u & ((UINT64_C(1) << 52) - 1))
               ? x
               : (x < 0 ? -0.0 : 0.0);
  }
  v.u &= ~((UINT64_C(1) << (52 - exponent)) - 1);
  *integer = v.d;
  return x - v.d;
}
void _putchar(char c) { console_putc(c); }
