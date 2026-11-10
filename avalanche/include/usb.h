#ifndef AVALANCHE_USB_H
#define AVALANCHE_USB_H
#include "system.h"
void xhci_init(void);
void xhci_poll(void);
void xhci_service_ports(void);
bool xhci_control(unsigned, uint8_t, uint8_t, uint16_t, uint16_t, void *, unsigned);
int xhci_bulk(unsigned, unsigned, void *, unsigned, bool);
void usb_keyboard_report(uint8_t[6], uint8_t *, const uint8_t *);
bool usb_control(unsigned, bool, unsigned, uint8_t, uint8_t, uint16_t, uint16_t,
                 uint16_t, bool);
int usb_bulk(unsigned address, unsigned endpoint, unsigned packet,
             unsigned *toggle, void *data, unsigned size, bool input);
bool usb_storage_attach(unsigned address, unsigned control_packet,
                        unsigned interface, unsigned input, unsigned output,
                        unsigned input_packet, unsigned output_packet);
void usb_storage_detach(unsigned address);
struct bluetooth_peer {
  uint8_t address[6], address_type;
  int8_t rssi;
  uint32_t last_seen;
  char name[32];
};
unsigned usb_bluetooth_state(void);
bool usb_bluetooth_scan(bool);
unsigned usb_bluetooth_peers(struct bluetooth_peer *, unsigned);
enum bluetooth_pair_state {
  BT_PAIR_IDLE, BT_PAIR_STARTING, BT_PAIR_CONNECTING, BT_PAIR_AUTHENTICATING,
  BT_PAIR_CONFIRM, BT_PAIR_ENCRYPTING, BT_PAIR_CONNECTED, BT_PAIR_FAILED,
  BT_PAIR_DISCONNECTING
};
int usb_bluetooth_pair(const uint8_t address[6]);
unsigned usb_bluetooth_pair_state(uint32_t *number, unsigned *error);
void usb_bluetooth_confirm(bool accept);
void usb_bluetooth_disconnect(void);
unsigned usb_bluetooth_acl_capacity(void);
int usb_bluetooth_acl_send(const void *, unsigned);
void bluetooth_audio_receive(const uint8_t *, unsigned);
void bluetooth_audio_poll(void);
int bluetooth_audio_start(const char *);
int bluetooth_audio_status(void);
unsigned bluetooth_audio_bytes(void);
void bluetooth_audio_stop(void);
bool usb_ethernet_mac(uint8_t[6]);
bool usb_ethernet_send(const void *, unsigned);
void usb_ethernet_poll(void (*receive)(const uint8_t *, uint32_t));
bool usb_audio_available(void);
int usb_audio_start(void);
void usb_audio_stop(void);
int usb_audio_queue(const void *, unsigned);
unsigned usb_audio_completed(void);
int usb_audio_error(void);
#endif
