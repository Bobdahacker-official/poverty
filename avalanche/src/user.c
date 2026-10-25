#include "abi.h"
#include "system.h"
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t user_table[1024] __attribute__((aligned(4096)));
static bool running;
static uint32_t cpu_ticks;
static char arguments[256];
extern int arch_user_enter(uint32_t, uint32_t);
extern void arch_user_leave(int) __attribute__((noreturn));
#define APP(name) extern const uint8_t app_##name##_start[], app_##name##_end[]
APP(hello);
APP(cat);
APP(udp);
APP(ush);
APP(http);
struct builtin {
  const char *name;
  const uint8_t *start, *end;
};
static const struct builtin builtins[] = {
    {"hello", app_hello_start, app_hello_end},
    {"cat", app_cat_start, app_cat_end},
    {"udp", app_udp_start, app_udp_end},
    {"ush", app_ush_start, app_ush_end},
    {"http", app_http_start, app_http_end}};
struct elf_header {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version, entry, phoff, shoff, flags;
  uint16_t ehsize, phsize, phnum, shsize, shnum, shstr;
} __attribute__((packed));
struct program_header {
  uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));
static void tlb(void) {
  __asm__ volatile("mov %0,%%cr3" ::"r"((uint32_t)page_directory) : "memory");
}
void user_init(void) {
  for (unsigned i = 0; i < 1024; i++)
    page_directory[i] = (i << 22) | 0x83;
  page_directory[AV_USER_BASE >> 22] = (uint32_t)user_table | 7;
  uint32_t cr4, cr0;
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  cr4 = (cr4 & ~(0x20u | 0x80u)) | 0x10;
  __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));
  tlb();
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 |= 0x80010000u;
  __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
}
bool platform_mmio(uint32_t address, uint32_t size) {
  if (!size || size - 1 > 0xffffffffu - address)
    return false;
  unsigned first = address >> 22, last = (address + size - 1) >> 22;
  if (first <= (AV_USER_BASE >> 22) && last >= (AV_USER_BASE >> 22))
    return false;
  for (unsigned i = first; i <= last; i++)
    page_directory[i] |= 0x18;
  tlb();
  return true;
}
static void release_pages(void) {
  for (unsigned i = 0; i < 1024; i++)
    if (user_table[i] & 1) {
      page_free((void *)(user_table[i] & ~4095u));
      user_table[i] = 0;
    }
  tlb();
}
static bool range(uint32_t p, uint32_t n, bool write) {
  if (!n)
    return true;
  if (p < AV_USER_BASE || p >= AV_USER_END || n > AV_USER_END - p)
    return false;
  uint32_t first = (p - AV_USER_BASE) / 4096,
           last = (p + n - 1 - AV_USER_BASE) / 4096;
  for (uint32_t i = first; i <= last; i++)
    if ((user_table[i] & 5) != 5 || (write && !(user_table[i] & 2)))
      return false;
  return true;
}
static bool user_string(uint32_t p, char *out, unsigned cap) {
  for (unsigned i = 0; i < cap; i++) {
    if (!range(p + i, 1, false))
      return false;
    out[i] = *(const char *)(p + i);
    if (!out[i])
      return true;
  }
  return false;
}
static bool map_page(uint32_t v, bool write) {
  unsigned i = (v - AV_USER_BASE) / 4096;
  if (user_table[i] & 1) {
    if (write)
      user_table[i] |= 2;
    return true;
  }
  void *p = page_alloc();
  if (!p)
    return false;
  user_table[i] = (uint32_t)p | 5 | (write ? 2 : 0);
  return true;
}
bool user_active(void) { return running; }
void user_stop(int status) {
  running = false;
  arch_user_leave(status);
}
void user_timer(struct trap_frame *f) {
  if (running && (f->cs & 3) == 3 && ++cpu_ticks > 12000) {
    console_write("\nProcess exceeded its two-minute userspace CPU budget.\n");
    user_stop(124);
  }
}
int user_run(const void *image, uint32_t size, const char *args) {
  if (running || size < sizeof(struct elf_header))
    return -1;
  const struct elf_header *h = image;
  if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' ||
      h->ident[3] != 'F' || h->ident[4] != 1 || h->ident[5] != 1 ||
      h->ident[6] != 1 || h->type != 2 || h->machine != 3 || h->version != 1 ||
      h->ehsize != sizeof(*h) || h->phsize != sizeof(struct program_header) ||
      !h->phnum || h->phnum > 32 || h->phoff > size ||
      h->phnum > (size - h->phoff) / sizeof(struct program_header))
    return -2;
  const struct program_header *ph =
      (const void *)((const uint8_t *)image + h->phoff);
  bool executable = false;
  uint32_t total = 0;
  for (unsigned i = 0; i < h->phnum; i++) {
    const struct program_header *p = &ph[i];
    if (p->type == 3 || p->type == 2)
      return -3;
    if (p->type != 1)
      continue;
    if (p->memsz < p->filesz || p->offset > size ||
        p->filesz > size - p->offset)
      return -3;
    if (!p->memsz)
      continue;
    if (p->vaddr < AV_USER_BASE || p->vaddr > 0x403f0000u ||
        p->memsz > 0x403f0000u - p->vaddr || p->memsz > 1024 * 1024 ||
        total > 1024 * 1024 - p->memsz)
      return -3;
    total += p->memsz;
    if ((p->flags & 1) && h->entry >= p->vaddr &&
        h->entry - p->vaddr < p->memsz)
      executable = true;
    for (unsigned j = 0; j < i; j++)
      if (ph[j].type == 1 && ph[j].memsz &&
          p->vaddr < ph[j].vaddr + ph[j].memsz &&
          ph[j].vaddr < p->vaddr + p->memsz)
        return -3;
  }
  if (!executable)
    return -3;
  release_pages();
  for (unsigned i = 0; i < h->phnum; i++) {
    const struct program_header *p = &ph[i];
    if (p->type != 1 || !p->memsz)
      continue;
    uint32_t start = p->vaddr & ~4095u,
             last = (p->vaddr + p->memsz - 1) & ~4095u;
    for (uint32_t a = start;; a += 4096) {
      if (!map_page(a, (p->flags & 2) != 0))
        goto no_memory;
      if (a == last)
        break;
    }
    for (uint32_t n = 0; n < p->filesz;) {
      uint32_t v = p->vaddr + n, index = (v - AV_USER_BASE) / 4096,
               offset = v & 4095, count = p->filesz - n;
      if (count > 4096 - offset)
        count = 4096 - offset;
      memcpy((void *)((user_table[index] & ~4095u) + offset),
             (const uint8_t *)image + p->offset + n, count);
      n += count;
    }
  }
  for (uint32_t p = 0x403fc000; p < AV_USER_END; p += 4096)
    if (!map_page(p, true))
      goto no_memory;
  tlb();
  memset(arguments, 0, sizeof(arguments));
  if (args) {
    size_t n = strlen(args);
    if (n >= sizeof(arguments))
      n = sizeof(arguments) - 1;
    memcpy(arguments, args, n);
  }
  running = true;
  cpu_ticks = 0;
  int status = arch_user_enter(h->entry, AV_USER_END - 16);
  running = false;
  net_tcp_close();
  release_pages();
  return status;
no_memory:
  release_pages();
  return -4;
}
static uint8_t executable_image[FS_MAX_FILE];
int user_exec(const char *name, const char *args) {
  int size = fs_size(name);
  if (size >= 0) {
    int n = fs_read(name, executable_image, sizeof(executable_image));
    return n < 0 ? n : user_run(executable_image, (uint32_t)n, args);
  }
  for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
    if (str_eq(name, builtins[i].name))
      return user_run(builtins[i].start,
                      (uint32_t)(builtins[i].end - builtins[i].start), args);
  return -5;
}
int user_install(void) {
  for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    int r = fs_write(builtins[i].name, builtins[i].start,
                     (uint32_t)(builtins[i].end - builtins[i].start));
    if (r < 0)
      return r;
  }
  return 0;
}
void user_syscall(struct trap_frame *f) {
  if (!running || (f->cs & 3) != 3) {
    f->eax = (uint32_t)-1;
    return;
  }
  uint32_t n = f->ecx;
  char name[32];
  int result = -1;
  switch (f->eax) {
  case AV_EXIT:
    user_stop((int)f->ebx);
  case AV_WRITE:
    if (n <= 4096 && range(f->ebx, n, false)) {
      const char *s = (void *)f->ebx;
      for (uint32_t i = 0; i < n; i++)
        console_putc(s[i]);
      result = (int)n;
    }
    break;
  case AV_READLINE:
    if (n >= 1 && n <= 4096 && range(f->ebx, n, true)) {
      char *out = (void *)f->ebx;
      uint32_t i = 0;
      for (;;) {
        char c = console_input();
        if (c == '\n') {
          console_putc('\n');
          break;
        }
        if (c == '\b') {
          if (i) {
            i--;
            console_write("\b \b");
          }
        } else if (c >= 32 && c <= 126 && i < n - 1) {
          out[i++] = c;
          console_putc(c);
        }
      }
      out[i] = 0;
      result = (int)i;
    }
    break;
  case AV_TICKS:
    result = (int)clock_ticks();
    break;
  case AV_FILE_READ:
    if (f->edx <= FS_MAX_FILE && user_string(f->ebx, name, sizeof(name)) &&
        range(f->ecx, f->edx, true))
      result = fs_read(name, (void *)f->ecx, f->edx);
    break;
  case AV_FILE_WRITE:
    if (f->edx <= FS_MAX_FILE && user_string(f->ebx, name, sizeof(name)) &&
        range(f->ecx, f->edx, false))
      result = fs_write(name, (void *)f->ecx, f->edx);
    break;
  case AV_FILE_REMOVE:
    if (user_string(f->ebx, name, sizeof(name)))
      result = fs_remove(name);
    break;
  case AV_FILE_LIST:
    fs_list();
    result = 0;
    break;
  case AV_SLEEP:
    if (f->ebx <= 1000) {
      sleep_ticks(f->ebx);
      result = 0;
    }
    break;
  case AV_UDP_SEND:
    if (f->esi <= 1472 && range(f->edx, f->esi, false))
      result = net_udp_send(f->ebx, (uint16_t)(f->ecx >> 16), (uint16_t)f->ecx,
                            (void *)f->edx, f->esi);
    break;
  case AV_UDP_RECV:
    if (f->edx <= 1472 && range(f->ecx, f->edx, true)) {
      uint32_t source = 0;
      uint16_t port = 0;
      result = net_udp_recv((uint16_t)f->ebx, (void *)f->ecx, f->edx, &source,
                            &port);
      f->edx = source;
      f->ecx = port;
    }
    break;
  case AV_ARGS:
    if (n && n <= 256 && range(f->ebx, n, true)) {
      uint32_t len = strlen(arguments);
      if (len >= n)
        len = n - 1;
      memcpy((void *)f->ebx, arguments, len);
      ((char *)f->ebx)[len] = 0;
      result = (int)len;
    }
    break;
  case AV_FILE_SIZE:
    if (user_string(f->ebx, name, sizeof(name)))
      result = fs_size(name);
    break;
  case AV_YIELD:
    net_poll();
    result = 0;
    break;
  case AV_TCP_CONNECT:
    if (f->ecx && f->ecx <= 65535)
      result = net_tcp_connect(f->ebx, (uint16_t)f->ecx);
    break;
  case AV_TCP_SEND:
    if (n <= 4096 && range(f->ebx, n, false))
      result = net_tcp_send((void *)f->ebx, n);
    break;
  case AV_TCP_RECV:
    if (n <= 4096 && range(f->ebx, n, true) && f->edx <= 1000)
      result = net_tcp_recv((void *)f->ebx, n, f->edx);
    break;
  case AV_TCP_CLOSE:
    result = net_tcp_close();
    break;
  case AV_DNS: {
    char hostname[256];
    uint32_t answer;
    if (user_string(f->ebx, hostname, sizeof(hostname)) &&
        range(f->ecx, 4, true)) {
      result = net_dns(hostname, &answer);
      if (!result)
        memcpy((void *)f->ecx, &answer, 4);
    }
    break;
  }
  default:
    break;
  }
  f->eax = (uint32_t)result;
}
