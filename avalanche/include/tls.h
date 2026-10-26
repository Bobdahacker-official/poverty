#ifndef AVALANCHE_TLS_H
#define AVALANCHE_TLS_H
#include "avalanche.h"
bool tls_connect(const char *hostname);
int tls_send(const void *, unsigned);
int tls_recv(void *, unsigned);
void tls_close(void);
unsigned tls_error(void);
bool rtc_time(uint32_t *days, uint32_t *seconds);
#endif
