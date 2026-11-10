#ifndef AV_TCP_H
#define AV_TCP_H
#include <stdint.h>
#include <stdbool.h>
struct tcp_snapshot;
int net_tcp_socket(void);
int net_tcp_bind(int, uint32_t, uint16_t);
bool net_tcp_hungup(int);
int net_tcp_socket_error(int, bool);
void net_tcp_reuse(int, bool);
void tcp_diagnostics(void);
int net_tcp_listen(int, unsigned);
int net_tcp_accept(int);
unsigned net_tcp_pending(int);
int net_tcp_socket_connect(int, uint32_t, uint16_t, bool);
int net_tcp_socket_send(int, const void *, uint32_t, bool);
int net_tcp_socket_recv(int, void *, uint32_t, uint32_t);
int net_tcp_socket_close(int);
int net_tcp_shutdown(int, unsigned);
void net_tcp_socket_snapshot(int, struct tcp_snapshot *);
void tcp_input(uint32_t, const uint8_t *, uint32_t, const uint8_t *);
void tcp_poll(void);
void tcp_reset_all(void);
bool net_frame_send(const void *, uint32_t);
bool net_resolve(uint32_t, uint8_t *);
void net_device_mac(uint8_t[6]);
#endif
