#ifndef AVALANCHE_PACKAGE_H
#define AVALANCHE_PACKAGE_H
#include "system.h"
void sha256(const void *, size_t, uint8_t[32]);
void package_command(char *);
int user_builtin(const char *, const void **, uint32_t *);
#endif
