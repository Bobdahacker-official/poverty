#include "system.h"
#include "virtio.h"
#define PCI_MAX 1024
extern uint64_t pci_ecam_bus(unsigned);
static struct pci_device pci[PCI_MAX];
static unsigned pci_count;
static struct block_device disks[64];
static unsigned ndisks;
uint32_t pci_read(const struct pci_device *d, uint8_t r) {
  uint64_t base = pci_ecam_bus(d->bus);
  if (base) return *(volatile uint32_t *)(uintptr_t)(base + ((unsigned)d->slot<<15) + ((unsigned)d->function<<12) + (r&0xfc));
  io_out32(0xcf8, 0x80000000u | ((uintptr_t)d->bus << 16) |
                      ((uintptr_t)d->slot << 11) |
                      ((uintptr_t)d->function << 8) | (r & 0xfc));
  return io_in32(0xcfc);
}
void pci_write(const struct pci_device *d, uint8_t r, uint32_t v) {
  uint64_t base = pci_ecam_bus(d->bus);
  if (base) {
    *(volatile uint32_t *)(uintptr_t)(base + ((unsigned)d->slot<<15) + ((unsigned)d->function<<12) + (r&0xfc)) = v;
    return;
  }
  io_out32(0xcf8, 0x80000000u | ((uintptr_t)d->bus << 16) |
                      ((uintptr_t)d->slot << 11) |
                      ((uintptr_t)d->function << 8) | (r & 0xfc));
  io_out32(0xcfc, v);
}
void pci_enable(const struct pci_device *d, uint16_t flags) {
  uint32_t old =
      pci_read(d, 4); /* Do not write ones into PCI status W1C bits. */
  pci_write(d, 4, (old & 65535u) | flags);
}
const struct pci_device *pci_find(uint16_t vendor, uint16_t device) {
  for (unsigned i = 0; i < pci_count; i++)
    if (pci[i].vendor == vendor && pci[i].device == device)
      return &pci[i];
  return NULL;
}
const struct pci_device *pci_find_class(uint8_t c, uint8_t s, uint8_t p) {
  for (unsigned i = 0; i < pci_count; i++)
    if (pci[i].class_id == c && pci[i].subclass == s && pci[i].prog_if == p)
      return &pci[i];
  return NULL;
}
bool disk_register(const struct block_device *d) {
  if (ndisks >= 64)
    return false;
  disks[ndisks++] = *d;
  return true;
}
static void pci_init(void) {
  for (unsigned b = 0; b < 256; b++)
    for (unsigned s = 0; s < 32; s++) {
      struct pci_device d = {.bus = b, .slot = s};
      if ((pci_read(&d, 0) & 65535) == 65535)
        continue;
      unsigned functions = (pci_read(&d, 12) & 0x00800000u) ? 8 : 1;
      for (unsigned f = 0; f < functions; f++) {
        d.function = f;
        uint32_t v = pci_read(&d, 0);
        if ((v & 65535) == 65535)
          continue;
        d.vendor = v;
        d.device = v >> 16;
        v = pci_read(&d, 8);
        d.class_id = v >> 24;
        d.subclass = v >> 16;
        d.prog_if = v >> 8;
        d.irq = pci_read(&d, 0x3c) & 255;
        unsigned bars = ((pci_read(&d, 12) >> 16) & 0x7f) == 0 ? 6 : 2;
        for (unsigned k = 0; k < 6; k++)
          d.bar[k] = k < bars ? pci_read(&d, 16 + k * 4) : 0;
        if (pci_count < PCI_MAX)
          pci[pci_count++] = d;
      }
    }
}
static void ata_delay(const struct block_device *d) {
  for (unsigned i = 0; i < 4; i++)
    io_in8(d->control);
}
static bool ata_wait(const struct block_device *d, bool drq) {
  for (unsigned i = 0; i < 2000000; i++) {
    uint8_t s = io_in8(d->port + 7);
    if (s == 0xff || s == 0)
      return false;
    if (!(s & 0x80)) {
      if (s & 0x21)
        return false;
      if (!drq || (s & 8))
        return true;
    }
    cpu_pause();
  }
  return false;
}
static void ata_init(void) {
  for (unsigned channel = 0; channel < 2; channel++)
    for (unsigned slave = 0; slave < 2; slave++) {
      struct block_device d = {.port = channel ? 0x170 : 0x1f0,
                               .control = channel ? 0x376 : 0x3f6,
                               .slave = slave};
      io_out8(d.control, 2);
      io_out8(d.port + 6, 0xa0 | (slave << 4));
      ata_delay(&d);
      io_out8(d.port + 2, 0);
      io_out8(d.port + 3, 0);
      io_out8(d.port + 4, 0);
      io_out8(d.port + 5, 0);
      io_out8(d.port + 7, 0xec);
      ata_delay(&d);
      if (!ata_wait(&d, true))
        continue;
      uint16_t words[256];
      for (unsigned i = 0; i < 256; i++)
        words[i] = io_in16(d.port);
      if (!(words[49] & (1 << 9)))
        continue;
      if ((words[106] & 0xd000) == 0x5000 &&
          ((uintptr_t)words[117] | ((uintptr_t)words[118] << 16)) != 256)
        continue;
      d.sectors = (uintptr_t)words[60] | ((uintptr_t)words[61] << 16);
      if (d.sectors > 0x10000000u)
        d.sectors = 0x10000000u;
      for (unsigned i = 0; i < 20; i++) {
        d.model[i * 2] = words[27 + i] >> 8;
        d.model[i * 2 + 1] = words[27 + i] & 255;
      }
      d.model[40] = 0;
      for (int i = 39; i >= 0 && d.model[i] == ' '; i--)
        d.model[i] = 0;
      if (d.sectors)
        disk_register(&d);
    }
}
unsigned disk_count(void) { return ndisks; }
const struct block_device *disk_get(unsigned n) {
  return n < ndisks ? &disks[n] : NULL;
}
static bool ata_select(unsigned n, uint32_t lba) {
  if (n >= ndisks || lba >= disks[n].sectors)
    return false;
  struct block_device *d = &disks[n];
  io_out8(d->port + 6, 0xe0 | (d->slave << 4) | ((lba >> 24) & 15));
  ata_delay(d);
  if (!ata_wait(d, false))
    return false;
  io_out8(d->port + 2, 1);
  io_out8(d->port + 3, lba);
  io_out8(d->port + 4, lba >> 8);
  io_out8(d->port + 5, lba >> 16);
  return true;
}
bool disk_raw_read(unsigned n, uint32_t lba, void *buf) {
  if (n < ndisks && disks[n].backend == 3) return lba < disks[n].sectors && nvme_transfer(disks[n].unit,lba,buf,false);
  if (n < ndisks && disks[n].backend == 2)
    return lba < disks[n].sectors && virtio_block_transfer(lba, buf, false);
  if (n < ndisks && disks[n].backend == 1)
    return lba < disks[n].sectors &&
           ahci_transfer(disks[n].unit, lba, buf, false);
  if (!ata_select(n, lba))
    return false;
  struct block_device *d = &disks[n];
  io_out8(d->port + 7, 0x20);
  ata_delay(d);
  if (!ata_wait(d, true))
    return false;
  uint16_t *p = buf;
  for (unsigned i = 0; i < 256; i++)
    p[i] = io_in16(d->port);
  ata_delay(d);
  return ata_wait(d, false);
}
bool disk_raw_write(unsigned n, uint32_t lba, const void *buf) {
  if (n < ndisks && disks[n].backend == 3) return !disks[n].read_only && lba < disks[n].sectors && nvme_transfer(disks[n].unit,lba,(void *)buf,true);
  if (n < ndisks && disks[n].read_only)
    return false;
  if (n < ndisks && disks[n].backend == 2)
    return lba < disks[n].sectors &&
           virtio_block_transfer(lba, (void *)(uintptr_t)buf, true);
  if (n < ndisks && disks[n].backend == 1)
    return lba < disks[n].sectors &&
           ahci_transfer(disks[n].unit, lba, (void *)(uintptr_t)buf, true);
  if (!ata_select(n, lba))
    return false;
  struct block_device *d = &disks[n];
  io_out8(d->port + 7, 0x30);
  ata_delay(d);
  if (!ata_wait(d, true))
    return false;
  const uint16_t *p = buf;
  for (unsigned i = 0; i < 256; i++)
    io_out16(d->port, p[i]);
  ata_delay(d);
  return ata_wait(d, false);
}
bool disk_raw_flush(unsigned n) {
  if (n < ndisks && disks[n].backend == 3) return nvme_flush(disks[n].unit);
  if (n < ndisks && disks[n].backend == 2)
    return virtio_block_flush();
  if (n < ndisks && disks[n].backend == 1)
    return ahci_flush(disks[n].unit);
  if (n >= ndisks)
    return false;
  struct block_device *d = &disks[n];
  io_out8(d->port + 6, 0xe0 | (d->slave << 4));
  ata_delay(d);
  io_out8(d->port + 7, 0xe7);
  ata_delay(d);
  return ata_wait(d, false);
}
static bool mouse_ready;
static uint8_t packet[3], packet_n;
static int mouse_x, mouse_y;
static volatile bool mouse_irq_poll;
static struct mouse_event {
  int x, y;
  uint8_t buttons;
} mouse_events[32];
static unsigned mouse_event_head, mouse_event_count;
static uint8_t mouse_gui_buttons;
static bool mouse_release_stage;
void mouse_interrupt_poll(bool enabled) {
  mouse_irq_poll = false;
  mouse_event_head = mouse_event_count = 0;
  mouse_gui_buttons = 0;
  mouse_release_stage = false;
  mouse_irq_poll = enabled;
}
static unsigned mouse_width = 4096, mouse_height = 4096, mouse_speed = 100;
static int mouse_x_fraction, mouse_y_fraction;
void mouse_configure(unsigned width, unsigned height, unsigned speed) {
  if (width)
    mouse_width = width;
  if (height)
    mouse_height = height;
  if (speed >= 25 && speed <= 250)
    mouse_speed = speed;
}
static uint8_t mouse_buttons;
static bool absolute_mouse;
struct pointer_reply {
  uint32_t value, x, y, z;
};
static struct pointer_reply pointer_command(uint32_t command,
                                            uint32_t argument) {
  uint32_t a = 0x564d5868, b = argument, c = command, d = 0x5658;
  __asm__ volatile("inl %%dx, %%eax"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d)::"memory");
  return (struct pointer_reply){a, b, c, d};
}
bool mouse_absolute(void) { return absolute_mouse; }
static void pointer_absolute_init(void) {
  struct pointer_reply reply = pointer_command(10, ~0x564d5868u);
  if (reply.x != 0x564d5868 || reply.value == 0xffffffffu)
    return;
  pointer_command(41, 0x45414552);
  reply = pointer_command(40, 0);
  if ((reply.value >> 16) || !(reply.value & 0xffff))
    return;
  reply = pointer_command(39, 1);
  if (reply.value != 0x3442554a) {
    pointer_command(41, 0xf5);
    return;
  }
  pointer_command(41, 0x53424152);
  absolute_mouse = true;
  console_write("Absolute virtual pointer ready\n");
}
static void pointer_buttons(uint8_t buttons) {
  if (mouse_irq_poll && buttons != mouse_buttons) {
    if (mouse_event_count == 32) {
      mouse_event_head = (mouse_event_head + 1) % 32;
      mouse_event_count--;
    }
    mouse_events[(mouse_event_head + mouse_event_count) % 32] =
        (struct mouse_event){mouse_x, mouse_y, buttons};
    mouse_event_count++;
  }
  mouse_buttons = buttons;
}
void mouse_feed_relative(int dx, int dy, uint8_t buttons) {
  mouse_x_fraction += dx * (int)mouse_speed;
  mouse_y_fraction += dy * (int)mouse_speed;
  mouse_x += mouse_x_fraction / 100;
  mouse_y -= mouse_y_fraction / 100;
  mouse_x_fraction %= 100;
  mouse_y_fraction %= 100;
  if (mouse_x < 0) mouse_x = 0;
  if (mouse_y < 0) mouse_y = 0;
  if (mouse_x >= (int)mouse_width) mouse_x = mouse_width - 1;
  if (mouse_y >= (int)mouse_height) mouse_y = mouse_height - 1;
  pointer_buttons(buttons & 7);
}
static void pointer_absolute_poll(void) {
  for (unsigned i = 0; i < 64; i++) {
    struct pointer_reply reply = pointer_command(40, 0);
    if (reply.value >> 16 || (reply.value & 0xffff) < 4)
      return;
    reply = pointer_command(39, 4);
    if (reply.value & 0x10000)
      continue;
    uint32_t x = reply.x > 65535 ? 65535 : reply.x;
    uint32_t y = reply.y > 65535 ? 65535 : reply.y;
    mouse_x = ((uint64_t)x * (mouse_width - 1) + 32767) / 65535;
    mouse_y = ((uint64_t)y * (mouse_height - 1) + 32767) / 65535;
    pointer_buttons(((reply.value & 0x20) ? 1 : 0) |
                    ((reply.value & 0x10) ? 2 : 0) |
                    ((reply.value & 0x08) ? 4 : 0));
  }
}
static bool ps2_wait_write(void) {
  for (unsigned i = 0; i < 100000; i++)
    if (!(io_in8(0x64) & 2))
      return true;
  return false;
}
static bool ps2_read(uint8_t *v) {
  for (unsigned i = 0; i < 100000; i++)
    if (io_in8(0x64) & 1) {
      *v = io_in8(0x60);
      return true;
    }
  return false;
}
static bool mouse_command(uint8_t c) {
  if (!ps2_wait_write())
    return false;
  io_out8(0x64, 0xd4);
  if (!ps2_wait_write())
    return false;
  io_out8(0x60, c);
  uint8_t ack;
  return ps2_read(&ack) && ack == 0xfa;
}
static void mouse_init(void) {
  if (!ps2_wait_write())
    return;
  io_out8(0x64, 0xa8);
  if (!ps2_wait_write())
    return;
  io_out8(0x64, 0x20);
  uint8_t cfg;
  if (!ps2_read(&cfg))
    return;
  if (!ps2_wait_write())
    return;
  io_out8(0x64, 0x60);
  if (!ps2_wait_write())
    return;
  io_out8(0x60, cfg & ~0x22);
  mouse_ready = mouse_command(0xf6) && mouse_command(0xf4);
  console_write(mouse_ready ? "PS/2 mouse ready\n"
                            : "PS/2 mouse initialization failed\n");
  if (mouse_ready)
    pointer_absolute_init();
}
static void mouse_poll(void) {
  uint8_t status = io_in8(0x64);
  if ((status & 0x21) != 0x21)
    return;
  uint8_t b = io_in8(0x60);
  if (!mouse_ready)
    return;
  if (!packet_n && !(b & 8))
    return;
  packet[packet_n++] = b;
  if (packet_n == 3) {
    packet_n = 0;
    if (absolute_mouse) {
      pointer_absolute_poll();
      return;
    }
    if (packet[0] & 0xc0)
      return;
    int dx = (int)packet[1] - ((packet[0] & 0x10) ? 256 : 0),
        dy = (int)packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    mouse_x_fraction += dx * (int)mouse_speed;
    mouse_y_fraction += dy * (int)mouse_speed;
    mouse_x += mouse_x_fraction / 100;
    mouse_y -= mouse_y_fraction / 100;
    mouse_x_fraction %= 100;
    mouse_y_fraction %= 100;
    if (mouse_x < 0)
      mouse_x = 0;
    if (mouse_y < 0)
      mouse_y = 0;
    if (mouse_x >= (int)mouse_width)
      mouse_x = mouse_width - 1;
    if (mouse_y >= (int)mouse_height)
      mouse_y = mouse_height - 1;
    pointer_buttons(packet[0] & 7);
  }
}
void devices_poll(void) {
  if (!mouse_irq_poll) mouse_poll();
  usb_poll();
}
void devices_tick(void) {
  if (mouse_irq_poll)
    for (unsigned i = 0; i < 32; i++) {
      if (io_in8(0x64) & 0x20)
        mouse_poll();
      else
        console_keyboard_poll();
    }
}
void mouse_print(void) {
  if (!mouse_ready) {
    console_write("No PS/2 mouse acknowledged initialization.\n");
    return;
  }
  console_write(absolute_mouse ? "Absolute pointer x=" : "PS/2 mouse x=");
  print_uint(mouse_x);
  console_write(" y=");
  print_uint(mouse_y);
  console_write(" buttons=");
  print_uint(mouse_buttons);
  console_putc('\n');
}
void mouse_state(int *x, int *y, uint8_t *buttons) {
  *x = mouse_x;
  *y = mouse_y;
  *buttons = mouse_buttons;
}
void mouse_input_state(int *x, int *y, uint8_t *buttons) {
  uintptr_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
  if (mouse_event_count) {
    struct mouse_event e = mouse_events[mouse_event_head];
    *x = e.x;
    *y = e.y;
    if ((mouse_gui_buttons & ~e.buttons) && !mouse_release_stage) {
      *buttons = mouse_gui_buttons;
      mouse_release_stage = true;
    } else {
      *buttons = e.buttons;
      mouse_gui_buttons = e.buttons;
      mouse_event_head = (mouse_event_head + 1) % 32;
      mouse_event_count--;
      mouse_release_stage = false;
    }
  } else {
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
    mouse_gui_buttons = mouse_buttons;
  }
  if (flags & (1u << 9))
    __asm__ volatile("sti" ::: "memory");
}
static uint8_t cmos(uint8_t r) {
  io_out8(0x70, r);
  return io_in8(0x71);
}
static uint8_t decimal(uint8_t n) { return (n >> 4) * 10 + (n & 15); }
bool rtc_time(uint32_t *days, uint32_t *seconds) {
  uint8_t a[7], b[7];
  const uint8_t regs[] = {0, 2, 4, 7, 8, 9, 11};
  bool stable = false;
  for (unsigned tries = 0; tries < 100; tries++) {
    if (cmos(10) & 0x80)
      continue;
    for (unsigned i = 0; i < 7; i++)
      a[i] = cmos(regs[i]);
    if (cmos(10) & 0x80)
      continue;
    for (unsigned i = 0; i < 7; i++)
      b[i] = cmos(regs[i]);
    stable = true;
    for (unsigned i = 0; i < 7; i++)
      if (a[i] != b[i])
        stable = false;
    if (stable)
      break;
  }
  if (!stable) {
    return false;
  }
  bool pm = a[2] & 0x80;
  a[2] &= 0x7f;
  if (!(a[6] & 4))
    for (unsigned i = 0; i < 6; i++)
      a[i] = decimal(a[i]);
  if (!(a[6] & 2))
    a[2] = (a[2] % 12) + (pm ? 12 : 0);
  unsigned year = 2000 + a[5], month = a[4], day = a[3];
  if (month < 1 || month > 12 || day < 1 || day > 31 || a[2] > 23 || a[1] > 59 || a[0] > 59) return false;
  uint32_t total = 719528;
  for (unsigned y = 1970; y < year; y++) total += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  static const unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  for (unsigned m = 1; m < month; m++) total += lengths[m-1] + (m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  *days = total + day - 1;
  *seconds = a[2] * 3600u + a[1] * 60u + a[0];
  return true;
}
void rtc_print(void) {
  uint8_t a[7], b[7];
  const uint8_t regs[] = {0, 2, 4, 7, 8, 9, 11};
  bool stable = false;
  for (unsigned tries = 0; tries < 100; tries++) {
    if (cmos(10) & 0x80)
      continue;
    for (unsigned i = 0; i < 7; i++)
      a[i] = cmos(regs[i]);
    if (cmos(10) & 0x80)
      continue;
    for (unsigned i = 0; i < 7; i++)
      b[i] = cmos(regs[i]);
    stable = true;
    for (unsigned i = 0; i < 7; i++)
      if (a[i] != b[i])
        stable = false;
    if (stable)
      break;
  }
  if (!stable) {
    console_write("RTC unavailable.\n");
    return;
  }
  bool pm = a[2] & 0x80;
  a[2] &= 0x7f;
  if (!(a[6] & 4))
    for (unsigned i = 0; i < 6; i++)
      a[i] = decimal(a[i]);
  if (!(a[6] & 2))
    a[2] = (a[2] % 12) + (pm ? 12 : 0);
  print_uint(2000 + a[5]);
  console_putc('-');
  print_uint(a[4]);
  console_putc('-');
  print_uint(a[3]);
  console_putc(' ');
  print_uint(a[2]);
  console_putc(':');
  print_uint(a[1]);
  console_putc(':');
  print_uint(a[0]);
  console_write(" (firmware RTC)\n");
}
void speaker_beep(uint32_t hz, uint32_t duration) {
  if (hz < 20 || hz > 20000 || duration > 500)
    return;
  uint32_t divisor = 1193182 / hz;
  io_out8(0x43, 0xb6);
  io_out8(0x42, divisor);
  io_out8(0x42, divisor >> 8);
  uint8_t old = io_in8(0x61);
  io_out8(0x61, old | 3);
  sleep_ticks(duration);
  io_out8(0x61, old & ~3);
}
void devices_init(void) {
  pci_init();
  ata_init();
  ahci_init();
  nvme_init();
  virtio_block_init();
  mouse_init();
  usb_init();
}
void devices_list(void) {
  console_write(pci_ecam_bus(0) ? "PCI Express ECAM: " : "PCI configuration ports: ");
  print_uint(pci_count);
  console_write(" devices\n");
  for (unsigned i = 0; i < pci_count; i++) {
    const struct pci_device *d = &pci[i];
    print_uint(d->bus);
    console_putc(':');
    print_uint(d->slot);
    console_putc('.');
    print_uint(d->function);
    console_write(" id=");
    print_hex(((uintptr_t)d->vendor << 16) | d->device);
    console_write(" class=");
    print_hex(((uintptr_t)d->class_id << 8) | d->subclass);
    console_putc('\n');
  }
  for (unsigned i = 0; i < ndisks; i++) {
    console_write(disks[i].backend == 3 ? "NVMe disk " : disks[i].backend == 2 ? "VirtIO disk "
                  : disks[i].backend    ? "AHCI disk "
                                        : "ATA disk ");
    print_uint(i);
    console_write(" ");
    console_write(disks[i].model);
    console_write(" sectors=");
    print_uint(disks[i].sectors);
    console_putc('\n');
  }
  console_write(
      "Drivers: PCI, ATA PIO/LBA28, AHCI SATA/DMA48, VirtIO block/network, "
      "PS/2 keyboard/mouse, USB 1.1 UHCI HID keyboard/mouse, "
      "COM1,\nCMOS RTC, PIT timer/speaker, framebuffer, RTL8139, E1000, AMD PCnet.\n");
}
