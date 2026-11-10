#ifndef AVALANCHE_RTW8821CU_H
#define AVALANCHE_RTW8821CU_H
#include "system.h"

#define RTW8821CU_TX_PIPES 4
struct rtw8821cu_endpoint {
  unsigned number, packet_size;
};
struct rtw8821cu_usb_config {
  unsigned configuration, tx_count;
  struct rtw8821cu_endpoint rx, tx[RTW8821CU_TX_PIPES];
};
bool rtw8821cu_usb_match(unsigned vendor, unsigned product);
bool rtw8821cu_usb_parse(const uint8_t *, unsigned, struct rtw8821cu_usb_config *);
void rtw8821cu_usb_attach(unsigned slot, unsigned vendor, unsigned product,
                        const struct rtw8821cu_usb_config *);
void rtw8821cu_usb_detach(unsigned slot);
void rtw8821cu_usb_info(void);
#endif
