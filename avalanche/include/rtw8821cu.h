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
struct rtw8821cu_hw {
  unsigned slot;
  const char *stage;
  uint16_t error_register, firmware_version;
  bool error, powered, efuse_valid, firmware_running;
  uint8_t efuse[512];
};
bool rtw8821cu_hw_start(unsigned, const struct rtw8821cu_usb_config *,
                        struct rtw8821cu_hw *);
bool rtw8821cu_hw_stop(struct rtw8821cu_hw *);
void rtw8821cu_command(const char *);
bool rtw8821cu_usb_match(unsigned vendor, unsigned product);
bool rtw8821cu_usb_parse(const uint8_t *, unsigned, struct rtw8821cu_usb_config *);
void rtw8821cu_usb_attach(unsigned slot, unsigned vendor, unsigned product,
                        const struct rtw8821cu_usb_config *);
void rtw8821cu_usb_detach(unsigned slot);
void rtw8821cu_usb_info(void);
#endif
