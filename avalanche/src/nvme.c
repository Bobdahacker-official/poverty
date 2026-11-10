#include "system.h"
#include "nvme_health.h"
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
static uint8_t health_page[4096] __attribute__((aligned(4096)));
static char controller_model[41], controller_serial[21], controller_firmware[9];
static struct nvme_health health_cache;
static bool health_valid, health_busy;
static uint32_t admin_status;
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
  uint64_t address = page_physical(p);
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
  if (!qid) admin_status = status >> 17;
  bool ok = (status & 65535u) == cid && !(status >> 17);
  q->head = (q->head + 1) % QUEUE_DEPTH;
  if (!q->head)
    q->phase ^= 1;
  write_reg(0x1000 + (2 * qid + 1) * stride, q->head);
  return ok;
}
static void buffer_command(struct nvme_command *c, void *p) {
  c->word[6] = page_physical(p);
  c->word[7] = page_physical(p) >> 32;
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
  uint64_t base = pci_memory_bar(controller, 0);
  if (!base || !platform_mmio(base, 0x2000))
    return;
  pci_enable(controller, 6);
  registers = page_pointer(base);
  uint64_t cap = read_reg(0) | (uint64_t)read_reg(4) << 32;
  if ((cap & 65535) + 1 < QUEUE_DEPTH || !(cap & (UINT64_C(1) << 37)) ||
      ((cap >> 48) & 15))
    return;
  stride = 4u << ((cap >> 32) & 15);
  if (!platform_mmio(base, 0x1000 + 3 * stride + 4))
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
  memcpy(controller_model, model, sizeof(model));
  memcpy(controller_serial, identify + 4, 20); controller_serial[20] = 0;
  memcpy(controller_firmware, identify + 64, 8); controller_firmware[8] = 0;
  for (int i = 19; i >= 0 && controller_serial[i] == ' '; i--) controller_serial[i] = 0;
  for (int i = 7; i >= 0 && controller_firmware[i] == ' '; i--) controller_firmware[i] = 0;
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
                                .logical_sector_size = 1u << shift,
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

static uint16_t health_u16(unsigned at) {
  return (uint16_t)health_page[at] | (uint16_t)health_page[at + 1] << 8;
}
static uint32_t health_u32(unsigned at) {
  return (uint32_t)health_u16(at) | (uint32_t)health_u16(at + 2) << 16;
}
int nvme_health_read(unsigned unit, struct nvme_health *out, bool refresh) {
  if (!out) return -22;
  if (!registers || !namespace_count || unit >= namespace_count) return -19;
  if (dead) return -5;
  if (health_busy) return -16;
  if (refresh || !health_valid || (uint32_t)(clock_ticks() - health_cache.sampled_ticks) >= 500) {
    health_busy = true;
    memset(health_page, 0, sizeof(health_page));
    struct nvme_command command = {{2, 0xffffffffu}};
    /* Controller-wide SMART/Health log, retaining asynchronous events.
       NUMD is a zero-based count of DWORDs: 512 bytes -> 127. */
    command.word[10] = (127u << 16) | (1u << 15) | 2;
    buffer_command(&command, health_page);
    bool success = submit(0, &command);
    if (!success && !dead && (admin_status & 0x7ff) == 2) {
      /* Older controllers reserve RAE. No asynchronous events are armed by
         this driver, so retrying without retention does not lose a consumer. */
      command.word[10] &= ~(1u << 15);
      success = submit(0, &command);
    }
    if (!success) {
      health_busy = false;
      return !dead && ((admin_status & 255) == 1 || (admin_status & 255) == 2) ? -95 : -5;
    }
    struct nvme_health sample;
    memset(&sample, 0, sizeof(sample));
    sample.version = 1;
    sample.sampled_ticks = clock_ticks();
    memcpy(sample.model, controller_model, sizeof(sample.model));
    memcpy(sample.serial, controller_serial, sizeof(sample.serial));
    memcpy(sample.firmware, controller_firmware, sizeof(sample.firmware));
    sample.critical_warning = health_page[0];
    sample.temperature_kelvin = health_u16(1);
    sample.available_spare = health_page[3];
    sample.spare_threshold = health_page[4];
    sample.percentage_used = health_page[5];
    memcpy(sample.counters, health_page + 32, sizeof(sample.counters));
    sample.warning_temperature_minutes = health_u32(192);
    sample.critical_temperature_minutes = health_u32(196);
    for (unsigned i = 0; i < 8; i++) sample.temperature_sensors[i] = health_u16(200 + i * 2);
    for (unsigned i = 0; i < 2; i++) {
      sample.thermal_transitions[i] = health_u32(216 + i * 4);
      sample.thermal_seconds[i] = health_u32(224 + i * 4);
    }
    health_cache = sample;
    health_valid = true;
    health_busy = false;
  }
  *out = health_cache;
  out->namespace_id = namespaces[unit].id;
  out->namespace_blocks = namespaces[unit].blocks;
  out->logical_block_size = 1u << namespaces[unit].shift;
  return 0;
}
void nvme_counter_format(char *out, unsigned capacity, const uint8_t *counter) {
  uint8_t value[16]; char reversed[40]; unsigned count = 0;
  memcpy(value, counter, sizeof(value));
  for (;;) {
    unsigned remainder = 0; bool nonzero = false;
    for (int i = 15; i >= 0; i--) {
      unsigned word = (remainder << 8) | value[i];
      value[i] = word / 10; remainder = word % 10;
      nonzero |= value[i] != 0;
    }
    reversed[count++] = '0' + remainder;
    if (!nonzero) break;
  }
  if (!capacity) return;
  unsigned used = 0;
  while (count && used + 1 < capacity) out[used++] = reversed[--count];
  out[used] = 0;
}
static void health_number(const char *label, uint64_t value) {
  uint8_t bytes[16] = {0}; char number[40];
  for (unsigned i = 0; i < 8; i++) { bytes[i] = value; value >>= 8; }
  nvme_counter_format(number, sizeof(number), bytes);
  console_write(label); console_write(number); console_putc('\n');
}
void nvme_health_command(char *arguments) {
  while (*arguments == ' ') arguments++;
  bool all = !*arguments, valid = false;
  unsigned selected = all ? 0 : parse_uint(arguments, &valid);
  if (!all && !valid) { console_write("nvmehealth [DISK_INDEX]\n"); return; }
  bool found = false;
  for (unsigned index = 0; index < disk_count(); index++) {
    const struct block_device *disk = disk_get(index);
    if ((!all && index != selected) || !disk || disk->backend != 3) continue;
    found = true; health_number("Disk: ", index);
    struct nvme_health sample;
    int result = nvme_health_read(disk->unit, &sample, !all || !health_valid);
    if (result < 0) { console_write("Health log error: "); print_uint(-result); console_putc('\n'); continue; }
    console_write(sample.model); console_putc('\n');
    console_write("Serial: "); console_write(sample.serial); console_putc('\n');
    console_write("Firmware: "); console_write(sample.firmware); console_putc('\n');
    health_number("Namespace: ", sample.namespace_id);
    health_number("Logical blocks: ", sample.namespace_blocks);
    health_number("Logical block bytes: ", sample.logical_block_size);
    health_number("Critical warning bits: ", sample.critical_warning);
    if (sample.temperature_kelvin) {
      int celsius = (int)sample.temperature_kelvin - 273;
      console_write("Temperature C: ");
      if (celsius < 0) console_putc('-');
      health_number("", celsius < 0 ? -celsius : celsius);
    } else console_write("Temperature: not reported by controller\n");
    health_number("Available spare percent: ", sample.available_spare);
    health_number("Spare threshold percent: ", sample.spare_threshold);
    health_number("Endurance used percent: ", sample.percentage_used);
    static const char *labels[NVME_HEALTH_COUNTERS] = {"Data units read", "Data units written", "Host reads", "Host writes", "Controller busy minutes", "Power cycles", "Power-on hours", "Unsafe shutdowns", "Media integrity errors", "Error log entries"};
    for (unsigned i = 0; i < NVME_HEALTH_COUNTERS; i++) {
      char number[40]; nvme_counter_format(number, sizeof(number), sample.counters[i]);
      console_write(labels[i]); console_write(": "); console_write(number); console_putc('\n');
    }
    console_write("Data units are 512,000 bytes, rounded up by the controller.\n");
    health_number("Warning temperature minutes: ", sample.warning_temperature_minutes);
    health_number("Critical temperature minutes: ", sample.critical_temperature_minutes);
    for (unsigned i = 0; i < 8; i++) if (sample.temperature_sensors[i]) {
      health_number("Sensor index: ", i + 1); health_number("Sensor Kelvin: ", sample.temperature_sensors[i]);
    }
    for (unsigned i = 0; i < 2; i++) {
      health_number("Thermal management level: ", i + 1);
      health_number("Transitions: ", sample.thermal_transitions[i]);
      health_number("Total seconds: ", sample.thermal_seconds[i]);
    }
  }
  if (!found) console_write("No NVMe namespace matches this selection\n");
}
