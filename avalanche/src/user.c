#include "abi.h"
#include "process.h"
static uint32_t kernel_directory[1024] __attribute__((aligned(4096)));
static struct process tasks[PROCESS_MAX];
static int current = -1, next_pid = 1;
static int tcp_owner;
static bool tcp_cleanup;
static struct trap_frame *kernel_frame;
static uint8_t executable_image[FS_MAX_FILE];
#define APP(n) extern const uint8_t app_##n##_start[], app_##n##_end[]
APP(hello);
APP(cat);
APP(udp);
APP(ush);
APP(http);
APP(worker);
APP(linuxhello);
APP(linuxfork);
struct builtin {
  const char *name;
  const uint8_t *start, *end;
};
static const struct builtin builtins[] = {
    {"hello", app_hello_start, app_hello_end},
    {"cat", app_cat_start, app_cat_end},
    {"udp", app_udp_start, app_udp_end},
    {"ush", app_ush_start, app_ush_end},
    {"http", app_http_start, app_http_end},
    {"worker", app_worker_start, app_worker_end},
    {"linuxhello", app_linuxhello_start, app_linuxhello_end},
    {"linuxfork", app_linuxfork_start, app_linuxfork_end}};
struct elf_header {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version, entry, phoff, shoff, flags;
  uint16_t ehsize, phsize, phnum, shsize, shnum, shstr;
} __attribute__((packed));
struct program_header {
  uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));
static void activate(uint32_t *directory) {
  __asm__ volatile("mov %0,%%cr3" ::"r"((uint32_t)directory) : "memory");
}
struct process *process_current(void) {
  return current < 0 ? NULL : &tasks[current];
}
bool user_active(void) {
  return current >= 0 && tasks[current].state != TASK_EMPTY;
}
bool process_address(uint32_t p, uint32_t n) {
  if (!n)
    return true;
  if (n > 0xffffffffu - p)
    return false;
  uint32_t end = p + n;
  return (p >= 0x08000000u && end <= 0x20000000u) ||
         (p >= AV_USER_BASE && end <= AV_USER_END) ||
         (p >= 0xbfc00000u && end <= 0xc0000000u);
}
uint32_t *process_pte(struct process *t, uint32_t v) {
  unsigned index = v >> 22;
  for (unsigned i = 0; i < t->table_count; i++)
    if (t->indices[i] == index)
      return &t->tables[i][(v >> 12) & 1023];
  return NULL;
}
bool process_range(struct process *t, uint32_t p, uint32_t n, bool write) {
  if (!n)
    return true;
  if (!t || !process_address(p, n))
    return false;
  uint32_t end = (p + n - 1) & ~4095u;
  for (uint32_t v = p & ~4095u;; v += 4096) {
    uint32_t *pte = process_pte(t, v);
    if (!pte || (*pte & 5) != 5 || (write && !(*pte & 2)))
      return false;
    if (v == end)
      return true;
  }
}
bool process_copyin(struct process *t, void *out, uint32_t p, uint32_t n) {
  if (!process_range(t, p, n, false))
    return false;
  uint8_t *d = out;
  while (n) {
    uint32_t count = 4096 - (p & 4095);
    if (count > n)
      count = n;
    uint32_t *pte = process_pte(t, p);
    memcpy(d, (void *)((*pte & ~4095u) + (p & 4095)), count);
    d += count;
    p += count;
    n -= count;
  }
  return true;
}
bool process_copyout(struct process *t, uint32_t p, const void *in,
                     uint32_t n) {
  if (!process_range(t, p, n, true))
    return false;
  const uint8_t *s = in;
  while (n) {
    uint32_t count = 4096 - (p & 4095);
    if (count > n)
      count = n;
    uint32_t *pte = process_pte(t, p);
    memcpy((void *)((*pte & ~4095u) + (p & 4095)), s, count);
    s += count;
    p += count;
    n -= count;
  }
  return true;
}
bool process_string(struct process *t, uint32_t p, char *out, unsigned cap) {
  for (unsigned i = 0; i < cap; i++) {
    if (!process_copyin(t, out + i, p + i, 1))
      return false;
    if (!out[i])
      return true;
  }
  return false;
}
bool process_map(struct process *t, uint32_t v, bool write) {
  if (!process_address(v, 4096))
    return false;
  uint32_t *pte = process_pte(t, v);
  if (!pte) {
    if (t->table_count == PROCESS_TABLES)
      return false;
    unsigned slot = t->table_count++;
    t->indices[slot] = v >> 22;
    memset(t->tables[slot], 0, 4096);
    t->directory[v >> 22] = (uint32_t)t->tables[slot] | 7;
    pte = &t->tables[slot][(v >> 12) & 1023];
  }
  if (*pte & ~4095u) {
    if (write)
      *pte |= 2;
    return true;
  }
  if (t->pages == PROCESS_PAGE_LIMIT)
    return false;
  void *page = page_alloc();
  if (!page)
    return false;
  *pte = (uint32_t)page | 5 | (write ? 2 : 0);
  t->pages++;
  if (t == process_current())
    __asm__ volatile("invlpg (%0)" ::"r"(v) : "memory");
  return true;
}
void process_unmap(struct process *t, uint32_t v) {
  uint32_t *pte = process_pte(t, v);
  if (pte && (*pte & ~4095u)) {
    page_free((void *)(*pte & ~4095u));
    *pte = 0;
    t->pages--;
    if (t == process_current())
      __asm__ volatile("invlpg (%0)" ::"r"(v) : "memory");
  }
}
static void release(struct process *t) {
  for (unsigned i = 0; i < t->table_count; i++)
    for (unsigned j = 0; j < 1024; j++)
      if (t->tables[i][j] & ~4095u) {
        page_free((void *)(t->tables[i][j] & ~4095u));
        t->tables[i][j] = 0;
      }
  t->pages = 0;
}
void user_init(void) {
  for (unsigned i = 0; i < 1024; i++)
    kernel_directory[i] = (i << 22) | 0x83;
  uint32_t cr4, cr0;
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  cr4 = (cr4 & ~0xa0u) | 0x10;
  __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));
  activate(kernel_directory);
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 |= 0x80010000u;
  __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
}
bool platform_mmio(uint32_t address, uint32_t size) {
  if (!size || size - 1 > 0xffffffffu - address)
    return false;
  unsigned first = address >> 22, last = (address + size - 1) >> 22;
  for (unsigned i = first; i <= last; i++)
    if (process_address(i << 22, 0x400000))
      return false;
  for (unsigned i = first; i <= last; i++) {
    kernel_directory[i] |= 0x18;
    for (unsigned j = 0; j < PROCESS_MAX; j++)
      if (tasks[j].state != TASK_EMPTY)
        tasks[j].directory[i] |= 0x18;
  }
  activate(current < 0 ? kernel_directory : tasks[current].directory);
  return true;
}
static struct process *find(int pid) {
  for (unsigned i = 0; i < PROCESS_MAX; i++)
    if (tasks[i].state != TASK_EMPTY && tasks[i].pid == pid)
      return &tasks[i];
  return NULL;
}
static struct process *empty(void) {
  for (unsigned i = 0; i < PROCESS_MAX; i++)
    if (tasks[i].state == TASK_EMPTY)
      return &tasks[i];
  return NULL;
}
static void initialize(struct process *t) {
  memset(t, 0, sizeof(*t));
  memcpy(t->directory, kernel_directory, 4096);
  t->pid = next_pid++;
  t->parent = current < 0 ? 0 : tasks[current].pid;
  t->state = TASK_EMPTY;
  linux_init_fds(t);
  platform_fpu_initial(t->fpu);
}
static void set_name(char *out, const char *s, unsigned cap) {
  unsigned n = 0;
  while (s[n] && n + 1 < cap) {
    out[n] = s[n];
    n++;
  }
  out[n] = 0;
}
static uint32_t stack_string(struct process *t, uint32_t *sp, const void *s,
                             uint32_t n) {
  *sp -= n;
  if (!process_copyout(t, *sp, s, n))
    return 0;
  return *sp;
}
static uint32_t linux_stack(struct process *t, const struct elf_header *h,
                            uint32_t phdr, const char *name, const char *args) {
  uint32_t sp = 0xc0000000u, argv[17], argc = 0, env[8], envc = 0;
  if (t->exec_vectors) {
    for (unsigned i = 0; i < t->argc; i++) {
      const char *p = t->exec_argv + t->arg_offsets[i];
      argv[argc++] = stack_string(t, &sp, p, strlen(p) + 1);
    }
    for (unsigned i = 0; i < t->envc; i++) {
      const char *p = t->exec_env + t->env_offsets[i];
      env[envc++] = stack_string(t, &sp, p, strlen(p) + 1);
    }
  } else {
    char tokens[256];
    set_name(tokens, args ? args : "", sizeof(tokens));
    argv[argc++] = stack_string(t, &sp, name, strlen(name) + 1);
    char *p = tokens;
    while (*p && argc < 16) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      char *word = p;
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
      argv[argc++] = stack_string(t, &sp, word, strlen(word) + 1);
    }
    const char e1[] = "TERM=vt100", e2[] = "PATH=/";
    env[envc++] = stack_string(t, &sp, e1, sizeof(e1));
    env[envc++] = stack_string(t, &sp, e2, sizeof(e2));
  }
  uint8_t random[16];
  uint32_t seed = clock_ticks() ^ (uint32_t)t;
  for (unsigned i = 0; i < 16; i++) {
    seed = seed * 1664525u + 1013904223u;
    random[i] = seed >> 24;
  }
  uint32_t rnd = stack_string(t, &sp, random, 16);
  uint32_t words[64], n = 0;
  words[n++] = argc;
  for (unsigned i = 0; i < argc; i++)
    words[n++] = argv[i];
  words[n++] = 0;
  for (unsigned i = 0; i < envc; i++)
    words[n++] = env[i];
  words[n++] = 0;
  uint32_t aux[] = {3,  phdr,     4,  sizeof(struct program_header),
                    5,  h->phnum, 6,  4096,
                    9,  h->entry, 11, 0,
                    12, 0,        13, 0,
                    14, 0,        23, 0,
                    25, rnd,      0,  0};
  for (unsigned i = 0; i < sizeof(aux) / 4; i++)
    words[n++] = aux[i];
  sp = (sp - n * 4) & ~15u;
  if (!process_copyout(t, sp, words, n * 4))
    return 0;
  return sp;
}
static int load(struct process *t, const void *image, uint32_t size,
                const char *name, const char *args) {
  if (size < sizeof(struct elf_header))
    return -8;
  const struct elf_header *h = image;
  if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' ||
      h->ident[3] != 'F' || h->ident[4] != 1 || h->ident[5] != 1 ||
      h->ident[6] != 1 || h->type != 2 || h->machine != 3 || h->version != 1 ||
      h->ehsize != sizeof(*h) || h->phsize != sizeof(struct program_header) ||
      !h->phnum || h->phnum > 32 || h->phoff > size ||
      h->phnum > (size - h->phoff) / sizeof(struct program_header))
    return -8;
  const struct program_header *ph =
      (const void *)((const uint8_t *)image + h->phoff);
  bool entry = false,
       native = h->entry >= AV_USER_BASE && h->entry < AV_USER_END;
  uint32_t total = 0, high = 0, phdr = 0;
  for (unsigned i = 0; i < h->phnum; i++) {
    const struct program_header *p = &ph[i];
    if (p->type == 2 || p->type == 3)
      return -8;
    if (p->type != 1)
      continue;
    if (p->filesz > p->memsz || p->offset > size ||
        p->filesz > size - p->offset)
      return -8;
    if (!p->memsz)
      continue;
    if (!process_address(p->vaddr, p->memsz) || p->vaddr >= 0xbfc00000u ||
        p->memsz > 8 * 1024 * 1024 || total > 8 * 1024 * 1024 - p->memsz)
      return -8;
    total += p->memsz;
    if (p->vaddr + p->memsz > high)
      high = p->vaddr + p->memsz;
    if ((p->flags & 1) && h->entry >= p->vaddr &&
        h->entry - p->vaddr < p->memsz)
      entry = true;
    if (h->phoff >= p->offset && h->phoff - p->offset < p->filesz)
      phdr = p->vaddr + h->phoff - p->offset;
    for (unsigned j = 0; j < i; j++)
      if (ph[j].type == 1 && ph[j].memsz &&
          p->vaddr < ph[j].vaddr + ph[j].memsz &&
          ph[j].vaddr < p->vaddr + p->memsz)
        return -8;
  }
  if (!entry)
    return -8;
  t->linux_abi = !native;
  set_name(t->name, name, sizeof(t->name));
  set_name(t->arguments, args ? args : "", sizeof(t->arguments));
  for (unsigned i = 0; i < h->phnum; i++) {
    const struct program_header *p = &ph[i];
    if (p->type != 1 || !p->memsz)
      continue;
    uint32_t last = (p->vaddr + p->memsz - 1) & ~4095u;
    for (uint32_t v = p->vaddr & ~4095u;; v += 4096) {
      if (!process_map(t, v, (p->flags & 2) != 0))
        goto memory;
      if (v == last)
        break;
    }
    for (uint32_t n = 0; n < p->filesz;) {
      uint32_t v = p->vaddr + n, amount = 4096 - (v & 4095);
      if (amount > p->filesz - n)
        amount = p->filesz - n;
      uint32_t *pte = process_pte(t, v);
      memcpy((void *)((*pte & ~4095u) + (v & 4095)),
             (const uint8_t *)image + p->offset + n, amount);
      n += amount;
    }
  }
  uint32_t stack_top = native ? AV_USER_END : 0xc0000000u;
  for (uint32_t v = stack_top - 65536; v < stack_top; v += 4096)
    if (!process_map(t, v, true))
      goto memory;
  t->frame =
      (void *)(t->kernel_stack + PROCESS_STACK - sizeof(struct trap_frame));
  memset(t->frame, 0, sizeof(*t->frame));
  t->frame->gs = t->frame->fs = t->frame->es = t->frame->ds = 35;
  t->frame->cs = 27;
  t->frame->ss = 35;
  t->frame->eflags = 0x202;
  t->frame->eip = h->entry;
  t->frame->useresp =
      native ? stack_top - 16 : linux_stack(t, h, phdr, name, args);
  if (!t->frame->useresp)
    goto memory;
  t->heap_min = t->heap_break = (high + 4095) & ~4095u;
  t->mmap_next = native ? 0x40200000u : 0x10000000u;
  t->state = TASK_READY;
  return t->pid;
memory:
  release(t);
  return -12;
}
static int load_name(struct process *t, const char *name, const char *args) {
  int n = fs_size(name);
  if (n >= 0) {
    n = fs_read(name, executable_image, sizeof(executable_image));
    return n < 0 ? -5 : load(t, executable_image, (uint32_t)n, name, args);
  }
  for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
    if (str_eq(name, builtins[i].name))
      return load(t, builtins[i].start,
                  (uint32_t)(builtins[i].end - builtins[i].start), name, args);
  return -2;
}
int user_spawn(const char *name, const char *args, bool foreground) {
  struct process *t = empty();
  if (!t)
    return -11;
  initialize(t);
  t->foreground = foreground;
  int result = load_name(t, name, args);
  if (result < 0) {
    release(t);
    linux_close_fds(t);
    t->state = TASK_EMPTY;
  }
  return result;
}
int user_run(const void *image, uint32_t size, const char *args) {
  struct process *t = empty();
  if (!t)
    return -11;
  initialize(t);
  t->foreground = true;
  int pid = load(t, image, size, "program", args);
  if (pid < 0) {
    release(t);
    return pid;
  }
  return user_wait_pid(pid);
}
int user_exec(const char *name, const char *args) {
  int pid = user_spawn(name, args, true);
  return pid < 0 ? pid : user_wait_pid(pid);
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
static void reap(struct process *t) {
  release(t);
  t->state = TASK_EMPTY;
}
static void wake_waiters(void) {
  for (unsigned i = 0; i < PROCESS_MAX; i++) {
    struct process *p = &tasks[i];
    if (p->state != TASK_WAIT)
      continue;
    for (unsigned j = 0; j < PROCESS_MAX; j++) {
      struct process *c = &tasks[j];
      if (c->state != TASK_ZOMBIE || c->parent != p->pid ||
          (p->wait_pid > 0 && p->wait_pid != c->pid))
        continue;
      uint32_t status =
          c->signal ? (uint32_t)c->signal : ((uint32_t)c->status & 255) << 8;
      if (p->wait_status && !process_copyout(p, p->wait_status, &status, 4))
        p->frame->eax = (uint32_t)-14;
      else
        p->frame->eax = c->pid;
      reap(c);
      p->state = TASK_READY;
      break;
    }
  }
}
void process_exit(struct process *t, int status, int signal) {
  if (!t || t->state == TASK_ZOMBIE || t->state == TASK_EMPTY)
    return;
  if (tcp_owner == t->pid)
    tcp_cleanup = true;
  t->status = status;
  t->signal = signal;
  t->state = TASK_ZOMBIE;
  if (t->clear_tid) {
    uint32_t zero = 0;
    process_copyout(t, t->clear_tid, &zero, 4);
  }
  linux_close_fds(t);
  for (unsigned i = 0; i < PROCESS_MAX; i++)
    if (tasks[i].parent == t->pid)
      tasks[i].parent = 0;
  wake_waiters();
}
void user_fault(unsigned vector, uint32_t eip) {
  struct process *t = process_current();
  console_write("\nProcess ");
  print_uint((uint32_t)t->pid);
  console_write(" fault ");
  print_uint(vector);
  console_write(" at ");
  print_hex(eip);
  console_putc('\n');
  int signal = (vector == 0 || vector == 16 || vector == 19) ? 8
               : vector == 6                                 ? 4
                                                             : 11;
  process_exit(t, 128 + signal, signal);
}
int user_kill(int pid, int signal) {
  struct process *t = find(pid);
  if (!t)
    return -3;
  if (!signal)
    return 0;
  if (signal != 2 && signal != 3 && signal != 9 && signal != 15)
    return -22;
  process_exit(t, 128 + signal, signal);
  return 0;
}
int process_wait(struct process *p, int pid, uint32_t status, bool nohang) {
  bool children = false;
  for (unsigned i = 0; i < PROCESS_MAX; i++) {
    struct process *c = &tasks[i];
    if (c->state == TASK_EMPTY || c->parent != p->pid ||
        (pid > 0 && c->pid != pid))
      continue;
    children = true;
    if (c->state == TASK_ZOMBIE) {
      uint32_t s =
          c->signal ? (uint32_t)c->signal : ((uint32_t)c->status & 255) << 8;
      if (status && !process_copyout(p, status, &s, 4))
        return -14;
      int id = c->pid;
      reap(c);
      return id;
    }
  }
  if (!children)
    return -10;
  if (nohang)
    return 0;
  p->state = TASK_WAIT;
  p->wait_pid = pid;
  p->wait_status = status;
  return 0;
}
void process_read(struct process *t, uint32_t address, uint32_t capacity,
                  bool linux_abi) {
  t->read_address = address;
  t->read_capacity = capacity;
  t->read_count = 0;
  t->read_linux = linux_abi;
  t->state = TASK_INPUT;
}
static void wake_sleepers(void) {
  uint32_t now = clock_ticks();
  for (unsigned i = 0; i < PROCESS_MAX; i++)
    if (tasks[i].state == TASK_SLEEP && (int32_t)(now - tasks[i].wake) >= 0)
      tasks[i].state = TASK_READY;
}
struct trap_frame *user_reschedule(struct trap_frame *f) {
  wake_sleepers();
  if (f->vector == 128 && current >= 0 &&
      tasks[current].state == TASK_RUNNING && !tasks[current].yield)
    return f;
  if (current >= 0) {
    platform_fpu_save(tasks[current].fpu);
    tasks[current].frame = f;
    tasks[current].yield = false;
    if (tasks[current].state == TASK_RUNNING)
      tasks[current].state = TASK_READY;
  } else
    kernel_frame = f;
  unsigned start = current < 0 ? PROCESS_MAX : (unsigned)current;
  for (unsigned step = 1; step <= PROCESS_MAX + 1; step++) {
    unsigned n = (start + step) % (PROCESS_MAX + 1);
    if (n == PROCESS_MAX) {
      if (!kernel_frame)
        continue;
      current = -1;
      activate(kernel_directory);
      platform_tls(NULL);
      uint8_t fpu[512] __attribute__((aligned(16)));
      platform_fpu_initial(fpu);
      platform_fpu_restore(fpu);
      return kernel_frame;
    }
    struct process *t = &tasks[n];
    if (t->state != TASK_READY)
      continue;
    current = (int)n;
    t->state = TASK_RUNNING;
    platform_kernel_stack((uint32_t)(t->kernel_stack + PROCESS_STACK));
    platform_tls(t->tls);
    platform_fpu_restore(t->fpu);
    activate(t->directory);
    return t->frame;
  }
  return f;
}
struct trap_frame *user_timer(struct trap_frame *f) {
  wake_sleepers();
  if ((f->cs & 3) != 3 || current < 0)
    return f;
  struct process *t = &tasks[current];
  t->cpu_ticks++;
  if (t->cpu_ticks >= 12000) {
    process_exit(t, 124, 0);
    return user_reschedule(f);
  }
  if (clock_ticks() % 2)
    return f;
  return user_reschedule(f);
}
void user_yield_kernel(void) {
  if (current < 0)
    __asm__ volatile("int $0x81" ::: "memory", "cc");
}
static char tty[256];
static unsigned tty_head, tty_count;
void user_poll(void) {
  if (tcp_cleanup) {
    net_tcp_close();
    tcp_owner = 0;
    tcp_cleanup = false;
  }
  wake_sleepers();
  bool foreground = false;
  for (unsigned i = 0; i < PROCESS_MAX; i++)
    if (tasks[i].foreground && tasks[i].state != TASK_EMPTY &&
        tasks[i].state != TASK_ZOMBIE)
      foreground = true;
  if (!foreground)
    return;
  for (unsigned k = 0; k < 64; k++) {
    char c = console_getchar();
    if (!c)
      break;
    if (c == 3 || c == 28) {
      for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].foreground)
          process_exit(&tasks[i], c == 3 ? 130 : 131, c == 3 ? 2 : 3);
      tty_head = tty_count = 0;
      console_write(c == 3 ? "^C\n" : "^\\\n");
      return;
    }
    if (tty_count < sizeof(tty)) {
      tty[(tty_head + tty_count) % sizeof(tty)] = c;
      tty_count++;
    }
  }
  for (unsigned i = 0; i < PROCESS_MAX && tty_count; i++) {
    struct process *t = &tasks[i];
    if (t->state != TASK_INPUT || !t->foreground)
      continue;
    while (tty_count && t->state == TASK_INPUT) {
      char c = tty[tty_head];
      tty_head = (tty_head + 1) % sizeof(tty);
      tty_count--;
      if (c == 21) {
        while (t->read_count) {
          t->read_count--;
          console_write("\b \b");
        }
        continue;
      }
      if (c == '\b') {
        if (t->read_count) {
          t->read_count--;
          console_write("\b \b");
        }
        continue;
      }
      if (c == '\n' || c == 4) {
        if (t->read_linux && c == '\n' && t->read_count < t->read_capacity) {
          process_copyout(t, t->read_address + t->read_count, &c, 1);
          t->read_count++;
        }
        if (!t->read_linux) {
          char zero = 0;
          process_copyout(t, t->read_address + t->read_count, &zero, 1);
        }
        if (c == '\n')
          console_putc('\n');
        t->frame->eax = t->read_count;
        t->state = TASK_READY;
        break;
      }
      uint32_t limit = t->read_capacity - (t->read_linux ? 0 : 1);
      if (c >= 32 && c <= 126 && t->read_count < limit) {
        process_copyout(t, t->read_address + t->read_count, &c, 1);
        t->read_count++;
        console_putc(c);
        if (t->read_linux && t->read_count == limit) {
          t->frame->eax = t->read_count;
          t->state = TASK_READY;
        }
      }
    }
  }
}
int user_wait_pid(int pid) {
  for (;;) {
    struct process *t = find(pid);
    if (!t)
      return -3;
    if (t->state == TASK_ZOMBIE) {
      int result = t->status;
      reap(t);
      return result;
    }
    net_poll();
    user_poll();
    user_yield_kernel();
  }
}
void user_list(void) {
  const char *states[] = {"empty", "ready", "running", "sleep",
                          "input", "wait",  "zombie"};
  console_write("PID PPID STATE ABI NAME\n");
  for (unsigned i = 0; i < PROCESS_MAX; i++) {
    struct process *t = &tasks[i];
    if (t->state == TASK_EMPTY)
      continue;
    print_uint((uint32_t)t->pid);
    console_putc(' ');
    print_uint((uint32_t)t->parent);
    console_putc(' ');
    console_write(states[t->state]);
    console_write(t->linux_abi ? " linux " : " native ");
    console_write(t->name);
    console_putc('\n');
  }
}
int process_fork(struct trap_frame *f) {
  struct process *p = process_current(), *c = empty();
  if (!p || !c)
    return -11;
  initialize(c);
  c->linux_abi = p->linux_abi;
  c->foreground = p->foreground;
  c->heap_min = p->heap_min;
  c->heap_break = p->heap_break;
  c->mmap_next = p->mmap_next;
  memcpy(c->tls, p->tls, sizeof(c->tls));
  platform_fpu_save(p->fpu);
  memcpy(c->fpu, p->fpu, 512);
  platform_fpu_restore(p->fpu);
  memcpy(c->name, p->name, sizeof(c->name));
  memcpy(c->arguments, p->arguments, sizeof(c->arguments));
  for (unsigned i = 0; i < p->table_count; i++)
    for (unsigned j = 0; j < 1024; j++) {
      uint32_t old = p->tables[i][j];
      if (!(old & ~4095u))
        continue;
      uint32_t v = ((uint32_t)p->indices[i] << 22) | (j << 12);
      if (!process_map(c, v, old & 2)) {
        release(c);
        return -12;
      }
      uint32_t *pte = process_pte(c, v);
      memcpy((void *)(*pte & ~4095u), (void *)(old & ~4095u), 4096);
      *pte = (*pte & ~4095u) | (old & 4095u);
    }
  c->frame = (void *)(c->kernel_stack + PROCESS_STACK - sizeof(*f));
  *c->frame = *f;
  c->frame->eax = 0;
  linux_clone_fds(c, p);
  c->state = TASK_READY;
  return c->pid;
}
static int copy_vector(struct process *old, uint32_t vector, char *data,
                       uint16_t *offsets, unsigned max, unsigned *count) {
  *count = 0;
  if (!vector)
    return 0;
  unsigned used = 0;
  for (unsigned i = 0; i <= max; i++) {
    uint32_t p;
    if (!process_copyin(old, &p, vector + i * 4, 4))
      return -14;
    if (!p)
      return 0;
    if (i == max)
      return -7;
    char word[256];
    if (!process_string(old, p, word, sizeof(word)))
      return -14;
    unsigned n = strlen(word) + 1;
    if (n > 512 - used)
      return -7;
    offsets[i] = (uint16_t)used;
    memcpy(data + used, word, n);
    used += n;
    (*count)++;
  }
  return -7;
}
int process_execve(uint32_t filename, uint32_t argv, uint32_t envp) {
  struct process *old = process_current();
  char name[64];
  if (!process_string(old, filename, name, sizeof(name)))
    return -14;
  const char *simple = name;
  while (*simple == '/')
    simple++;
  struct process *next = empty();
  if (!next)
    return -11;
  initialize(next);
  next->exec_vectors = true;
  int r = copy_vector(old, argv, next->exec_argv, next->arg_offsets, 16,
                      &next->argc);
  if (r < 0)
    return r;
  r = copy_vector(old, envp, next->exec_env, next->env_offsets, 8, &next->envc);
  if (r < 0)
    return r;
  r = load_name(next, simple, "");
  if (r < 0) {
    release(next);
    return r;
  }
  next->pid = old->pid;
  next->parent = old->parent;
  next->foreground = old->foreground;
  memcpy(next->fds, old->fds, sizeof(next->fds));
  next->cloexec = old->cloexec;
  linux_exec_fds(next);
  release(old);
  old->state = TASK_EMPTY;
  return 0;
}

static bool range(uint32_t p, uint32_t n, bool write) {
  return process_range(process_current(), p, n, write);
}
static bool user_string(uint32_t p, char *out, unsigned cap) {
  return process_string(process_current(), p, out, cap);
}
void user_syscall(struct trap_frame *f) {
  struct process *t = process_current();
  if (!t || (f->cs & 3) != 3) {
    f->eax = (uint32_t)-1;
    return;
  }
  t->frame = f;
  if (t->linux_abi) {
    linux_syscall(f);
    return;
  }
  uint32_t n = f->ecx;
  char name[32];
  int result = -1;
  switch (f->eax) {
  case AV_EXIT:
    process_exit(t, (int)f->ebx, 0);
    result = 0;
    break;
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
      if (!t->foreground)
        result = -11;
      else {
        process_read(t, f->ebx, n, false);
        result = 0;
      }
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
    if (f->ebx <= 360000) {
      t->wake = clock_ticks() + f->ebx;
      t->state = TASK_SLEEP;
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
      uint32_t len = strlen(t->arguments);
      if (len >= n)
        len = n - 1;
      memcpy((void *)f->ebx, t->arguments, len);
      ((char *)f->ebx)[len] = 0;
      result = (int)len;
    }
    break;
  case AV_FILE_SIZE:
    if (user_string(f->ebx, name, sizeof(name)))
      result = fs_size(name);
    break;
  case AV_YIELD:
    t->yield = true;
    net_poll();
    result = 0;
    break;
  case AV_TCP_CONNECT:
    if (tcp_owner && tcp_owner != t->pid) {
      result = -16;
      break;
    }
    if (f->ecx && f->ecx <= 65535) {
      result = net_tcp_connect(f->ebx, (uint16_t)f->ecx);
      if (!result)
        tcp_owner = t->pid;
    }
    break;
  case AV_TCP_SEND:
    if (tcp_owner != t->pid) {
      result = -9;
      break;
    }
    if (n <= 4096 && range(f->ebx, n, false))
      result = net_tcp_send((void *)f->ebx, n);
    break;
  case AV_TCP_RECV:
    if (tcp_owner != t->pid) {
      result = -9;
      break;
    }
    if (n <= 4096 && range(f->ebx, n, true) && f->edx <= 1000)
      result = net_tcp_recv((void *)f->ebx, n, f->edx);
    break;
  case AV_TCP_CLOSE:
    if (tcp_owner != t->pid) {
      result = -9;
      break;
    }
    result = net_tcp_close();
    tcp_owner = 0;
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
  case AV_SPAWN: {
    char program[32], args[256];
    if (user_string(f->ebx, program, sizeof(program)) &&
        user_string(f->ecx, args, sizeof(args)))
      result = user_spawn(program, args, false);
    break;
  }
  case AV_WAIT:
    result = process_wait(t, (int)f->ebx, 0, false);
    break;
  case AV_GETPID:
    result = t->pid;
    break;
  case AV_KILL:
    result = user_kill((int)f->ebx, (int)f->ecx);
    break;
  default:
    result = -38;
    break;
  }
  f->eax = (uint32_t)result;
}

char user_tty_getchar(void) {
  if (!tty_count)
    return 0;
  char c = tty[tty_head];
  tty_head = (tty_head + 1) % sizeof(tty);
  tty_count--;
  return c;
}
