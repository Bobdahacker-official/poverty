#include "system.h"
/* AMD PCnet-PCI (Am79C97x), 32-bit software style 2 descriptor rings. */
struct descriptor {
  uint32_t address, status, message, reserved;
} __attribute__((packed));
struct init_block {
  uint16_t mode;
  uint8_t rlen, tlen;
  uint8_t mac[6], reserved[2];
  uint64_t filter;
  uint32_t receive, transmit;
} __attribute__((packed));
static struct descriptor rx[32] __attribute__((aligned(16)));
static struct descriptor tx[8] __attribute__((aligned(16)));
static uint8_t rx_data[32][2048] __attribute__((aligned(16)));
static uint8_t tx_data[8][2048] __attribute__((aligned(16)));
static struct init_block init __attribute__((aligned(16)));
static uint16_t io;
static unsigned rx_next, tx_next;
static bool available;
static const struct pci_device *adapter;
static uint16_t csr(unsigned n) {
  io_out16(io + 0x12, n);
  return io_in16(io + 0x10);
}
static void set_csr(unsigned n, uint16_t v) {
  io_out16(io + 0x12, n);
  io_out16(io + 0x10, v);
}
static uint16_t bcr(unsigned n) {
  io_out16(io + 0x12, n);
  return io_in16(io + 0x16);
}
static void set_bcr(unsigned n, uint16_t v) {
  io_out16(io + 0x12, n);
  io_out16(io + 0x16, v);
}
bool pcnet_init(uint8_t mac[6]) {
  static const uint16_t models[] = {0x2000, 0x2001, 0x2002};
  for (unsigned i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
    adapter = pci_find(0x1022, models[i]);
    if (adapter)
      break;
  }
  if (!adapter || !(adapter->bar[0] & 1))
    return false;
  io = adapter->bar[0] & ~3u;
  pci_enable(adapter, 5);
  (void)io_in16(io + 0x14); /* Reset and enter 16-bit access mode. */
  io_out16(io + 0x10, 0);
  if (csr(0) == 0xffff)
    return false;
  set_csr(0, 4);                      /* STOP */
  set_bcr(20, (bcr(20) & ~255u) | 2); /* 32-bit software style. */
  for (unsigned i = 0; i < 6; i++)
    mac[i] = init.mac[i] = io_in8(io + i);
  init.mode = 0;
  init.rlen = 5 << 4;
  init.tlen = 3 << 4;
  init.filter = 0;
  init.receive = (uintptr_t)rx;
  init.transmit = (uintptr_t)tx;
  memset(rx, 0, sizeof(rx));
  memset(tx, 0, sizeof(tx));
  for (unsigned i = 0; i < 32; i++) {
    rx[i].address = (uintptr_t)rx_data[i];
    rx[i].status = 0x80000000u | ((uint16_t)(-2048) & 0x0fff) | 0xf000;
  }
  for (unsigned i = 0; i < 8; i++)
    tx[i].address = (uintptr_t)tx_data[i];
  rx_next = tx_next = 0;
  set_csr(1, (uintptr_t)&init);
  set_csr(2, (uintptr_t)&init >> 16);
  set_csr(3, 0xffff); /* Keep IRQs masked; network stack polls. */
  set_csr(0, 1);
  uint32_t start = clock_ticks();
  while (!(csr(0) & 0x100)) {
    if (clock_ticks() - start > 100) {
      set_csr(0, 4);
      return false;
    }
    cpu_pause();
  }
  set_csr(0, 0x102); /* Acknowledge IDON, start. */
  available = true;
  return true;
}
bool pcnet_send(const void *data, unsigned size) {
  if (!available || size > 1514 || size < 14)
    return false;
  unsigned i = tx_next;
  uint32_t start = clock_ticks();
  while (tx[i].status & 0x80000000u) {
    if (clock_ticks() - start > 100)
      return false;
    cpu_pause();
  }
  if (tx[i].status & 0x40000000u)
    return false;
  memcpy(tx_data[i], data, size);
  tx[i].message = 0;
  tx[i].status = 0x83000000u | ((uint16_t)(-(int)size) & 0x0fff) | 0xf000;
  __asm__ volatile("sfence" ::: "memory");
  set_csr(0, 0x48); /* Transmit demand, clear transmit interrupt. */
  tx_next = (i + 1) % 8;
  return true;
}
void pcnet_poll(void (*receive)(const uint8_t *, uint32_t)) {
  if (!available)
    return;
  for (unsigned count = 0; count < 32; count++) {
    struct descriptor *d = &rx[rx_next];
    uint32_t status = d->status;
    if (status & 0x80000000u)
      break;
    __asm__ volatile("lfence" ::: "memory");
    unsigned length = d->message & 0x0fff;
    if ((status & 0x03000000u) == 0x03000000u && !(status & 0x40000000u) &&
        length >= 18 && length <= 1518)
      receive(rx_data[rx_next], length - 4);
    d->message = 0;
    d->status = 0x80000000u | ((uint16_t)(-2048) & 0x0fff) | 0xf000;
    rx_next = (rx_next + 1) % 32;
  }
  set_csr(0, csr(0) & 0x7f00); /* Clear completed interrupt flags. */
}
