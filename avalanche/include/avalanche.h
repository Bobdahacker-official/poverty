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
#define PHYSICAL_GIB 16u
#define PHYSICAL_PAGES (PHYSICAL_GIB * 262144u)
#define IDENTITY_ENTRIES (PHYSICAL_GIB * 512u)
void *pages_alloc(unsigned count);
void *page_alloc(void);
bool page_free(void *);
#endif
