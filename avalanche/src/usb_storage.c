#include "usb.h"
#define USB_DISKS 8
struct storage {
  unsigned address, control_packet, interface, input, output;
  unsigned input_packet, output_packet, input_toggle, output_toggle;
  uint32_t tag, blocks, block_size;
  uint8_t cbw[31] __attribute__((aligned(16)));
  uint8_t csw[13] __attribute__((aligned(16)));
  uint8_t data[4096] __attribute__((aligned(16)));
  bool ready;
};
static struct storage units[USB_DISKS];
static unsigned unit_count;
static void little32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = value >> (i * 8);
}
static uint32_t read_little32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint32_t big32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static bool reset(struct storage *s) {
  bool ok = usb_control(s->address, false, s->control_packet, 0x21, 0xff, 0,
                        s->interface, 0, false);
  ok = usb_control(s->address, false, s->control_packet, 2, 1, 0,
                   s->input | 0x80, 0, false) &&
       ok;
  ok = usb_control(s->address, false, s->control_packet, 2, 1, 0, s->output, 0,
                   false) &&
       ok;
  s->input_toggle = s->output_toggle = 0;
  return ok;
}
/* Return 0 for success, 1 for SCSI check condition, -1 for transport failure.
 */
static int command(struct storage *s, const uint8_t *cdb, unsigned length,
                   unsigned size, bool input) {
  if (!length || length > 16 || size > sizeof(s->data))
    return -1;
  memset(s->cbw, 0, sizeof(s->cbw));
  little32(s->cbw, 0x43425355);
  little32(s->cbw + 4, ++s->tag);
  little32(s->cbw + 8, size);
  s->cbw[12] = input ? 0x80 : 0;
  s->cbw[14] = length;
  memcpy(s->cbw + 15, cdb, length);
  if (usb_bulk(s->address, s->output, s->output_packet, &s->output_toggle,
               s->cbw, sizeof(s->cbw), false) != (int)sizeof(s->cbw))
    goto failed;
  int received = 0;
  if (size) {
    received = usb_bulk(s->address, input ? s->input : s->output,
                        input ? s->input_packet : s->output_packet,
                        input ? &s->input_toggle : &s->output_toggle, s->data,
                        size, input);
    if (received < 0)
      goto failed;
  }
  memset(s->csw, 0, sizeof(s->csw));
  if (usb_bulk(s->address, s->input, s->input_packet, &s->input_toggle, s->csw,
               sizeof(s->csw), true) != (int)sizeof(s->csw) ||
      read_little32(s->csw) != 0x53425355 ||
      read_little32(s->csw + 4) != s->tag || s->csw[12] > 1 ||
      read_little32(s->csw + 8) > size)
    goto failed;
  if (s->csw[12])
    return 1;
  if ((unsigned)received != size || read_little32(s->csw + 8))
    goto failed;
  return 0;
failed:
  if (!reset(s))
    s->ready = false;
  return -1;
}
bool usb_storage_attach(unsigned address, unsigned packet, unsigned interface,
                        unsigned input, unsigned output, unsigned input_packet,
                        unsigned output_packet) {
  if (unit_count == USB_DISKS || input_packet > 64 || output_packet > 64 ||
      !input_packet || !output_packet)
    return false;
  struct storage *s = &units[unit_count];
  memset(s, 0, sizeof(*s));
  s->address = address;
  s->control_packet = packet;
  s->interface = interface;
  s->input = input;
  s->output = output;
  s->input_packet = input_packet;
  s->output_packet = output_packet;
  uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
  if (command(s, inquiry, sizeof(inquiry), 36, true))
    return false;
  if ((s->data[0] & 31) != 0)
    return false;
  char model[41] = "USB ";
  memcpy(model + 4, s->data + 8, 8);
  model[12] = ' ';
  memcpy(model + 13, s->data + 16, 16);
  model[29] = 0;
  uint8_t ready[6] = {0}, sense[6] = {3, 0, 0, 0, 18, 0};
  bool available = false;
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    int result = command(s, ready, sizeof(ready), 0, false);
    if (!result) {
      available = true;
      break;
    }
    if (result < 0 || command(s, sense, sizeof(sense), 18, true) < 0)
      break;
    sleep_ticks(10);
  }
  if (!available)
    return false;
  uint8_t capacity[10] = {0x25};
  if (command(s, capacity, sizeof(capacity), 8, true))
    return false;
  uint32_t last = big32(s->data), bytes = big32(s->data + 4);
  if (last == UINT32_MAX ||
      (bytes != 512 && bytes != 1024 && bytes != 2048 && bytes != 4096))
    return false;
  s->blocks = last + 1;
  s->block_size = bytes;
  s->ready = true;
  uint64_t sectors = (uint64_t)s->blocks * (bytes / 512);
  struct block_device disk = {.backend = 4,
                              .logical_sector_size = bytes,
                              .unit = unit_count,
                              .sectors =
                                  sectors > UINT32_MAX ? UINT32_MAX : sectors};
  memcpy(disk.model, model, sizeof(model));
  if (!disk_register(&disk)) {
    s->ready = false;
    return false;
  }
  unit_count++;
  console_write("USB mass-storage ready\n");
  return true;
}
bool usb_storage_transfer(unsigned unit, uint32_t sector, void *data,
                          bool write) {
  if (unit >= unit_count || !units[unit].ready)
    return false;
  struct storage *s = &units[unit];
  unsigned ratio = s->block_size / 512, offset = (sector % ratio) * 512;
  uint32_t block = sector / ratio;
  if (block >= s->blocks)
    return false;
  uint8_t cdb[10] = {0x28,  0, block >> 24, block >> 16, block >> 8,
                     block, 0, 0,           1,           0};
  if (!write || ratio != 1)
    if (command(s, cdb, sizeof(cdb), s->block_size, true))
      return false;
  if (!write) {
    memcpy(data, s->data + offset, 512);
    return true;
  }
  memcpy(s->data + offset, data, 512);
  cdb[0] = 0x2a;
  return command(s, cdb, sizeof(cdb), s->block_size, false) == 0;
}
bool usb_storage_flush(unsigned unit) {
  if (unit >= unit_count || !units[unit].ready)
    return false;
  uint8_t cdb[10] = {0x35};
  return command(&units[unit], cdb, sizeof(cdb), 0, false) == 0;
}
