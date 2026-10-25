#ifndef AVALANCHE_VIRTIO_H
#define AVALANCHE_VIRTIO_H
#include "system.h"
struct virtq_desc {
  uint64_t address;
  uint32_t length;
  uint16_t flags, next;
} __attribute__((packed));
struct virtq_used {
  uint32_t id, length;
} __attribute__((packed));
struct virtq {
  uint16_t port, index, size, last;
  struct virtq_desc *desc;
  volatile uint16_t *avail;
  volatile uint16_t *used;
};
void virtio_block_init(void);
bool virtio_block_transfer(uint32_t, void *, bool);
bool virtio_block_flush(void);
bool virtio_net_init(uint8_t *);
bool virtio_net_send(const void *, uint32_t);
void virtio_net_poll(void (*)(const uint8_t *, uint32_t));
#endif
