#ifndef AVALANCHE_USB_H
#define AVALANCHE_USB_H
#include "system.h"
bool usb_control(unsigned, bool, unsigned, uint8_t, uint8_t, uint16_t, uint16_t,
                 uint16_t, bool);
int usb_bulk(unsigned address, unsigned endpoint, unsigned packet,
             unsigned *toggle, void *data, unsigned size, bool input);
bool usb_storage_attach(unsigned address, unsigned control_packet,
                        unsigned interface, unsigned input, unsigned output,
                        unsigned input_packet, unsigned output_packet);
#endif
