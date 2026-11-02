#ifndef AVALANCHE_H
#define AVALANCHE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
void console_putc(char);
void console_write(const char *);
#define PHYSICAL_GIB 64u
#define PHYSICAL_PAGES (PHYSICAL_GIB * 262144u)
#define IDENTITY_ENTRIES (PHYSICAL_GIB * 512u)
#define DIRECT_MAP_BASE UINT64_C(0xffff800000000000)
#define DIRECT_MAP_LIMIT (UINT64_C(1) << 47)
static inline uint64_t page_physical(const void *p) {
  uintptr_t address = (uintptr_t)p;
  return address >= DIRECT_MAP_BASE ? address - DIRECT_MAP_BASE : address;
}
static inline void *page_pointer(uint64_t physical) {
  return (void *)(uintptr_t)(DIRECT_MAP_BASE + physical);
}
void *pages_alloc(unsigned count);
void *page_alloc(void);
bool page_free(void *);
#endif
