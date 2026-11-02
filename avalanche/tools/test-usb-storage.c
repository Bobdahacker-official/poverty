#include "../src/usb_storage.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t medium[8192], pending[16];
static uint32_t pending_tag, pending_size;
static unsigned phase, controls, faults, flushes;
static struct block_device registered;
void console_write(const char *text) { (void)text; }
void sleep_ticks(uint32_t ticks) { (void)ticks; }
bool disk_register(const struct block_device *disk) {
  registered = *disk;
  return true;
}
bool usb_control(unsigned address, bool low, unsigned packet, uint8_t type,
                 uint8_t request, uint16_t value, uint16_t index, uint16_t size,
                 bool input) {
  assert(address == 2 && !low && packet == 8 && !size && !input);
  if (request == 0xff) {
    assert(type == 0x21 && !value && index == 3);
    phase = 0;
  } else {
    assert(type == 2 && request == 1 && !value &&
           (index == 0x81 || index == 2));
  }
  controls++;
  return true;
}
int usb_bulk(unsigned address, unsigned endpoint, unsigned packet,
             unsigned *toggle, void *data, unsigned size, bool input) {
  assert(address == 2 && packet == 64 && endpoint == (input ? 1u : 2u));
  *toggle ^= ((size + packet - 1) / packet) & 1;
  uint8_t *bytes = data;
  if (!phase) {
    assert(!input && size == 31 && read_little32(bytes) == 0x43425355);
    pending_tag = read_little32(bytes + 4);
    pending_size = read_little32(bytes + 8);
    memcpy(pending, bytes + 15, sizeof(pending));
    phase = pending_size ? 1 : 2;
    return size;
  }
  if (phase == 1) {
    assert(size == pending_size);
    unsigned block = big32(pending + 2);
    if (input)
      memset(bytes, 0, size);
    switch (pending[0]) {
    case 0x12:
      assert(input && size == 36);
      memcpy(bytes + 8, "TEST USB", 8);
      memcpy(bytes + 16, "NATIVE 4K DISK  ", 16);
      break;
    case 0x25:
      assert(input && size == 8);
      bytes[3] = 1;
      bytes[6] = 0x10; /* Two 4096-byte blocks. */
      break;
    case 0x28:
      assert(input && block < 2 && size == 4096);
      memcpy(bytes, medium + block * 4096, size);
      break;
    case 0x2a:
      assert(!input && block < 2 && size == 4096);
      memcpy(medium + block * 4096, bytes, size);
      break;
    default:
      assert(0);
    }
    phase = 2;
    return size;
  }
  assert(phase == 2 && input && size == 13);
  memset(bytes, 0, size);
  little32(bytes, faults == 1 ? 0 : 0x53425355);
  little32(bytes + 4, pending_tag + (faults == 2));
  if (faults == 3)
    bytes[12] = 2;
  if (faults == 4)
    little32(bytes + 8, pending_size + 1);
  if (faults == 5)
    bytes[12] = 1;
  if (pending[0] == 0x35)
    flushes++;
  phase = 0;
  return size;
}
int main(void) {
  for (unsigned i = 0; i < sizeof(medium); i++)
    medium[i] = (i * 7 + 3) & 255;
  assert(usb_storage_attach(2, 8, 3, 1, 2, 64, 64));
  assert(registered.backend == 4 && registered.sectors == 16);
  uint8_t data[512], original[8192];
  assert(usb_storage_transfer(0, 11, data, false));
  assert(!memcmp(data, medium + 11 * 512, sizeof(data)));
  memcpy(original, medium, sizeof(medium));
  memset(data, 0xb5, sizeof(data));
  assert(usb_storage_transfer(0, 3, data, true));
  assert(!memcmp(medium, original, 3 * 512));
  assert(!memcmp(medium + 3 * 512, data, sizeof(data)));
  assert(
      !memcmp(medium + 4 * 512, original + 4 * 512, sizeof(medium) - 4 * 512));
  assert(usb_storage_flush(0) && flushes == 1);
  assert(!usb_storage_transfer(0, 16, data, false));
  assert(!usb_storage_transfer(1, 0, data, false));
  puts(
      "PASS USB 4 KiB sector read/modify/write preserves neighbouring sectors");
  for (faults = 1; faults <= 4; faults++) {
    controls = 0;
    assert(!usb_storage_transfer(0, 0, data, false));
    assert(controls == 3 && units[0].ready);
    assert(!units[0].input_toggle && !units[0].output_toggle);
  }
  faults = 5;
  controls = 0;
  assert(!usb_storage_transfer(0, 0, data, false));
  assert(!controls); /* A SCSI check condition is not a BOT phase error. */
  faults = 0;
  assert(usb_storage_transfer(0, 0, data, false));
  puts("PASS USB malformed status/tag/residue rejection and reset recovery");
  return 0;
}
