#include "system.h"
static volatile uint32_t ticks;
static uint64_t gdt[6] __attribute__((aligned(16)));
static uint8_t interrupt_stack[32768] __attribute__((aligned(16)));
struct tss {
  uint32_t prev, esp0, ss0, unused[22];
  uint16_t trap, iomap;
} __attribute__((packed));
static struct tss tss;
struct gate {
  uint16_t low, selector;
  uint8_t zero, flags;
  uint16_t high;
} __attribute__((packed));
static struct gate idt[256];
extern void *exception_table[32];
extern void timer_entry(void), syscall_entry(void), irq7_entry(void),
    irq15_entry(void), arch_load_gdt(void *);
static void gate(unsigned n, void *entry, uint8_t flags) {
  uint32_t a = (uint32_t)entry;
  idt[n] = (struct gate){a & 65535, 8, 0, flags, a >> 16};
}
void platform_init(void) {
  gdt[0] = 0;
  gdt[1] = 0x00cf9a000000ffffULL;
  gdt[2] = 0x00cf92000000ffffULL;
  gdt[3] = 0x00cffa000000ffffULL;
  gdt[4] = 0x00cff2000000ffffULL;
  memset(&tss, 0, sizeof(tss));
  tss.ss0 = 16;
  tss.esp0 = (uint32_t)(interrupt_stack + sizeof(interrupt_stack));
  tss.iomap = sizeof(tss);
  uint32_t b = (uint32_t)&tss, l = sizeof(tss) - 1;
  gdt[5] = (l & 65535ULL) | ((uint64_t)(b & 0xffffff) << 16) | (0x89ULL << 40) |
           ((uint64_t)(l & 0xf0000) << 32) | ((uint64_t)(b >> 24) << 56);
  struct {
    uint16_t limit;
    uint32_t base;
  } __attribute__((packed)) gdtr = {sizeof(gdt) - 1, (uint32_t)gdt},
                            idtr = {sizeof(idt) - 1, (uint32_t)idt};
  arch_load_gdt(&gdtr);
  for (unsigned i = 0; i < 32; i++)
    gate(i, exception_table[i], 0x8e);
  gate(32, timer_entry, 0x8e);
  gate(39, irq7_entry, 0x8e);
  gate(47, irq15_entry, 0x8e);
  gate(128, syscall_entry, 0xee);
  __asm__ volatile("lidt %0" ::"m"(idtr));
  io_out8(0x20, 0x11);
  io_out8(0xa0, 0x11);
  io_out8(0x21, 0x20);
  io_out8(0xa1, 0x28);
  io_out8(0x21, 4);
  io_out8(0xa1, 2);
  io_out8(0x21, 1);
  io_out8(0xa1, 1);
  io_out8(0x21, 0xfe);
  io_out8(0xa1, 0xff);
  io_out8(0x43, 0x36);
  io_out8(0x40, 11932 & 255);
  io_out8(0x40, 11932 >> 8);
}
void platform_enable(void) { __asm__ volatile("sti"); }
uint32_t clock_ticks(void) { return ticks; }
void sleep_ticks(uint32_t n) {
  uint32_t start = ticks;
  while ((uint32_t)(ticks - start) < n) {
    net_poll();
    __asm__ volatile("hlt");
  }
}
void trap_dispatch(struct trap_frame *f) {
  if (f->vector == 39) {
    io_out8(0x20, 0x0b);
    if (io_in8(0x20) & 0x80)
      io_out8(0x20, 0x20);
    return;
  }
  if (f->vector == 47) {
    io_out8(0xa0, 0x0b);
    if (io_in8(0xa0) & 0x80)
      io_out8(0xa0, 0x20);
    io_out8(0x20, 0x20);
    return;
  }
  if (f->vector == 32) {
    ++ticks;
    io_out8(0x20, 0x20);
    user_timer(f);
    return;
  }
  if (f->vector == 128) {
    __asm__ volatile("sti");
    user_syscall(f);
    __asm__ volatile("cli");
    return;
  }
  if ((f->cs & 3) == 3 && user_active()) {
    console_write("\nUser process fault ");
    print_uint(f->vector);
    console_write(" at ");
    print_hex(f->eip);
    console_putc('\n');
    user_stop(128 + (int)f->vector);
  }
  console_write("\nKERNEL EXCEPTION ");
  print_uint(f->vector);
  console_write(" error=");
  print_hex(f->error);
  console_write(" eip=");
  print_hex(f->eip);
  console_write("\nCPU halted.\n");
  for (;;)
    __asm__ volatile("cli; hlt");
}
uint32_t parse_uint(const char *s, bool *ok) {
  uint32_t n = 0;
  *ok = false;
  if (!*s)
    return 0;
  while (*s) {
    if (*s < '0' || *s > '9' || n > (0xffffffffu - (uint32_t)(*s - '0')) / 10)
      return 0;
    n = n * 10 + (uint32_t)(*s++ - '0');
  }
  *ok = true;
  return n;
}
uint32_t crc32(const void *p, size_t n) {
  const uint8_t *b = p;
  uint32_t c = ~0u;
  while (n--) {
    c ^= *b++;
    for (unsigned j = 0; j < 8; j++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}
