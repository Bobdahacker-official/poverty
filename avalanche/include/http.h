#ifndef AVALANCHE_HTTP_H
#define AVALANCHE_HTTP_H
#include "avalanche.h"
int http_get(const char *url, char *body, unsigned capacity);
bool http_resolve(const char *base, const char *reference, char *out, unsigned capacity);
#endif
