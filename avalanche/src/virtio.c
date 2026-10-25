#include "virtio.h"
static uint8_t block_memory[32768] __attribute__((aligned(4096)));
static uint8_t rx_memory[32768] __attribute__((aligned(4096)));
static uint8_t tx_memory[32768] __attribute__((aligned(4096)));
static struct virtq blockq, rxq, txq;
static bool block_ready, block_flushable, block_readonly, net_ready;
static uint16_t block_port, net_port;
static uint8_t block_buffer[512] __attribute__((aligned(512)));
static struct {
  uint32_t type, reserved;
  uint64_t sector;
} block_header;
static volatile uint8_t block_status;
static uint8_t rx_buffers[32][1546] __attribute__((aligned(16)));
static uint8_t tx_buffer[1546] __attribute__((aligned(16)));
static bool begin(const struct pci_device *d, uint32_t features,
                  uint16_t *port) {
  if (!d || !(d->bar[0] & 1))
    return false;
  *port = (uint16_t)(d->bar[0] & ~3u);
  pci_enable(d, 5);
  io_out8(*port + 18, 0);
  for (unsigned i = 0; i < 10000; i++) {
    if (!io_in8(*port + 18))
      break;
    if (i == 9999)
      return false;
  }
  io_out8(*port + 18, 1);
  io_out8(*port + 18, 3);
  uint32_t host = io_in32(*port);
  if ((features & host) != features) {
    io_out8(*port + 18, 0x83);
    return false;
  }
  io_out32(*port + 4, features);
  return true;
}
static bool queue(struct virtq *q, uint16_t port, uint16_t index,
                  uint8_t *memory) {
  io_out16(port + 14, index);
  uint16_t n = io_in16(port + 12);
  if (n < 4 || n > 1024 || (n & (n - 1)) || io_in32(port + 8))
    return false;
  memset(memory, 0, 32768);
  q->port = port;
  q->index = index;
  q->size = n;
  q->last = 0;
  q->desc = (void *)memory;
  q->avail = (void *)(memory + 16 * n);
  uint32_t offset = (16 * n + 4 + 2 * n + 4095) & ~4095u;
  q->used = (void *)(memory + offset);
  q->avail[0] = 1;
  io_out32(port + 8, (uint32_t)memory >> 12);
  return true;
}
static void submit(struct virtq *q, uint16_t descriptor) {
  uint16_t index = q->avail[1];
  q->avail[2 + (index % q->size)] = descriptor;
  __asm__ volatile("" ::: "memory");
  q->avail[1] = index + 1;
  __asm__ volatile("" ::: "memory");
  io_out16(q->port + 16, q->index);
}
static bool wait_used(struct virtq *q) {
  uint32_t start = clock_ticks();
  while (q->used[1] == q->last) {
    if (clock_ticks() - start > 300)
      return false;
    cpu_pause();
  }
  __asm__ volatile("" ::: "memory");
  volatile struct virtq_used *ring = (void *)(q->used + 2);
  bool valid = ring[q->last % q->size].id == 0;
  q->last++;
  (void)io_in8(q->port + 19);
  return valid;
}
static bool block_request(uint32_t type, uint32_t lba, void *buffer) {
  if (!block_ready)
    return false;
  block_header.type = type;
  block_header.reserved = 0;
  block_header.sector = lba;
  block_status = 0xff;
  if (type == 1)
    memcpy(block_buffer, buffer, 512);
  blockq.desc[0] = (struct virtq_desc){
      (uint32_t)&block_header, sizeof(block_header), 1, type == 4 ? 2 : 1};
  blockq.desc[1] = (struct virtq_desc){(uint32_t)block_buffer, 512,
                                       (uint16_t)(type == 0 ? 3 : 1), 2};
  blockq.desc[2] = (struct virtq_desc){(uint32_t)&block_status, 1, 2, 0};
  submit(&blockq, 0);
  if (!wait_used(&blockq)) {
    io_out8(block_port + 18, 0);
    block_ready = false;
    return false;
  }
  if (block_status != 0)
    return false;
  if (type == 0)
    memcpy(buffer, block_buffer, 512);
  return true;
}
void virtio_block_init(void) {
  const struct pci_device *d = pci_find(0x1af4, 0x1001);
  if (!d || !(d->bar[0] & 1))
    return;
  uint16_t port = (uint16_t)(d->bar[0] & ~3u);
  uint32_t host = io_in32(port),
           selected = host & ((1u << 5) | (1u << 6) | (1u << 9));
  if (!begin(d, selected, &block_port) ||
      !queue(&blockq, block_port, 0, block_memory))
    return;
  uint32_t low = io_in32(block_port + 20), high = io_in32(block_port + 24);
  if ((selected & (1u << 6)) && io_in32(block_port + 40) != 512) {
    io_out8(block_port + 18, 0x83);
    return;
  }
  block_readonly = (selected & (1u << 5)) != 0;
  block_flushable = (selected & (1u << 9)) != 0;
  io_out8(block_port + 18, 7);
  block_ready = true;
  struct block_device b = {.backend = 2,
                           .read_only = block_readonly || !block_flushable,
                           .sectors = high ? 0xffffffffu : low};
  const char name[] = "VirtIO legacy block";
  memcpy(b.model, name, sizeof(name));
  if (!b.sectors || !disk_register(&b)) {
    io_out8(block_port + 18, 0);
    block_ready = false;
  }
}
bool virtio_block_transfer(uint32_t lba, void *data, bool write) {
  if (write && (block_readonly || !block_flushable))
    return false;
  return block_request(write ? 1 : 0, lba, data);
}
bool virtio_block_flush(void) {
  return block_flushable && block_request(4, 0, NULL);
}
bool virtio_net_init(uint8_t *mac) {
  const struct pci_device *d = pci_find(0x1af4, 0x1000);
  if (!begin(d, 1u << 5, &net_port) || !queue(&rxq, net_port, 0, rx_memory) ||
      !queue(&txq, net_port, 1, tx_memory) || rxq.size < 32)
    return false;
  for (unsigned i = 0; i < 6; i++)
    mac[i] = io_in8(net_port + 20 + i);
  for (unsigned i = 0; i < 32; i++) {
    rxq.desc[i] = (struct virtq_desc){(uint32_t)rx_buffers[i],
                                      sizeof(rx_buffers[i]), 2, 0};
    rxq.avail[2 + i] = i;
  }
  rxq.avail[1] = 32;
  io_out8(net_port + 18, 7);
  io_out16(net_port + 16, 0);
  net_ready = true;
  return true;
}
bool virtio_net_send(const void *frame, uint32_t n) {
  if (!net_ready || n > 1514 || n < 14)
    return false;
  unsigned padded = n < 60 ? 60 : n;
  memset(tx_buffer, 0, 10 + padded);
  memcpy(tx_buffer + 10, frame, n);
  txq.desc[0] = (struct virtq_desc){(uint32_t)tx_buffer, 10 + padded, 0, 0};
  submit(&txq, 0);
  if (!wait_used(&txq)) {
    io_out8(net_port + 18, 0);
    net_ready = false;
    return false;
  }
  return true;
}
void virtio_net_poll(void (*receive)(const uint8_t *, uint32_t)) {
  if (!net_ready)
    return;
  for (unsigned count = 0; count < 32 && rxq.used[1] != rxq.last; count++) {
    __asm__ volatile("" ::: "memory");
    volatile struct virtq_used *ring = (void *)(rxq.used + 2);
    uint32_t i = ring[rxq.last % rxq.size].id,
             n = ring[rxq.last % rxq.size].length;
    rxq.last++;
    if (i >= 32) {
      io_out8(net_port + 18, 0);
      net_ready = false;
      return;
    }
    if (n >= 24 && n <= sizeof(rx_buffers[i]))
      receive(rx_buffers[i] + 10, n - 10);
    submit(&rxq, (uint16_t)i);
  }
  (void)io_in8(net_port + 19);
}
