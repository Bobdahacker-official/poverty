#ifndef AV_NET_SERVICE_H
#define AV_NET_SERVICE_H
#include <stdint.h>
#include <stdbool.h>
void net_service_init(void);
void net_service_command(char *);
void net_diagnostics(void);
void net_dhcp_begin(void);
void net_dhcp_stop(void);
void net_service_poll(void);
void net_dns_servers(uint32_t,uint32_t);
void route_init(void);
void route_config(uint32_t,uint32_t,uint32_t);
bool route_lookup(uint32_t,uint32_t *);
void route_command(char *);
#endif
