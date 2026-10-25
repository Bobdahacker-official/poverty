#include "system.h"
static volatile uint32_t ticks;
static bool have_fpu, have_fxsr;
static uint8_t initial_fpu[512] __attribute__((aligned(16)));
static void fpu_init(void) {
  uint32_t a = 1, b, c, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
  have_fpu = (d & 1) != 0;
  have_fxsr = (d & (1u << 24)) != 0;
  if (!have_fpu)
    return;
  uintptr_t cr0;
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 = (cr0 & ~12u) | 0x22;
  __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
  if (have_fxsr) {
    uintptr_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= 1u << 9;
    if (d & (1u << 25))
      cr4 |= 1u << 10;
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));
  }
  __asm__ volatile("fninit");
  if (have_fxsr && (d & (1u << 25))) {
    uint32_t mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
  }
  platform_fpu_save(initial_fpu);
  platform_fpu_restore(initial_fpu);
}
void platform_fpu_save(void *p) {
  if (!have_fpu)
    return;
  if (have_fxsr)
    __asm__ volatile("fxsave (%0)" ::"r"(p) : "memory");
  else
    __asm__ volatile("fnsave (%0)" ::"r"(p) : "memory");
}
void platform_fpu_restore(const void *p) {
  if (!have_fpu)
    return;
  if (have_fxsr)
    __asm__ volatile("fxrstor (%0)" ::"r"(p) : "memory");
  else
    __asm__ volatile("frstor (%0)" ::"r"(p) : "memory");
}
void platform_fpu_initial(void *p) { memcpy(p, initial_fpu, 512); }

static uint64_t gdt[11] __attribute__((aligned(16)));
static uint8_t interrupt_stack[32768] __attribute__((aligned(16)));
struct tss {
  uint32_t reserved;
  uint64_t esp0, rsp1, rsp2, reserved2, ist[7], reserved3;
  uint16_t reserved4, iomap;
} __attribute__((packed));
static struct tss tss;
struct gate {
  uint16_t low, selector;
  uint8_t zero, flags;
  uint16_t high;
  uint32_t upper, reserved;
} __attribute__((packed));
static struct gate idt[256];
extern void *exception_table[32];
extern void timer_entry(void), syscall_entry(void), irq7_entry(void),
    irq15_entry(void), schedule_entry(void), arch_load_gdt(void *);
static void gate(unsigned n, void *entry, uint8_t flags) {
  uint32_t a = (uintptr_t)entry;
  idt[n] = (struct gate){a & 65535, 8, 0, flags, a >> 16, 0, 0};
}
void platform_init(void) {
  fpu_init();
  gdt[0] = 0;
  gdt[1] = 0x00af9a000000ffffULL;
  gdt[2] = 0x00cf92000000ffffULL;
  gdt[3] = 0x00affa000000ffffULL;
  gdt[4] = 0x00cff2000000ffffULL;
  memset(&tss, 0, sizeof(tss));
  gdt[7] = 0x00cffa000000ffffULL;
  tss.esp0 = (uintptr_t)(interrupt_stack + sizeof(interrupt_stack));
  tss.iomap = sizeof(tss);
  uint32_t b = (uintptr_t)&tss, l = sizeof(tss) - 1;
  gdt[5] = (l & 65535ULL) | ((uint64_t)(b & 0xffffff) << 16) | (0x89ULL << 40) |
           ((uint64_t)(l & 0xf0000) << 32) | ((uint64_t)(b >> 24) << 56);
  struct {
    uint16_t limit;
    uint64_t base;
  } __attribute__((packed)) gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt},
                            idtr = {sizeof(idt) - 1, (uintptr_t)idt};
  arch_load_gdt(&gdtr);
  for (unsigned i = 0; i < 32; i++)
    gate(i, exception_table[i], 0x8e);
  gate(32, timer_entry, 0x8e);
  gate(39, irq7_entry, 0x8e);
  gate(47, irq15_entry, 0x8e);
  gate(128, syscall_entry, 0xee);
  gate(129, schedule_entry, 0x8e);
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
  while ((uintptr_t)(ticks - start) < n) {
    net_poll();
    __asm__ volatile("hlt");
  }
}
void platform_kernel_stack(uint32_t stack) { tss.esp0 = stack; }
void platform_tls(const uint64_t *tls) {
  for (unsigned i = 0; i < 3; i++)
    gdt[8 + i] = tls ? tls[i] : 0;
}
struct trap_frame *trap_dispatch(struct trap_frame *f) {
  if (f->vector == 39) {
    io_out8(0x20, 0x0b);
    if (io_in8(0x20) & 0x80)
      io_out8(0x20, 0x20);
    return f;
  }
  if (f->vector == 47) {
    io_out8(0xa0, 0x0b);
    if (io_in8(0xa0) & 0x80)
      io_out8(0xa0, 0x20);
    io_out8(0x20, 0x20);
    return f;
  }
  if (f->vector == 32) {
    ++ticks;
    io_out8(0x20, 0x20);
    return user_timer(f);
  }
  if (f->vector == 129)
    return user_reschedule(f);
  if (f->vector == 128) {
    __asm__ volatile("sti");
    user_syscall(f);
    __asm__ volatile("cli");
    return user_reschedule(f);
  }
  if ((f->cs & 3) == 3 && user_active()) {
    user_fault(f->vector, f->eip);
    return user_reschedule(f);
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
    if (*s < '0' || *s > '9' || n > (0xffffffffu - (uintptr_t)(*s - '0')) / 10)
      return 0;
    n = n * 10 + (uintptr_t)(*s++ - '0');
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
