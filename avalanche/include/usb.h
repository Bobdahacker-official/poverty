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
#endif
