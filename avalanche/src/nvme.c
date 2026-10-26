#include "system.h"
/* NVM Express PCI transport: polled admin/I/O queues, 4 KiB DMA pages.
 * Register/command layout follows the NVM Express base and NVM command specs.
 */
#define QUEUE_DEPTH 16
#define NAMESPACES 8
struct nvme_command {
  uint32_t word[16];
};
struct nvme_completion {
  volatile uint32_t result, reserved, head, status;
};
struct nvme_queue {
  struct nvme_command submission[QUEUE_DEPTH] __attribute__((aligned(4096)));
  struct nvme_completion completion[QUEUE_DEPTH] __attribute__((aligned(4096)));
  unsigned tail, head, phase;
  uint16_t sequence;
};
static struct nvme_queue queues[2] __attribute__((aligned(4096)));
static uint8_t identify[4096] __attribute__((aligned(4096)));
static uint8_t data_page[4096] __attribute__((aligned(4096)));
static struct {
  uint32_t id;
  unsigned shift;
  uint64_t blocks;
} namespaces[NAMESPACES];
static unsigned namespace_count, stride, timeout;
static volatile uint32_t *registers;
static const struct pci_device *controller;
static bool dead;
static uint32_t read_reg(unsigned offset) { return registers[offset / 4]; }
static void write_reg(unsigned offset, uint32_t v) {
  registers[offset / 4] = v;
  __asm__ volatile("" ::: "memory");
}
static void address_reg(unsigned offset, const void *p) {
  uint64_t address = (uintptr_t)p;
  write_reg(offset, address);
  write_reg(offset + 4, address >> 32);
}
static bool ready(bool on) {
  uint32_t start = clock_ticks();
  while ((read_reg(0x1c) & 1) != (unsigned)on) {
    if ((read_reg(0x1c) & 2) || clock_ticks() - start > timeout)
      return false;
    cpu_pause();
  }
  return true;
}
static bool submit(unsigned qid, struct nvme_command *command) {
  if (dead)
    return false;
  struct nvme_queue *q = &queues[qid];
  uint16_t cid = ++q->sequence;
  command->word[0] = (command->word[0] & 65535u) | ((uint32_t)cid << 16);
  q->submission[q->tail] = *command;
  q->tail = (q->tail + 1) % QUEUE_DEPTH;
  __asm__ volatile("sfence" ::: "memory");
  write_reg(0x1000 + (2 * qid) * stride, q->tail);
  uint32_t start = clock_ticks(), status;
  while (((status = q->completion[q->head].status) >> 16 & 1) != q->phase) {
    if ((read_reg(0x1c) & 2) || clock_ticks() - start > timeout) {
      dead = true;
      /* Keep all DMA storage alive and stop further bus-master transactions. */
      pci_write(controller, 4, pci_read(controller, 4) & 0xfffbu);
      return false;
    }
    cpu_pause();
  }
  __asm__ volatile("lfence" ::: "memory");
  bool ok = (status & 65535u) == cid && !(status >> 17);
  q->head = (q->head + 1) % QUEUE_DEPTH;
  if (!q->head)
    q->phase ^= 1;
  write_reg(0x1000 + (2 * qid + 1) * stride, q->head);
  return ok;
}
static void buffer_command(struct nvme_command *c, void *p) {
  c->word[6] = (uintptr_t)p;
  c->word[7] = (uint64_t)(uintptr_t)p >> 32;
}
static bool identify_object(uint32_t id, unsigned kind) {
  memset(identify, 0, sizeof(identify));
  struct nvme_command c = {{6, id}};
  c.word[10] = kind;
  buffer_command(&c, identify);
  return submit(0, &c);
}
void nvme_init(void) {
  controller = pci_find_class(1, 8, 2);
  if (!controller || !controller->bar[0] || (controller->bar[0] & 1))
    return;
  if ((controller->bar[0] & 6) == 4 && controller->bar[1])
    return;
  uint32_t base = controller->bar[0] & ~15u;
  if (!platform_mmio(base, 0x2000))
    return;
  registers = (void *)(uintptr_t)base;
  uint64_t cap = read_reg(0) | (uint64_t)read_reg(4) << 32;
  if ((cap & 65535) + 1 < QUEUE_DEPTH || !(cap & (UINT64_C(1) << 37)) ||
      ((cap >> 48) & 15))
    return;
  stride = 4u << ((cap >> 32) & 15);
  if (stride > 1024)
    return;
  timeout = (((cap >> 24) & 255) + 1) * 50; /* 100 Hz PIT; CAP.TO is 500 ms. */
  if (timeout < 500)
    timeout = 500;
  pci_enable(controller, 6);
  write_reg(0x0c, 0xffffffffu); /* Mask interrupts; queues are polled. */
  write_reg(0x14, read_reg(0x14) & ~1u);
  if (!ready(false))
    return;
  memset(queues, 0, sizeof(queues));
  queues[0].phase = queues[1].phase = 1;
  write_reg(0x24, (QUEUE_DEPTH - 1) | (QUEUE_DEPTH - 1) << 16);
  address_reg(0x28, queues[0].submission);
  address_reg(0x30, queues[0].completion);
  write_reg(0x14, 1u | (6u << 16) | (4u << 20));
  if (!ready(true) || !identify_object(0, 1))
    return;
  char model[41];
  memcpy(model, identify + 24, 40);
  model[40] = 0;
  for (int i = 39; i >= 0 && model[i] == ' '; i--)
    model[i] = 0;
  struct nvme_command c = {{9}};
  c.word[10] = 7; /* One I/O queue pair. */
  if (!submit(0, &c))
    return;
  memset(&c, 0, sizeof(c));
  c.word[0] = 5;
  c.word[10] = 1 | (QUEUE_DEPTH - 1) << 16;
  c.word[11] = 1;
  buffer_command(&c, queues[1].completion);
  if (!submit(0, &c))
    return;
  memset(&c, 0, sizeof(c));
  c.word[0] = 1;
  c.word[10] = 1 | (QUEUE_DEPTH - 1) << 16;
  c.word[11] = 1 | 1u << 16;
  buffer_command(&c, queues[1].submission);
  if (!submit(0, &c) || !identify_object(0, 2))
    return;
  uint32_t active[NAMESPACES];
  memcpy(active, identify, sizeof(active));
  for (unsigned i = 0; i < NAMESPACES && active[i]; i++) {
    if (!identify_object(active[i], 0))
      continue;
    unsigned format = identify[26] & 15;
    unsigned shift = identify[128 + 4 * format + 2];
    uint64_t blocks;
    memcpy(&blocks, identify, 8);
    if (!blocks || (identify[26] & 0xf0) || identify[128 + 4 * format] ||
        identify[128 + 4 * format + 1] || shift < 9 || shift > 12)
      continue;
    unsigned unit = namespace_count;
    namespaces[unit].id = active[i];
    namespaces[unit].shift = shift;
    namespaces[unit].blocks = blocks;
    uint64_t sectors = blocks > (UINT64_MAX >> (shift - 9))
                           ? UINT64_MAX
                           : blocks << (shift - 9);
    struct block_device disk = {.backend = 3,
                                .unit = unit,
                                .sectors = sectors > UINT32_MAX ? UINT32_MAX
                                                                : sectors};
    memcpy(disk.model, model, sizeof(model));
    if (disk_register(&disk))
      namespace_count++;
  }
}
static bool transfer(unsigned unit, uint64_t block, bool write) {
  struct nvme_command c = {{write ? 1 : 2, namespaces[unit].id}};
  c.word[10] = block;
  c.word[11] = block >> 32;
  buffer_command(&c, data_page);
  return submit(1, &c);
}
bool nvme_transfer(unsigned unit, uint32_t sector, void *data, bool write) {
  if (unit >= namespace_count || dead)
    return false;
  unsigned shift = namespaces[unit].shift;
  uint64_t block = sector >> (shift - 9);
  unsigned offset = (sector & ((1u << (shift - 9)) - 1)) * 512;
  if (block >= namespaces[unit].blocks)
    return false;
  if ((!write || shift != 9) && !transfer(unit, block, false))
    return false;
  if (write) {
    memcpy(data_page + offset, data, 512);
    return transfer(unit, block, true);
  }
  memcpy(data, data_page + offset, 512);
  return true;
}
bool nvme_flush(unsigned unit) {
  if (unit >= namespace_count || dead)
    return false;
  struct nvme_command c = {{0, namespaces[unit].id}};
  return submit(1, &c);
}
