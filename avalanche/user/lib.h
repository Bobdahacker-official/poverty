#ifndef AV_USER_LIB_H
#define AV_USER_LIB_H
#include "abi.h"
#include <stddef.h>
#include <stdint.h>
static inline int av_call(uint32_t n, uint32_t a, uint32_t b, uint32_t c,
                          uint32_t d) {
  int r;
  __asm__ volatile("int $0x80"
                   : "=a"(r)
                   : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                   : "memory", "cc");
  return r;
}
static inline int av_recv(uint16_t port, void *data, uint32_t cap,
                          uint32_t *source, uint16_t *srcport) {
  uint32_t a = AV_UDP_RECV, c = (uint32_t)data, d = cap;
  __asm__ volatile("int $0x80"
                   : "+a"(a), "+c"(c), "+d"(d)
                   : "b"((uint32_t)port)
                   : "memory", "cc");
  if (source)
    *source = d;
  if (srcport)
    *srcport = (uint16_t)c;
  return (int)a;
}
size_t ulen(const char *);
int ueq(const char *, const char *);
void puts_u(const char *);
void uint_u(uint32_t);
char *word_u(char **);
uint32_t decimal_u(const char *);
uint32_t address_u(const char *);
static inline int args_u(char *p, uint32_t n) {
  return av_call(AV_ARGS, (uint32_t)p, n, 0, 0);
}
static inline int read_u(const char *n, void *p, uint32_t size) {
  return av_call(AV_FILE_READ, (uint32_t)n, (uint32_t)p, size, 0);
}
static inline int write_u(const char *n, const void *p, uint32_t size) {
  return av_call(AV_FILE_WRITE, (uint32_t)n, (uint32_t)p, size, 0);
}
static inline int send_u(uint32_t ip, uint16_t local, uint16_t remote,
                         const void *p, uint32_t n) {
  return av_call(AV_UDP_SEND, ip, ((uint32_t)local << 16) | remote, (uint32_t)p,
                 n);
}
#endif
