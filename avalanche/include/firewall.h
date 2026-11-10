#ifndef AV_FIREWALL_H
#define AV_FIREWALL_H
#include <stdbool.h>
#include <stdint.h>
void firewall_init(void);
bool firewall_packet(bool, const uint8_t *, unsigned);
void firewall_command(char *);
#endif
