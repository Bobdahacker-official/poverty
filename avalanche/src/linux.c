#include "process.h"
/* Deliberate Linux i386 syscall subset for static, int-0x80 ELF programs.
 * Unimplemented operations return -ENOSYS; this is not a Linux kernel. */
#define HANDLES 64
#define H_FILE 1
#define H_DIR 2
#define H_NULL 3
#define H_ZERO 4
struct handle {
  unsigned refs, kind;
  uint32_t flags, position;
  char name[32];
};
static struct handle handles[HANDLES];
static uint8_t contents[FS_MAX_FILE];
static void put32(uint8_t *p, uint32_t n) { memcpy(p, &n, 4); }
static void put64(uint8_t *p, uint32_t n) {
  put32(p, n);
  put32(p + 4, 0);
}
static bool filename(struct process *t, uint32_t address, char *out) {
  char path[96];
  if (!process_string(t, address, path, sizeof(path)))
    return false;
  const char *s = path;
  while (*s == '/')
    s++;
  if (!*s || str_eq(s, ".")) {
    out[0] = '/';
    out[1] = 0;
    return true;
  }
  if (strlen(s) > 31)
    return false;
  for (unsigned i = 0; s[i]; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
      return false;
    out[i] = c;
    out[i + 1] = 0;
  }
  return true;
}
void linux_init_fds(struct process *t) {
  for (unsigned i = 0; i < PROCESS_FDS; i++)
    t->fds[i] = FD_UNUSED;
  t->fds[0] = FD_INPUT;
  t->fds[1] = FD_OUTPUT;
  t->fds[2] = FD_ERROR;
}
static int close_fd(struct process *t, unsigned fd) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  int h = t->fds[fd];
  if (h >= 0 && handles[h].refs && !--handles[h].refs)
    memset(&handles[h], 0, sizeof(handles[h]));
  t->fds[fd] = FD_UNUSED;
  t->cloexec &= ~(1u << fd);
  return 0;
}
void linux_close_fds(struct process *t) {
  for (unsigned i = 0; i < PROCESS_FDS; i++)
    if (t->fds[i] != FD_UNUSED)
      close_fd(t, i);
}
void linux_clone_fds(struct process *c, struct process *p) {
  memcpy(c->fds, p->fds, sizeof(c->fds));
  c->cloexec = p->cloexec;
  for (unsigned i = 0; i < PROCESS_FDS; i++)
    if (c->fds[i] >= 0)
      handles[c->fds[i]].refs++;
}
void linux_exec_fds(struct process *t) {
  for (unsigned i = 0; i < PROCESS_FDS; i++)
    if (t->cloexec & (1u << i))
      close_fd(t, i);
}
static struct handle *get_handle(struct process *t, uint32_t fd) {
  return fd < PROCESS_FDS && t->fds[fd] >= 0 ? &handles[t->fds[fd]] : NULL;
}
static int new_fd(struct process *t, unsigned start) {
  for (unsigned i = start; i < PROCESS_FDS; i++)
    if (t->fds[i] == FD_UNUSED)
      return (int)i;
  return -24;
}
static int open_file(struct process *t, uint32_t p, uint32_t flags) {
  char path[96], name[32];
  if (!process_string(t, p, path, sizeof(path)))
    return -14;
  int fd = new_fd(t, 0);
  if (fd < 0)
    return fd;
  unsigned hi;
  for (hi = 0; hi < HANDLES; hi++)
    if (!handles[hi].refs)
      break;
  if (hi == HANDLES)
    return -23;
  unsigned kind = H_FILE;
  if (str_eq(path, "/dev/null"))
    kind = H_NULL;
  else if (str_eq(path, "/dev/zero"))
    kind = H_ZERO;
  else {
    if (!filename(t, p, name))
      return -2;
    if (str_eq(name, "/"))
      kind = H_DIR;
  }
  if ((flags & 3) == 3)
    return -22;
  if ((flags & 0x10000) && kind != H_DIR)
    return -20;
  if (kind == H_DIR && (flags & 3))
    return -21;
  if (kind == H_FILE) {
    int size = fs_size(name);
    if (size >= 0 && (flags & 64) && (flags & 128))
      return -17;
    if (size < 0) {
      if (!(flags & 64))
        return -2;
      int r = fs_write(name, "", 0);
      if (r < 0)
        return -28;
    } else if ((flags & 512) && (flags & 3)) {
      if (fs_write(name, "", 0) < 0)
        return -5;
    }
  }
  struct handle *h = &handles[hi];
  memset(h, 0, sizeof(*h));
  h->refs = 1;
  h->kind = kind;
  h->flags = flags & ~(64u | 128u | 512u | 0x80000u);
  if (kind == H_FILE)
    memcpy(h->name, name, strlen(name) + 1);
  t->fds[fd] = (int)hi;
  if (flags & 0x80000)
    t->cloexec |= 1u << fd;
  return fd;
}
static int read_fd(struct process *t, unsigned fd, uint32_t out, uint32_t n) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  if (!process_range(t, out, n, true))
    return -14;
  if (!n)
    return 0;
  if (t->fds[fd] == FD_INPUT) {
    if (!t->foreground)
      return -11;
    if (n > 4096)
      n = 4096;
    process_read(t, out, n, true);
    return 0;
  }
  if (t->fds[fd] < 0)
    return -9;
  struct handle *h = get_handle(t, fd);
  if ((h->flags & 3) == 1)
    return -9;
  if (h->kind == H_DIR)
    return -21;
  if (h->kind == H_NULL)
    return 0;
  if (h->kind == H_ZERO) {
    uint8_t zero[256] = {0};
    uint32_t done = 0;
    while (done < n) {
      uint32_t k = n - done;
      if (k > 256)
        k = 256;
      process_copyout(t, out + done, zero, k);
      done += k;
    }
    return (int)n;
  }
  int size = fs_read(h->name, contents, sizeof(contents));
  if (size < 0)
    return -5;
  if (h->position >= (unsigned)size)
    return 0;
  uint32_t count = (unsigned)size - h->position;
  if (count > n)
    count = n;
  process_copyout(t, out, contents + h->position, count);
  h->position += count;
  return (int)count;
}
static int write_fd(struct process *t, unsigned fd, uint32_t p, uint32_t n) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  if (!process_range(t, p, n, false))
    return -14;
  if (!n)
    return 0;
  if (t->fds[fd] == FD_OUTPUT || t->fds[fd] == FD_ERROR) {
    char bytes[256];
    uint32_t done = 0;
    while (done < n) {
      unsigned k = n - done;
      if (k > sizeof(bytes))
        k = sizeof(bytes);
      process_copyin(t, bytes, p + done, k);
      for (unsigned i = 0; i < k; i++)
        console_putc(bytes[i]);
      done += k;
    }
    return (int)n;
  }
  if (t->fds[fd] < 0)
    return -9;
  struct handle *h = get_handle(t, fd);
  if (!(h->flags & 3))
    return -9;
  if (h->kind == H_DIR)
    return -21;
  if (h->kind == H_NULL || h->kind == H_ZERO)
    return (int)n;
  int old = fs_read(h->name, contents, sizeof(contents));
  if (old < 0)
    return -5;
  if (h->flags & 1024)
    h->position = (uint32_t)old;
  if (h->position > FS_MAX_FILE || n > FS_MAX_FILE - h->position)
    return -27;
  if (h->position > (uint32_t)old)
    memset(contents + old, 0, h->position - (uint32_t)old);
  process_copyin(t, contents + h->position, p, n);
  uint32_t end = h->position + n;
  if (end < (uint32_t)old)
    end = (uint32_t)old;
  if (fs_write(h->name, contents, end) < 0)
    return -5;
  h->position += n;
  return (int)n;
}
static int seek_fd(struct process *t, unsigned fd, int32_t offset,
                   unsigned whence) {
  struct handle *h = get_handle(t, fd);
  if (!h)
    return -9;
  if (h->kind != H_FILE && h->kind != H_DIR)
    return -29;
  if (whence > 2)
    return -22;
  int size = h->kind == H_FILE ? fs_size(h->name) : 34;
  if (size < 0)
    return -5;
  int64_t base = whence == 0   ? 0
                 : whence == 1 ? (int64_t)h->position
                               : (int64_t)size;
  int64_t pos = base + offset;
  if (pos < 0 || pos > FS_MAX_FILE)
    return -22;
  h->position = (uint32_t)pos;
  return (int)pos;
}
static int duplicate(struct process *t, unsigned fd, int target, bool exact) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  if (target < 0 || target >= PROCESS_FDS)
    return -9;
  if (exact && (unsigned)target == fd)
    return target;
  if (!exact) {
    target = new_fd(t, (unsigned)target);
    if (target < 0)
      return target;
  } else if (t->fds[target] != FD_UNUSED)
    close_fd(t, (unsigned)target);
  t->fds[target] = t->fds[fd];
  if (t->fds[target] >= 0)
    handles[t->fds[target]].refs++;
  t->cloexec &= ~(1u << target);
  return target;
}
static int stat_data(struct process *t, uint32_t out, bool wide, unsigned kind,
                     const char *name) {
  uint8_t b[96] = {0};
  uint32_t size = 0,
           mode = kind == H_DIR    ? 0040755
                  : kind == H_FILE ? 0100644
                                   : 0020666,
           ino = 1;
  if (kind == H_FILE) {
    int n = fs_size(name);
    if (n < 0)
      return -2;
    size = (uint32_t)n;
    ino = crc32(name, strlen(name));
  }
  if (wide) {
    put64(b, 1);
    put32(b + 12, ino);
    put32(b + 16, mode);
    put32(b + 20, 1);
    put64(b + 44, size);
    put32(b + 52, 4096);
    put64(b + 56, (size + 511) / 512);
    put64(b + 88, ino);
  } else {
    put32(b, 1);
    put32(b + 4, ino);
    uint16_t m = (uint16_t)mode, links = 1;
    memcpy(b + 8, &m, 2);
    memcpy(b + 10, &links, 2);
    put32(b + 20, size);
    put32(b + 24, 4096);
    put32(b + 28, (size + 511) / 512);
  }
  return process_copyout(t, out, b, wide ? 96 : 64) ? 0 : -14;
}
static int stat_fd(struct process *t, unsigned fd, uint32_t out, bool wide) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  if (t->fds[fd] < 0)
    return stat_data(t, out, wide, H_NULL, "");
  struct handle *h = get_handle(t, fd);
  return stat_data(t, out, wide, h->kind, h->name);
}
static int stat_path(struct process *t, uint32_t path, uint32_t out,
                     bool wide) {
  char name[32];
  if (!filename(t, path, name))
    return -2;
  return stat_data(t, out, wide, str_eq(name, "/") ? H_DIR : H_FILE, name);
}
static int directory_read(struct process *t, unsigned fd, uint32_t out,
                          uint32_t cap) {
  struct handle *h = get_handle(t, fd);
  if (!h)
    return -9;
  if (h->kind != H_DIR)
    return -20;
  if (!process_range(t, out, cap, true))
    return -14;
  uint32_t done = 0;
  while (h->position < 34) {
    char name[32];
    uint32_t size, slot = h->position;
    if (slot < 2) {
      name[0] = '.';
      name[1] = slot ? '.' : 0;
      name[slot ? 2 : 1] = 0;
    } else if (!fs_entry(slot - 2, name, &size)) {
      h->position++;
      continue;
    }
    unsigned len = strlen(name), record = (19 + len + 1 + 7) & ~7u;
    if (record > cap - done)
      return done ? (int)done : -22;
    uint8_t b[64] = {0};
    put64(b, slot + 1);
    put64(b + 8, slot + 1);
    uint16_t r = (uint16_t)record;
    memcpy(b + 16, &r, 2);
    b[18] = slot < 2 ? 4 : 8;
    memcpy(b + 19, name, len + 1);
    process_copyout(t, out + done, b, record);
    done += record;
    h->position++;
  }
  return (int)done;
}
static int memory_map(struct process *t, uint32_t address, uint32_t length,
                      uint32_t prot, uint32_t flags) {
  if (!length || length > 4 * 1024 * 1024 || !(flags & 0x20) || !(flags & 2) ||
      (flags & 1) || (prot & ~7u))
    return -22;
  uint32_t n = (length + 4095) & ~4095u;
  if (address & 4095)
    return -22;
  if (!address)
    address = t->mmap_next;
  if (!process_address(address, n))
    return -12;
  for (uint32_t off = 0; off < n; off += 4096) {
    uint32_t *pte = process_pte(t, address + off);
    if (pte && (*pte & ~4095u))
      return -12;
  }
  uint32_t done = 0;
  for (; done < n; done += 4096) {
    if (!process_map(t, address + done, (prot & 2) != 0))
      break;
    if (!prot)
      *process_pte(t, address + done) &= ~4u;
  }
  if (done < n) {
    for (uint32_t off = 0; off < done; off += 4096)
      process_unmap(t, address + off);
    return -12;
  }
  t->mmap_next = address + n;
  return (int)address;
}
static int tls_set(struct process *t, uint32_t address) {
  uint32_t data[4];
  if (!process_copyin(t, data, address, 16) ||
      !process_range(t, address, 16, true))
    return -14;
  int index = (int)data[0];
  if (index == -1) {
    for (index = 6; index < 9; index++)
      if (!t->tls[index - 6])
        break;
  }
  if (index < 6 || index > 8 || data[2] > 0xfffff || (data[3] & 6) ||
      !(data[3] & 1) || data[3] & ~0x7fu)
    return -22;
  uint32_t base = data[1], limit = data[2], f = data[3];
  uint8_t access = (f & 32) ? 0x72 : 0xf2;
  if (f & 8)
    access &= ~2u;
  uint8_t high = ((limit >> 16) & 15) | 0x40 | ((f & 16) ? 0x80 : 0);
  t->tls[index - 6] = (limit & 65535ULL) | ((uint64_t)(base & 0xffffff) << 16) |
                      ((uint64_t)access << 40) | ((uint64_t)high << 48) |
                      ((uint64_t)(base >> 24) << 56);
  data[0] = (uint32_t)index;
  process_copyout(t, address, data, 16);
  platform_tls(t->tls);
  return 0;
}
static int time_value(struct process *t, uint32_t out, bool nano) {
  uint32_t ticks = clock_ticks(),
           v[2] = {ticks / 100, (ticks % 100) * (nano ? 10000000u : 10000u)};
  return process_copyout(t, out, v, 8) ? 0 : -14;
}
void linux_syscall(struct trap_frame *f) {
  struct process *t = process_current();
  uint32_t a = f->ebx, b = f->ecx, c = f->edx;
  int result = -38;
  switch (f->eax) {
  case 1:
  case 252:
    process_exit(t, (int)(a & 255), 0);
    result = 0;
    break;
  case 2:
    result = process_fork(f);
    break;
  case 3:
    result = read_fd(t, a, b, c);
    break;
  case 4:
    result = write_fd(t, a, b, c);
    break;
  case 5:
    result = open_file(t, a, b);
    break;
  case 6:
    result = close_fd(t, a);
    break;
  case 7:
    if ((int)a == 0 || (int)a < -1) {
      result = -38;
      break;
    }
    if (b && !process_range(t, b, 4, true))
      result = -14;
    else if (c & ~1u)
      result = -22;
    else
      result = process_wait(t, (int)a, b, c & 1);
    break;
  case 10: {
    char name[32];
    result = filename(t, a, name) ? (fs_remove(name) < 0 ? -2 : 0) : -14;
    break;
  }
  case 11:
    result = process_execve(a, b, c);
    break;
  case 19:
    result = seek_fd(t, a, (int32_t)b, c);
    break;
  case 20:
  case 224:
    result = t->pid;
    break;
  case 24:
  case 47:
  case 49:
  case 50:
  case 199:
  case 200:
  case 201:
  case 202:
    result = 0;
    break;
  case 37:
    result = user_kill((int)a, (int)b);
    break;
  case 41:
    result = duplicate(t, a, 0, false);
    break;
  case 45: {
    if (!a) {
      result = (int)t->heap_break;
      break;
    }
    uint32_t limit = t->linux_abi ? 0x10000000u : 0x403e0000u;
    if (a < t->heap_min || a > limit) {
      result = (int)t->heap_break;
      break;
    }
    uint32_t old = (t->heap_break + 4095) & ~4095u, end = (a + 4095) & ~4095u;
    bool ok = true;
    uint32_t v = old;
    for (; v < end; v += 4096)
      if (!process_map(t, v, true)) {
        ok = false;
        break;
      }
    if (!ok) {
      for (uint32_t x = old; x < v; x += 4096)
        process_unmap(t, x);
    } else {
      for (uint32_t x = end; x < old; x += 4096)
        process_unmap(t, x);
      t->heap_break = a;
    }
    result = (int)t->heap_break;
    break;
  }
  case 54: {
    if (a >= PROCESS_FDS || t->fds[a] == FD_UNUSED) {
      result = -9;
      break;
    }
    if (t->fds[a] >= 0) {
      result = -25;
      break;
    }
    if (b == 0x5413) {
      uint16_t ws[4] = {0};
      console_size(&ws[0], &ws[1]);
      result = process_copyout(t, c, ws, sizeof(ws)) ? 0 : -14;
    } else if (b == 0x5401) {
      uint8_t term[36] = {0};
      put32(term, 0x100);
      put32(term + 4, 5);
      put32(term + 8, 0x18b2);
      put32(term + 12, 11);
      term[17] = 3;
      term[18] = 28;
      term[19] = 127;
      term[20] = 21;
      term[21] = 4;
      term[23] = 1;
      result = process_copyout(t, c, term, sizeof(term)) ? 0 : -14;
    } else
      result = -25;
    break;
  }
  case 55:
  case 221: {
    if (a >= PROCESS_FDS || t->fds[a] == FD_UNUSED) {
      result = -9;
      break;
    }
    if (b == 0)
      result = duplicate(t, a, (int)c, false);
    else if (b == 1)
      result = (t->cloexec >> a) & 1;
    else if (b == 2) {
      if (c & ~1u)
        result = -22;
      else {
        t->cloexec = (t->cloexec & ~(1u << a)) | ((c & 1) << a);
        result = 0;
      }
    } else if (b == 3) {
      struct handle *h = get_handle(t, a);
      result = h ? (int)h->flags : t->fds[a] == FD_INPUT ? 0 : 1;
    } else if (b == 4) {
      struct handle *h = get_handle(t, a);
      if (!h)
        result = -22;
      else {
        h->flags = (h->flags & ~(1024u | 2048u)) | (c & (1024u | 2048u));
        result = 0;
      }
    } else
      result = -22;
    break;
  }
  case 63:
    result = duplicate(t, a, (int)b, true);
    break;
  case 64:
    result = t->parent;
    break;
  case 85: {
    const char *target = "";
    char path[96];
    if (!process_string(t, a, path, sizeof(path)))
      result = -14;
    else if (str_eq(path, "/proc/self/exe")) {
      target = t->name;
      unsigned n = strlen(target);
      if (n > c)
        n = c;
      result = process_copyout(t, b, target, n) ? (int)n : -14;
    } else
      result = -2;
    break;
  }
  case 90: {
    uint32_t v[6];
    result = process_copyin(t, v, a, sizeof(v))
                 ? memory_map(t, v[0], v[1], v[2], v[3])
                 : -14;
    break;
  }
  case 91: {
    if ((a & 4095) || !b || b > 4 * 1024 * 1024 || !process_address(a, b)) {
      result = -22;
      break;
    }
    for (uint32_t off = 0; off < b; off += 4096)
      process_unmap(t, a + off);
    result = 0;
    break;
  }
  case 106:
  case 107:
    result = stat_path(t, a, b, false);
    break;
  case 108:
    result = stat_fd(t, a, b, false);
    break;
  case 120:
    result =
        ((a & ~255u) == 0 && (a & 255) == 17 && !b) ? process_fork(f) : -38;
    break;
  case 122: {
    char uts[6][65] = {{0}};
    const char *values[] = {"Linux",           "avalanche", "6.0.0-avalanche",
                            "Avalanche 0.3.0", "i686",      "localdomain"};
    for (unsigned i = 0; i < 6; i++)
      memcpy(uts[i], values[i], strlen(values[i]));
    result = process_copyout(t, a, uts, sizeof(uts)) ? 0 : -14;
    break;
  }
  case 125: {
    if ((a & 4095) || !b || b > 4 * 1024 * 1024 || !process_address(a, b) ||
        (c & ~7u)) {
      result = -22;
      break;
    }
    bool ok = true;
    for (uint32_t off = 0; off < b; off += 4096) {
      uint32_t *pte = process_pte(t, a + off);
      if (!pte || !(*pte & ~4095u)) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      result = -12;
      break;
    }
    for (uint32_t off = 0; off < b; off += 4096) {
      uint32_t *pte = process_pte(t, a + off);
      *pte = (*pte & ~6u) | (c ? 4 : 0) | ((c & 2) ? 2 : 0);
      __asm__ volatile("invlpg (%0)" ::"r"(a + off) : "memory");
    }
    result = 0;
    break;
  }
  case 140: {
    if (a >= PROCESS_FDS) {
      result = -9;
      break;
    }
    if ((b != 0 && b != 0xffffffffu) || (b == 0 && c > 0x7fffffffu) ||
        (b == 0xffffffffu && c < 0x80000000u)) {
      result = -22;
      break;
    }
    int n = seek_fd(t, a, (int32_t)c, f->edi);
    if (n < 0)
      result = n;
    else {
      uint32_t offset[2] = {(uint32_t)n, 0};
      result = process_copyout(t, f->esi, offset, 8) ? 0 : -14;
    }
    break;
  }
  case 158:
    t->yield = true;
    result = 0;
    break;
  case 162: {
    uint32_t ts[2];
    if (!process_copyin(t, ts, a, 8))
      result = -14;
    else if (ts[0] > 3600 || ts[1] >= 1000000000)
      result = -22;
    else if (b && !process_range(t, b, 8, true))
      result = -14;
    else {
      if (b) {
        uint32_t zero[2] = {0};
        process_copyout(t, b, zero, 8);
      }
      t->wake = clock_ticks() + ts[0] * 100 + (ts[1] + 9999999) / 10000000;
      t->state = TASK_SLEEP;
      result = 0;
    }
    break;
  }
  case 183: {
    char root[2] = {'/', 0};
    result = b < 2 ? -34 : process_copyout(t, a, root, 2) ? (int)a : -14;
    break;
  }
  case 192:
    result = memory_map(t, a, b, c, f->esi);
    break;
  case 195:
  case 196:
    result = stat_path(t, a, b, true);
    break;
  case 197:
    result = stat_fd(t, a, b, true);
    break;
  case 220:
    result = directory_read(t, a, b, c);
    break;
  case 243:
    result = tls_set(t, a);
    break;
  case 258:
    if (a && !process_range(t, a, 4, true))
      result = -14;
    else {
      t->clear_tid = a;
      result = t->pid;
    }
    break;
  case 265:
    if (a == 1 || a == 4 || a == 6)
      result = time_value(t, b, true);
    else
      result = -22;
    break;
  case 295:
    result = (int)a == -100 ? open_file(t, b, c) : -9;
    break;

  default:
    break;
  }
  f->eax = (uint32_t)result;
}
