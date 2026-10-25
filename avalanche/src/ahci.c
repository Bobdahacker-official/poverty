#include "system.h"
#define PORTS 8u
struct command_header {
  uint16_t flags, prdt_count;
  volatile uint32_t transferred;
  uint32_t table, table_high, reserved[4];
} __attribute__((packed));
struct command_table {
  uint8_t fis[64], atapi[16], reserved[48];
  uint32_t address, address_high, reserved2, bytes;
  uint8_t pad[112];
} __attribute__((packed));
_Static_assert(sizeof(struct command_header) == 32, "AHCI header ABI");
_Static_assert(sizeof(struct command_table) == 256, "AHCI table ABI");
static struct command_header lists[PORTS][32] __attribute__((aligned(1024)));
static struct command_table tables[PORTS] __attribute__((aligned(128)));
static uint8_t received_fis[PORTS][256] __attribute__((aligned(256)));
static uint16_t identify[PORTS][256] __attribute__((aligned(512)));
static uint8_t bounce[PORTS][512] __attribute__((aligned(512)));
static bool failed[PORTS];
static volatile uint32_t *ports[PORTS];
static unsigned count;
static bool wait_clear(volatile uint32_t *reg, uint32_t bits,
                       uint32_t timeout) {
  uint32_t start = clock_ticks();
  while (*reg & bits) {
    if (clock_ticks() - start >= timeout)
      return false;
    cpu_pause();
  }
  return true;
}
static bool execute(unsigned unit, uint8_t opcode, uint32_t lba, void *data,
                    bool write) {
  if (unit >= count || failed[unit])
    return false;
  volatile uint32_t *p = ports[unit];
  if (!wait_clear(p + 0x20 / 4, 0x88, 300) || p[0x38 / 4])
    return false;
  struct command_header *h = &lists[unit][0];
  struct command_table *t = &tables[unit];
  memset(t, 0, sizeof(*t));
  h->flags = 5 | (write ? 1 << 6 : 0);
  h->prdt_count = data ? 1 : 0;
  h->transferred = 0;
  h->table = (uint32_t)t;
  h->table_high = 0;
  if (data) {
    t->address = (uint32_t)bounce[unit];
    if (write)
      memcpy(bounce[unit], data, 512);
    t->bytes = 511;
  }
  t->fis[0] = 0x27;
  t->fis[1] = 0x80;
  t->fis[2] = opcode;
  t->fis[4] = lba;
  t->fis[5] = lba >> 8;
  t->fis[6] = lba >> 16;
  t->fis[7] = 0x40;
  t->fis[8] = lba >> 24;
  if (data)
    t->fis[12] = 1;
  p[0x10 / 4] = 0xffffffffu;
  __asm__ volatile("" ::: "memory");
  p[0x38 / 4] = 1;
  uint32_t start = clock_ticks();
  while (p[0x38 / 4] & 1) {
    if ((p[0x10 / 4] & (1u << 30)) || clock_ticks() - start > 500)
      goto error;
    cpu_pause();
  }
  __asm__ volatile("" ::: "memory");
  if ((p[0x10 / 4] & (1u << 30)) || (p[0x20 / 4] & 1) ||
      (data && h->transferred != 512))
    goto error;
  if (data && !write)
    memcpy(data, bounce[unit], 512);
  return true;
error:
  /* A failed DMA command must never retain a caller's buffer or reuse its slot.
   */
  failed[unit] = true;
  p[0x18 / 4] &= ~1u;
  (void)wait_clear(p + 0x18 / 4, 1u << 15, 100);
  return false;
}
void ahci_init(void) {
  const struct pci_device *d = pci_find_class(1, 6, 1);
  if (!d || (d->bar[5] & 1) || !d->bar[5])
    return;
  if (!platform_mmio(d->bar[5] & ~15u, 0x1100))
    return;
  volatile uint32_t *hba = (void *)(d->bar[5] & ~15u);
  pci_enable(d, 6);
  if (hba[0x24 / 4] & 1) {
    hba[0x28 / 4] |= 2;
    if (!wait_clear(hba + 0x28 / 4, 0x11, 500))
      return;
  }
  hba[1] = (hba[1] | 0x80000000u) & ~2u;
  uint32_t implemented = hba[3];
  for (unsigned port = 0; port < 32 && count < PORTS; port++) {
    if (!(implemented & (1u << port)))
      continue;
    volatile uint32_t *p = hba + (0x100 + port * 0x80) / 4;
    if ((p[0x28 / 4] & 0xf0f) != 0x103 || p[0x24 / 4] != 0x101)
      continue;
    p[0x18 / 4] &= ~1u;
    if (!wait_clear(p + 0x18 / 4, 1u << 15, 100))
      continue;
    p[0x18 / 4] &= ~(1u << 4);
    if (!wait_clear(p + 0x18 / 4, 1u << 14, 100))
      continue;
    unsigned unit = count;
    memset(lists[unit], 0, sizeof(lists[unit]));
    memset(received_fis[unit], 0, 256);
    p[0] = (uint32_t)lists[unit];
    p[1] = 0;
    p[2] = (uint32_t)received_fis[unit];
    p[3] = 0;
    p[0x14 / 4] = 0;
    p[0x30 / 4] = 0xffffffffu;
    p[0x10 / 4] = 0xffffffffu;
    ports[count++] = p;
    p[0x18 / 4] |= (1u << 4) | 1u;
    if (!execute(unit, 0xec, 0, identify[unit], false)) {
      p[0x18 / 4] &= ~1u;
      continue;
    }
    uint16_t *w = identify[unit];
    if (!(w[83] & (1 << 10)) ||
        ((w[106] & 0xd000) == 0x5000 &&
         ((uint32_t)w[117] | ((uint32_t)w[118] << 16)) != 256)) {
      p[0x18 / 4] &= ~1u;
      continue;
    }
    uint64_t sectors = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
                       ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    if (!sectors) {
      p[0x18 / 4] &= ~1u;
      continue;
    }
    struct block_device block = {
        .backend = 1,
        .unit = unit,
        .sectors = sectors > 0xffffffffu ? 0xffffffffu : (uint32_t)sectors};
    for (unsigned k = 0; k < 20; k++) {
      block.model[k * 2] = w[27 + k] >> 8;
      block.model[k * 2 + 1] = w[27 + k] & 255;
    }
    for (int k = 39; k >= 0 && block.model[k] == ' '; k--)
      block.model[k] = 0;
    if (!disk_register(&block)) {
      p[0x18 / 4] &= ~1u;
      break;
    }
  }
}
bool ahci_transfer(unsigned unit, uint32_t lba, void *data, bool write) {
  return execute(unit, write ? 0x35 : 0x25, lba, data, write);
}
bool ahci_flush(unsigned unit) { return execute(unit, 0xea, 0, NULL, false); }
