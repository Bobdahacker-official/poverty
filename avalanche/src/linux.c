#include "sandbox.h"
#include "tcp.h"
#include "abi.h"
#include "client.h"
#include "process.h"
/* Linux i386 int-0x80 and x86-64 SYSCALL interfaces for static ELF programs. */
#define HANDLES 64
#define H_FILE 1
#define H_DIR 2
#define H_NULL 3
#define H_ZERO 4
#define H_SOCKET 5
struct handle {
  unsigned refs, kind;
  uint32_t flags, position;
  uint32_t remote;
  uint16_t port;
  int socket_error, owner;
  bool connected, listening, reuse;
  int tcp_id;
  uint32_t bind_address;
  uint16_t bind_port;
  char name[256];
};
static struct handle handles[HANDLES];
bool linux_disk_busy(unsigned disk) {
  for (unsigned i = 0; i < HANDLES; i++) {
    unsigned volume;
    if (handles[i].refs && (handles[i].kind == H_FILE || handles[i].kind == H_DIR) &&
        fs_secondary_path(handles[i].name, &volume, NULL) && volume == disk)
      return true;
  }
  return false;
}
static bool trace_enabled;
static unsigned trace_count;
void linux_trace_set(bool enabled) {
  trace_enabled = enabled;
  trace_count = 0;
}
static uint8_t contents[FS_MAX_FILE];
static void put32(uint8_t *p, uint32_t n) { memcpy(p, &n, 4); }
static void put64(uint8_t *p, uint32_t n) {
  put32(p, n);
  put32(p + 4, 0);
}
static bool filename(struct process *t, uint32_t address, char *out) {
  char path[256];
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
  if (strlen(s) > 255)
    return false;
  for (unsigned i = 0; s[i]; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
          c == '/'))
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
  if (h >= 0 && handles[h].refs && !--handles[h].refs) {
    if (handles[h].kind == H_SOCKET && handles[h].tcp_id >= 0) net_tcp_socket_close(handles[h].tcp_id);
    memset(&handles[h], 0, sizeof(handles[h]));
  }
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
  if (!sandbox_has(t, (flags & 3) ? AV_CAP_WRITE : AV_CAP_READ)) return -13;
  if ((flags & (64 | 512)) && !sandbox_has(t, AV_CAP_WRITE)) return -13;
  char path[256], name[256];
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
    if (fs_directory(name) == 0)
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
  if (kind == H_FILE || kind == H_DIR)
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
  if ((h->kind == H_SOCKET && !sandbox_has(t, AV_CAP_NETWORK)) ||
      ((h->kind == H_FILE || h->kind == H_DIR) && !sandbox_has(t, AV_CAP_READ))) return -13;
  if (h->kind == H_SOCKET) {
    if (!h->connected)
      return -107;
    unsigned count = n;
    if (count > sizeof(contents))
      count = sizeof(contents);
    int r = net_tcp_socket_recv(h->tcp_id, contents, count, (h->flags & 2048) ? 0 : 500);
    if (r < 0)
      return r;
    if (!process_copyout(t, out, contents, r))
      return -14;
    return r;
  }
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
  uint32_t done = 0;
  while (done < n) {
    unsigned count = n - done;
    if (count > sizeof(contents))
      count = sizeof(contents);
    int r = fs_read_at(h->name, h->position, contents, count);
    if (r < 0)
      return done ? (int)done : r;
    if (!r)
      break;
    process_copyout(t, out + done, contents, r);
    h->position += r;
    done += r;
    if ((unsigned)r < count)
      break;
  }
  return (int)done;
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
  if ((h->kind == H_SOCKET && !sandbox_has(t, AV_CAP_NETWORK)) ||
      (h->kind == H_FILE && !sandbox_has(t, AV_CAP_WRITE))) return -13;
  if (h->kind == H_SOCKET) {
    if (!h->connected)
      return -107;
    unsigned done = 0;
    while (done < n) {
      unsigned count = n - done;
      if (count > sizeof(contents))
        count = sizeof(contents);
      if (!process_copyin(t, contents, p + done, count))
        return done ? (int)done : -14;
      int r = net_tcp_socket_send(h->tcp_id, contents, count, (h->flags & 2048) != 0);
      if (r < 0)
        return done ? (int)done : r;
      done += r;
    }
    return done;
  }
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
    h->position = (uintptr_t)old;
  if (h->position > FS_MAX_FILE || n > FS_MAX_FILE - h->position)
    return -27;
  if (h->position > (uintptr_t)old)
    memset(contents + old, 0, h->position - (uintptr_t)old);
  process_copyin(t, contents + h->position, p, n);
  uint32_t end = h->position + n;
  if (end < (uintptr_t)old)
    end = (uintptr_t)old;
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
  if (pos < 0 || pos > INT32_MAX)
    return -22;
  h->position = (uintptr_t)pos;
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
    size = (uintptr_t)n;
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
  char name[256];
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
  while (h->position < 0x7fffffffu) {
    char name[256];
    uint64_t size;
    uint32_t slot = h->position;
    bool directory = slot < 2;
    if (slot < 2) {
      name[0] = '.';
      name[1] = slot ? '.' : 0;
      name[slot ? 2 : 1] = 0;
    } else if (fs_browse(h->name, slot - 2, name, &size, &directory)) {
      break;
    }
    unsigned len = strlen(name), record = (19 + len + 1 + 7) & ~7u;
    if (record > cap - done)
      return done ? (int)done : -22;
    uint8_t b[280] = {0};
    put64(b, slot + 1);
    put64(b + 8, slot + 1);
    uint16_t r = (uint16_t)record;
    memcpy(b + 16, &r, 2);
    b[18] = directory ? 4 : 8;
    memcpy(b + 19, name, len + 1);
    process_copyout(t, out + done, b, record);
    done += record;
    h->position++;
  }
  return (int)done;
}
static int memory_map(struct process *t, uint32_t address, uint32_t length,
                      uint32_t prot, uint32_t flags) {
  if ((prot & 4) && (!sandbox_has(t, AV_CAP_EXEC_MEMORY) ||
      (t->capabilities != AV_CAP_ALL && (prot & 2)))) return -13;
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
    uint64_t *pte = process_pte(t, address + off);
    if (pte && *pte)
      return -12;
  }
  uint32_t done = 0;
  for (; done < n; done += 4096) {
    if (!process_map_lazy(t, address + done, (prot & 2) != 0))
      break;
    process_execute(t, address + done, (prot & 4) != 0);
    if (!prot)
      *process_pte(t, address + done) &= ~UINT64_C(4);
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
    for (index = 8; index < 11; index++)
      if (!t->tls[index - 8])
        break;
  }
  if (index < 8 || index > 10 || data[2] > 0xfffff || (data[3] & 6) ||
      !(data[3] & 1) || data[3] & ~0x7fu)
    return -22;
  uint32_t base = data[1], limit = data[2], f = data[3];
  uint8_t access = (f & 32) ? 0x72 : 0xf2;
  if (f & 8)
    access &= ~2u;
  uint8_t high = ((limit >> 16) & 15) | 0x40 | ((f & 16) ? 0x80 : 0);
  t->tls[index - 8] = (limit & 65535ULL) | ((uint64_t)(base & 0xffffff) << 16) |
                      ((uint64_t)access << 40) | ((uint64_t)high << 48) |
                      ((uint64_t)(base >> 24) << 56);
  data[0] = (uintptr_t)index;
  process_copyout(t, address, data, 16);
  platform_tls(t->tls);
  return 0;
}
static int time_value(struct process *t, uint32_t out, bool nano) {
  uint32_t ticks = clock_ticks(),
           v[2] = {ticks / 100, (ticks % 100) * (nano ? 10000000u : 10000u)};
  return process_copyout(t, out, v, 8) ? 0 : -14;
}
static int socket_shutdown(struct process *,unsigned,unsigned);
static int socket_create(struct process *,unsigned,unsigned,unsigned);
static int socket_bind(struct process *,unsigned,uint32_t,unsigned);
static int socket_listen(struct process *,unsigned,unsigned);
static int socket_accept(struct process *,unsigned,uint32_t,uint32_t,unsigned);
static int socket_connect(struct process *,unsigned,uint32_t,unsigned);
static int socket_address(struct process *,unsigned,uint32_t,uint32_t,bool);
static int socket_transfer(struct process *,unsigned,uint32_t,unsigned,unsigned,bool);
void linux_syscall(struct trap_frame *f) {
  struct process *t = process_current();
  if (!sandbox_linux(t, f->eax, false)) { f->eax = (uint32_t)-13; return; }
  uint32_t a = f->ebx, b = f->ecx, c = f->edx;
  int result = -38;
  switch (f->eax) {
  case 102: {
    uint32_t args[6]={0};unsigned count=a==1 || a==2 || a==3 || a==5 || a==6 || a==7?3:a==4 || a==13?2:a==9 || a==10?4:0;
    if(!count)result=-95;
    else if(!process_copyin(t,args,b,count*4))result=-14;
    else switch(a) {
    case 1:result=socket_create(t,args[0],args[1],args[2]);break;
    case 2:result=socket_bind(t,args[0],args[1],args[2]);break;
    case 3:result=socket_connect(t,args[0],args[1],args[2]);break;
    case 4:result=socket_listen(t,args[0],args[1]);break;
    case 5:result=socket_accept(t,args[0],args[1],args[2],0);break;
    case 6:case 7:result=socket_address(t,args[0],args[1],args[2],a==7);break;
    case 13:result=socket_shutdown(t,args[0],args[1]);break;
    case 9:case 10:result=socket_transfer(t,args[0],args[1],args[2],args[3],a==9);break;
    }
    break;
  }
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
    char name[256];
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
    uint32_t limit = t->linux_abi ? 0x10000000u : AV_USER_END - 65536u;
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
    struct handle *socket = get_handle(t, a);
    if (socket && socket->kind == H_SOCKET && b == 0x5421) {
      uint32_t flag;
      if (!process_copyin(t, &flag, c, 4))
        result = -14;
      else {
        socket->flags = (socket->flags & ~2048u) | (flag ? 2048u : 0);
        result = 0;
      }
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
    char path[256];
    if (!process_string(t, a, path, sizeof(path)))
      result = -14;
    else if (str_eq(path, "/proc/self/exe")) {
      char absolute[sizeof(t->name) + 2];
      unsigned prefix = t->name[0] == '/' ? 0 : 1;
      if (prefix)
        absolute[0] = '/';
      memcpy(absolute + prefix, t->name, strlen(t->name) + 1);
      target = absolute;
      unsigned n = strlen(target);
      if (n > c)
        n = c;
      result = process_copyout(t, b, target, n) ? (int)n : -14;
    } else {
      char name[256];
      struct file_metadata metadata;
      result = filename(t, a, name) && !fs_metadata(name, &metadata) ? -22 : -2;
    }
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
                            "Avalanche 0.4.0", "i686",      "localdomain"};
    for (unsigned i = 0; i < 6; i++)
      memcpy(uts[i], values[i], strlen(values[i]));
    result = process_copyout(t, a, uts, sizeof(uts)) ? 0 : -14;
    break;
  }
  case 125: {
    if ((c & 4) && (!sandbox_has(t, AV_CAP_EXEC_MEMORY) ||
        (t->capabilities != AV_CAP_ALL && (c & 2)))) { result = -13; break; }
    if ((a & 4095) || !b || b > 4 * 1024 * 1024 || !process_address(a, b) ||
        (c & ~7u)) {
      result = -22;
      break;
    }
    bool ok = true;
    for (uint32_t off = 0; off < b; off += 4096) {
      uint64_t *pte = process_pte(t, a + off);
      if (!pte || !*pte) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      result = -12;
      break;
    }
    for (uint32_t off = 0; off < b; off += 4096) {
      uint64_t *pte = process_pte(t, a + off);
      if ((c & 2) && (*pte & PROCESS_PHYSICAL_MASK) &&
          !process_private(t, a + off)) {
        result = -12;
        ok = false;
        break;
      }
      *pte = (*pte & ~UINT64_C(6)) | (c ? 4 : 0) | ((c & 2) ? 2 : 0);
      if (c & 2)
        *pte &= ~UINT64_C(512);
      process_execute(t, a + off, (c & 4) != 0);
      __asm__ volatile("invlpg (%0)" ::"r"((uintptr_t)(a + off)) : "memory");
    }
    if (ok)
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
      uint32_t offset[2] = {(uintptr_t)n, 0};
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
  f->eax = (uintptr_t)result;
}

struct linux64_stat {
  uint64_t device, inode, links;
  uint32_t mode, uid, gid, pad;
  uint64_t rdev;
  int64_t size, block_size, blocks;
  int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, reserved[3];
};
_Static_assert(sizeof(struct linux64_stat) == 144, "Linux x86-64 stat ABI");
static int stat64_data(struct process *t, uint32_t out, unsigned kind,
                       const char *name) {
  struct linux64_stat s = {0};
  s.block_size = 4096;
  if (kind == H_FILE || kind == H_DIR) {
    struct file_metadata m;
    int r = fs_metadata(name, &m);
    if (r)
      return r;
    s.device = 1;
    s.inode = m.inode;
    s.links = m.links;
    s.mode = m.mode;
    s.uid = m.uid;
    s.gid = m.gid;
    s.size = m.size;
    s.blocks = m.blocks;
    s.atime = m.atime;
    s.mtime = m.mtime;
    s.ctime = m.ctime;
  } else {
    s.mode = 0020666;
    s.links = 1;
    s.rdev = kind == H_NULL ? 0x103 : kind == H_ZERO ? 0x105 : 0x501;
    s.inode = s.rdev;
  }
  return process_copyout(t, out, &s, sizeof(s)) ? 0 : -14;
}
static int stat64_fd(struct process *t, unsigned fd, uint32_t out) {
  if (fd >= PROCESS_FDS || t->fds[fd] == FD_UNUSED)
    return -9;
  if (t->fds[fd] < 0)
    return stat64_data(t, out, 0, "");
  struct handle *h = get_handle(t, fd);
  if ((h->kind == H_FILE || h->kind == H_DIR) && !sandbox_has(t, AV_CAP_READ)) return -13;
  return stat64_data(t, out, h->kind, h->name);
}
static int stat64_path(struct process *t, uint32_t path, uint32_t out) {
  char name[256], raw[256];
  if (!process_string(t, path, raw, sizeof(raw)))
    return -14;
  if (str_eq(raw, "/dev/null"))
    return stat64_data(t, out, H_NULL, "");
  if (str_eq(raw, "/dev/zero"))
    return stat64_data(t, out, H_ZERO, "");
  if (!filename(t, path, name))
    return -2;
  return stat64_data(t, out, H_FILE, name);
}
static int clock64(struct process *t, unsigned clock, uint32_t out,
                   bool micro) {
  uint64_t stamp[2] = {0};
  uint32_t ticks = clock_ticks();
  if (clock == 0) {
    static bool valid;
    static uint64_t epoch;
    static uint32_t base;
    if (!valid) {
      uint32_t days, seconds;
      if (!rtc_time(&days, &seconds) || days < 719528)
        return -5;
      epoch = (uint64_t)(days - 719528) * 86400 + seconds;
      base = ticks;
      valid = true;
    }
    uint32_t elapsed = ticks - base;
    stamp[0] = epoch + elapsed / 100;
    stamp[1] = (elapsed % 100) * (micro ? 10000u : 10000000u);
  } else if (clock == 1 || clock == 4 || clock == 6) {
    stamp[0] = ticks / 100;
    stamp[1] = (ticks % 100) * (micro ? 10000u : 10000000u);
  } else
    return -22;
  return process_copyout(t, out, stamp, sizeof(stamp)) ? 0 : -14;
}
static int limit64(struct process *t, unsigned resource, uint32_t out) {
  uint64_t value;
  switch (resource) {
  case 3:
    value = 65536;
    break;
  case 7:
    value = PROCESS_FDS;
    break;
  case 9:
    value = PROCESS_PAGE_LIMIT * UINT64_C(4096);
    break;
  case 6:
    value = PROCESS_MAX;
    break;
  default:
    return -22;
  }
  uint64_t pair[2] = {value, value};
  return process_copyout(t, out, pair, sizeof(pair)) ? 0 : -14;
}
static int linux64_vector(struct process *t, unsigned fd, uint32_t address,
                          uint32_t count, bool write) {
  if (count > 1024 || !process_range(t, address, count * 16, false))
    return -14;
  int done = 0;
  for (unsigned i = 0; i < count; i++) {
    uint64_t v[2];
    if (!process_copyin(t, v, address + i * 16, 16) || v[0] > UINT32_MAX ||
        v[1] > INT32_MAX)
      return done ? done : -14;
    int r = write ? write_fd(t, fd, v[0], v[1]) : read_fd(t, fd, v[0], v[1]);
    if (r < 0)
      return done ? done : r;
    done += r;
    if ((uint64_t)r < v[1] || t->state != TASK_RUNNING)
      break;
  }
  return done;
}
struct linux_pollfd {
  int32_t fd;
  uint16_t events, revents;
};
static int poll_descriptors(struct process *t, uint32_t address,
                            unsigned count) {
  if (count > 256 ||
      (count &&
       !process_range(t, address, count * sizeof(struct linux_pollfd), true)))
    return -14;
  unsigned ready = 0;
  net_poll();
  struct tcp_snapshot state;
  net_tcp_snapshot(&state);
  for (unsigned i = 0; i < count; i++) {
    struct linux_pollfd p;
    if (!process_copyin(t, &p, address + i * sizeof(p), sizeof(p)))
      return -14;
    p.revents = 0;
    if (p.fd >= 0) {
      if (p.fd >= PROCESS_FDS || t->fds[p.fd] == FD_UNUSED)
        p.revents = 32;
      else {
        struct handle *h = get_handle(t, p.fd);
        if (h && h->kind == H_SOCKET) {
          net_tcp_socket_snapshot(h->tcp_id, &state);
          if (h->listening && (p.events & 1) && net_tcp_pending(h->tcp_id)) p.revents |= 1;
          if (h->socket_error || (h->connected && state.reset))
            p.revents |= 8;
          if (h->connected) {
            if ((p.events & 1) && (state.queued || state.closed))
              p.revents |= 1;
            if ((p.events & 4) && state.active && state.established &&
                !state.reset)
              p.revents |= 4;
            if ((p.events & 0x2000) && state.closed) p.revents |= 0x2000;
            if (net_tcp_hungup(h->tcp_id))
              p.revents |= 16;
          }
        } else {
          if ((p.events & 1) && h && (h->flags & 3) != 1)
            p.revents |= 1;
          if ((p.events & 4) &&
              (t->fds[p.fd] == FD_OUTPUT || t->fds[p.fd] == FD_ERROR ||
               (h && (h->flags & 3))))
            p.revents |= 4;
        }
      }
    }
    if (p.revents)
      ready++;
    if (!process_copyout(t, address + i * sizeof(p), &p, sizeof(p)))
      return -14;
  }
  return ready;
}
void linux_poll_wake(struct process *t) {
  int ready = poll_descriptors(t, t->poll_address, t->poll_count);
  if (!ready &&
      (t->poll_infinite || (int32_t)(clock_ticks() - t->poll_deadline) < 0))
    return;
  t->frame->eax = (int64_t)ready;
  t->state = TASK_READY;
  t->ready_since = clock_ticks();
}
static int poll_start(struct process *t, uint32_t address, unsigned count,
                      int timeout) {
  if (count > 256 || timeout > 86400000)
    return -22;
  int ready = poll_descriptors(t, address, count);
  if (ready || !timeout)
    return ready;
  t->poll_address = address;
  t->poll_count = count;
  t->poll_infinite = timeout < 0;
  t->poll_deadline = clock_ticks() + (timeout + 9) / 10;
  t->state = TASK_POLL;
  return 0;
}
static int socket_create(struct process *t, unsigned family, unsigned type,
                         unsigned protocol) {
  if (family != 2)
    return -97;
  if ((type & ~0x80800u) != 1 || (protocol && protocol != 6))
    return -93;
  int fd = new_fd(t, 0);
  if (fd < 0)
    return fd;
  unsigned i = 0;
  while (i < HANDLES && handles[i].refs)
    i++;
  if (i == HANDLES)
    return -23;
  struct handle *h = &handles[i];
  memset(h, 0, sizeof(*h));
  h->refs = 1;
  h->kind = H_SOCKET;
  h->tcp_id = net_tcp_socket();
  if (h->tcp_id < 0) { int result=h->tcp_id; memset(h,0,sizeof(*h)); return result; }
  h->flags = 2 | ((type & 0x800) ? 2048 : 0);
  t->fds[fd] = i;
  if (type & 0x80000)
    t->cloexec |= 1u << fd;
  return fd;
}
static int socket_connect(struct process *t, unsigned fd, uint32_t address, unsigned length) {
  struct handle *h=get_handle(t,fd);if(!h || h->kind!=H_SOCKET)return -88;
  uint8_t a[16];if(length<16 || !process_copyin(t,a,address,16))return -14;
  if(a[0]!=2 || a[1])return -97;
  if(h->connected)return -106;
  if(h->listening)return -22;
  h->remote=(uint32_t)a[4]<<24|(uint32_t)a[5]<<16|(uint32_t)a[6]<<8|a[7];h->port=(unsigned)a[2]<<8|a[3];
  struct network_info info;net_snapshot(&info);if(!info.ip && net_dhcp()<0)return -101;
  int result=net_tcp_socket_connect(h->tcp_id,h->remote,h->port,(h->flags&2048)!=0);
  if(result<0 && result!=-115) {h->socket_error=-result;return result;}
  h->connected=true;return result;
}
static int socket_bind(struct process *t,unsigned fd,uint32_t address,unsigned length) {
  struct handle *h=get_handle(t,fd);if(!h || h->kind!=H_SOCKET)return -88;
  uint8_t a[16];if(length<16 || !process_copyin(t,a,address,16))return -14;
  if(a[0]!=2 || a[1])return -97;
  uint32_t ip=(uint32_t)a[4]<<24|(uint32_t)a[5]<<16|(uint32_t)a[6]<<8|a[7];unsigned port=(unsigned)a[2]<<8|a[3];
  int result=net_tcp_bind(h->tcp_id,ip,port);
  if(!result) {h->bind_address=ip;struct tcp_snapshot snapshot;net_tcp_socket_snapshot(h->tcp_id,&snapshot);h->bind_port=snapshot.local_port;}
  return result;
}
static int socket_listen(struct process *t,unsigned fd,unsigned backlog) {
  struct handle *h=get_handle(t,fd);if(!h || h->kind!=H_SOCKET)return -88;
  int result=net_tcp_listen(h->tcp_id,backlog);if(!result)h->listening=true;return result;
}
static int socket_accept(struct process *t,unsigned fd,uint32_t address,uint32_t length,unsigned flags) {
  struct handle *listener=get_handle(t,fd);if(!listener || listener->kind!=H_SOCKET || !listener->listening)return -22;
  if(flags&~0x80800u)return -22;
  uint32_t capacity=0;
  if(address && (!length || !process_copyin(t,&capacity,length,4) || !process_range(t,address,capacity<16?capacity:16,true) || !process_range(t,length,4,true)))return -14;
  int descriptor=new_fd(t,0);if(descriptor<0)return descriptor;
  unsigned index=0;while(index<HANDLES && handles[index].refs)index++;
  if(index==HANDLES)return -23;
  uint32_t start=clock_ticks();int id;
  do {
    net_poll();id=net_tcp_accept(listener->tcp_id);
    if(id!=-11 || (listener->flags&2048))break;
    cpu_pause();
  } while((uint32_t)(clock_ticks()-start)<500);
  if(id<0)return id;
  struct handle *h=&handles[index];memset(h,0,sizeof(*h));h->refs=1;h->kind=H_SOCKET;h->tcp_id=id;
  h->flags=2|((flags&0x800)?2048:0);h->connected=true;h->reuse=listener->reuse;
  struct tcp_snapshot snapshot;net_tcp_socket_snapshot(id,&snapshot);h->remote=snapshot.remote;h->port=snapshot.remote_port;h->bind_port=snapshot.local_port;
  if(address) {
    uint8_t a[16]={2,0,h->port>>8,h->port,h->remote>>24,h->remote>>16,h->remote>>8,h->remote};
    unsigned size=capacity<16?capacity:16;
    uint32_t actual=16;
    if((size && !process_copyout(t,address,a,size)) || !process_copyout(t,length,&actual,4)) {net_tcp_socket_close(id);memset(h,0,sizeof(*h));return -14;}
  }
  t->fds[descriptor]=index;if(flags&0x80000)t->cloexec|=1u<<descriptor;
  return descriptor;
}
static int socket_address(struct process *t, unsigned fd, uint32_t out,
                          uint32_t size, bool peer) {
  struct handle *h = get_handle(t, fd);
  if (!h || h->kind != H_SOCKET)
    return -88;
  if (peer && !h->connected)
    return -107;
  uint32_t capacity;
  if (!process_copyin(t, &capacity, size, 4))
    return -14;
  struct tcp_snapshot snapshot;
  net_tcp_socket_snapshot(h->tcp_id, &snapshot);
  struct network_info network;
  net_snapshot(&network);
  uint32_t ip = peer ? h->remote : h->connected ? network.ip : h->bind_address;
  unsigned port = peer ? h->port : snapshot.local_port;
  uint8_t data[16] = {2, 0, port >> 8, port, ip >> 24, ip >> 16, ip >> 8, ip};
  uint32_t n = 16;
  if (capacity < n)
    n = capacity;
  if (!process_copyout(t, out, data, n))
    return -14;
  n = 16;
  return process_copyout(t, size, &n, 4) ? 0 : -14;
}
static int socket_shutdown(struct process *t,unsigned fd,unsigned how) {
  struct handle *h=get_handle(t,fd);if(!h || h->kind!=H_SOCKET)return -88;
  return net_tcp_shutdown(h->tcp_id,how);
}
static int socket_transfer(struct process *t,unsigned fd,uint32_t buffer,unsigned length,unsigned flags,bool write) {
  struct handle *h=get_handle(t,fd);if(!h || h->kind!=H_SOCKET)return -88;
  if(flags & ~(write?0xc040u:0x40u))return -95;
  unsigned previous=h->flags;if(flags&0x40)h->flags|=2048;
  int result=write?write_fd(t,fd,buffer,length):read_fd(t,fd,buffer,length);
  h->flags=previous;return result;
}
int linux_socket_service(struct process *t,unsigned operation,uint32_t a,uint32_t b,uint32_t c) {
  if(!sandbox_has(t,AV_CAP_NETWORK))return -13;
  switch(operation) {
  case 0:return socket_create(t,a,b,c);
  case 1:return socket_bind(t,a,b,c);
  case 2:return socket_listen(t,a,b);
  case 3:return socket_accept(t,a,b,c,0);
  case 4:return socket_connect(t,a,b,c);
  case 5:return socket_transfer(t,a,b,c,0,true);
  case 6:return socket_transfer(t,a,b,c,0,false);
  case 7:{struct handle *h=get_handle(t,a);return !h || h->kind!=H_SOCKET?-88:close_fd(t,a);}
  case 8:{
    struct handle *h=get_handle(t,a);struct av_socket_option option;
    if(!h || h->kind!=H_SOCKET)return -88;
    if(c<sizeof(option) || !process_copyin(t,&option,b,sizeof(option)))return -14;
    if(option.level==1 && option.name==2) {h->reuse=option.value!=0;net_tcp_reuse(h->tcp_id,h->reuse);return 0;}
    if(option.level==6 && option.name==1 && option.value==1)return 0;
    return -92;
  }
  case 9:case 10:return socket_address(t,a,b,c,operation==10);
  case 11:return socket_shutdown(t,a,b);
  default:return -22;
  }
}
void linux_syscall64(struct trap_frame *f) {
  struct process *t = process_current();
  if (!sandbox_linux(t, f->eax, true)) { f->eax = (uint64_t)-13; return; }
  uint64_t syscall_number = f->eax;
  if (f->eax >= 4096 && f->eax <= 4099) {
    f->eax = f->edi > UINT32_MAX ? -14 : client_call(t, f->eax - 4096, f->edi);
    return;
  }
  uint64_t a = f->edi, b = f->esi, c = f->edx, d = f->r10;
  int64_t result = -38;
  /* All user mappings currently fit in the lower 4 GiB. Reject high pointers
     rather than truncating them into a different, valid user allocation. */
  unsigned alias = 0;
  switch (f->eax) {
  case AV_LINUX_DOWNLOAD:
    result = a > UINT32_MAX ? -14 : gui_download_call(t, a);
    break;
  case 200:
    result = user_kill((int)a, (int)b);
    break;
  case 234:
    result = (int)a == t->pid && (int)b == t->pid ? user_kill(t->pid, c) : -3;
    break;
  case 13: {
    if (!a || a > 64 || a == 9 || a == 19 || d != 8) {
      result = -22;
      break;
    }
    uint64_t bit = UINT64_C(1) << (a - 1), action[4] = {0};
    if (b &&
        (b > UINT32_MAX || !process_copyin(t, action, b, sizeof(action)))) {
      result = -14;
      break;
    }
    if (b && action[0] > 1) {
      result = -38;
      break;
    }
    if (c) {
      uint64_t old[4] = {(t->ignored_signals & bit) ? 1 : 0, 0, 0, 0};
      if (c > UINT32_MAX || !process_copyout(t, c, old, sizeof(old))) {
        result = -14;
        break;
      }
    }
    if (b) {
      if (action[0])
        t->ignored_signals |= bit;
      else
        t->ignored_signals &= ~bit;
    }
    result = 0;
    break;
  }
  case 14: {
    if (d != 8 || a > 2) {
      result = -22;
      break;
    }
    uint64_t mask;
    if (b && (b > UINT32_MAX || !process_copyin(t, &mask, b, 8))) {
      result = -14;
      break;
    }
    if (c &&
        (c > UINT32_MAX || !process_copyout(t, c, &t->blocked_signals, 8))) {
      result = -14;
      break;
    }
    if (b) {
      if (a == 0)
        t->blocked_signals |= mask;
      else if (a == 1)
        t->blocked_signals &= ~mask;
      else
        t->blocked_signals = mask;
      t->blocked_signals &= ~((UINT64_C(1) << 8) | (UINT64_C(1) << 18));
      process_pending_signals(t);
    }
    result = 0;
    break;
  }
  case 202: {
    if (a > UINT32_MAX || (a & 3)) {
      result = -22;
      break;
    }
    uint32_t value;
    if (!process_copyin(t, &value, a, 4)) {
      result = -14;
      break;
    }
    unsigned operation = b & 127;
    bool private = b & 128;
    uint32_t mask = (operation == 9 || operation == 10) ? f->r9 : UINT32_MAX;
    if (b & ~UINT64_C(511) || !mask) {
      result = -22;
      break;
    }
    uint64_t *pte = process_pte(t, a);
    uint64_t key = private ? ((uint64_t)(unsigned)t->pid << 32) | a
                           : (*pte & PROCESS_PHYSICAL_MASK) + (a & 4095);
    if (operation == 1 || operation == 10) {
      result = c > INT32_MAX ? -22 : process_futex_wake(key, private, c, mask);
      break;
    }
    if (operation != 0 && operation != 9) {
      result = -38;
      break;
    }
    if (value != (uint32_t)c) {
      result = -11;
      break;
    }
    t->futex_timed = false;
    if (d) {
      uint64_t ts[2];
      if (d > UINT32_MAX || !process_copyin(t, ts, d, 16)) {
        result = -14;
        break;
      }
      if (ts[1] >= 1000000000 || ts[0] > UINT64_MAX / 100) {
        result = -22;
        break;
      }
      uint64_t ticks = ts[0] * 100 + (ts[1] + 9999999) / 10000000;
      if (operation == 9) {
        uint64_t now = clock_ticks();
        if (b & 256) {
          uint32_t days, seconds;
          if (!rtc_time(&days, &seconds) || days < 719528) {
            result = -5;
            break;
          }
          now = ((uint64_t)(days - 719528) * 86400 + seconds) * 100;
        }
        if (ticks <= now) {
          result = -110;
          break;
        }
        ticks -= now;
      }
      if (ticks > 8640000) {
        result = -22;
        break;
      }
      t->wake = clock_ticks() + ticks;
      t->futex_timed = true;
    }
    t->futex_key = key;
    t->futex_mask = mask;
    t->futex_private = private;
    t->state = TASK_FUTEX;
    result = 0;
    break;
  }
  case 267:
    if ((int32_t)a != -100)
      result = -9;
    else if (b > UINT32_MAX || c > UINT32_MAX || d > UINT32_MAX)
      result = -14;
    else {
      struct trap_frame compat = *f;
      compat.eax = 85;
      compat.ebx = b;
      compat.ecx = c;
      compat.edx = d;
      linux_syscall(&compat);
      result = (int64_t)compat.eax;
    }
    break;
  case 7:
    result = a > UINT32_MAX ? -14 : poll_start(t, a, b, (int)c);
    break;
  case 43:
  case 288:
    result = b > UINT32_MAX || c > UINT32_MAX ? -14 : socket_accept(t,a,b,c,f->eax == 288 ? d : 0);
    break;
  case 48:
    result=socket_shutdown(t,a,b);
    break;
  case 49:
    result = b > UINT32_MAX || c > UINT32_MAX ? -14 : socket_bind(t,a,b,c);
    break;
  case 50:
    result = socket_listen(t,a,(int)b < 0 ? 16 : b);
    break;
  case 41:
    result = socket_create(t, a, b, c);
    break;
  case 42:
    result = b > UINT32_MAX ? -14 : socket_connect(t, a, b, c);
    break;
  case 44:
    if (d & ~UINT64_C(0xc040))
      result = -95;
    else if (f->r8 || f->r9)
      result = -106;
    else
      result = b > UINT32_MAX || c > INT32_MAX ? -14 : socket_transfer(t,a,b,c,d,true);
    break;
  case 45:
    if (d & ~UINT64_C(0x40))
      result = -95;
    else if (b > UINT32_MAX || c > INT32_MAX || f->r8 > UINT32_MAX ||
             f->r9 > UINT32_MAX)
      result = -14;
    else {
      result = f->r8 ? socket_address(t, a, f->r8, f->r9, true) : 0;
      if (!result)
        result = socket_transfer(t,a,b,c,d,false);
    }
    break;
  case 51:
  case 52:
    result = b > UINT32_MAX || c > UINT32_MAX
                 ? -14
                 : socket_address(t, a, b, c, f->eax == 52);
    break;
  case 54: {
    struct handle *h = get_handle(t, a);
    uint32_t value;
    if (!h || h->kind != H_SOCKET)
      result = -88;
    else if (d > UINT32_MAX || f->r8 < 4 || !process_copyin(t, &value, d, 4))
      result = -14;
    else if (b == 1 && c == 2) { h->reuse=value!=0;net_tcp_reuse(h->tcp_id,h->reuse);result=0; }
    else if (b == 6 && c == 1 && value == 1)
      result = 0; /* TCP transmits immediately, without Nagle aggregation. */
    else
      result = -92;
    break;
  }
  case 55: {
    struct handle *h = get_handle(t, a);
    uint32_t cap, value = 0;
    if (!h || h->kind != H_SOCKET) {
      result = -88;
      break;
    }
    if (b != 1) {
      result = -92;
      break;
    }
    struct tcp_snapshot state;
    net_tcp_socket_snapshot(h->tcp_id,&state);
    if (c == 4)
      value = h->socket_error ? h->socket_error : net_tcp_socket_error(h->tcp_id,false);
    else if (c == 3)
      value = 1;
    else if (c == 8)
      value = 131072;
    else if(c == 7) value = 65536;
    else if(c == 2) value = h->reuse;
    else if(c == 30) value = h->listening;
    else {
      result = -92;
      break;
    }
    if (d > UINT32_MAX || f->r8 > UINT32_MAX ||
        !process_copyin(t, &cap, f->r8, 4) || cap < 4 ||
        !process_copyout(t, d, &value, 4))
      result = -14;
    else {
      cap = 4;
      result = process_copyout(t, f->r8, &cap, 4) ? 0 : -14;
      if (!result && c == 4) {
        h->socket_error = 0; net_tcp_socket_error(h->tcp_id,true);
      }
    }
    break;
  }
  case 4100: {
    char hostname[256];
    uint32_t ip;
    if (a > UINT32_MAX || b > UINT32_MAX ||
        !process_string(t, a, hostname, sizeof(hostname)))
      result = -14;
    else {
      struct network_info info;
      net_snapshot(&info);
      result = !info.ip ? net_dhcp() : 0;
      if (!result)
        result = net_dns(hostname, &ip);
      if (!result && !process_copyout(t, b, &ip, 4))
        result = -14;
    }
    break;
  }
  case 0:
    alias = 3;
    break;
  case 1:
    alias = 4;
    break;
  case 2:
    alias = 5;
    break;
  case 3:
    alias = 6;
    break;
  case 4:
  case 6:
    result = a > UINT32_MAX || b > UINT32_MAX ? -14 : stat64_path(t, a, b);
    break;
  case 5:
    result = b > UINT32_MAX ? -14 : stat64_fd(t, a, b);
    break;
  case 8:
    result = (int64_t)b < INT32_MIN || (int64_t)b > INT32_MAX
                 ? -75
                 : seek_fd(t, a, b, c);
    break;
  case 9:
    result = a > UINT32_MAX || b > UINT32_MAX || c > 7 || d > UINT32_MAX
                 ? -22
                 : memory_map(t, a, b, c, d);
    break;
  case 10:
    alias = 125;
    break;
  case 11:
    alias = 91;
    break;
  case 12:
    alias = 45;
    break;
  case 16:
    alias = 54;
    break;
  case 19:
  case 20:
    result = b > UINT32_MAX || c > 1024
                 ? -14
                 : linux64_vector(t, a, b, c, f->eax == 20);
    break;
  case 21: {
    char name[256];
    struct file_metadata meta;
    if (a > UINT32_MAX || !filename(t, a, name))
      result = -14;
    else if (b & ~7u)
      result = -22;
    else {
      result = fs_metadata(name, &meta);
      if (!result && (b & 1) && !(meta.mode & 0111))
        result = -13;
    }
    break;
  }
  case 24:
    alias = 158;
    break;
  case 32:
    alias = 41;
    break;
  case 33:
    alias = 63;
    break;
  case 35: {
    uint64_t ts[2];
    if (a > UINT32_MAX || b > UINT32_MAX || !process_copyin(t, ts, a, 16))
      result = -14;
    else if (ts[0] > 3600 || ts[1] >= 1000000000)
      result = -22;
    else if (b && !process_range(t, b, 16, true))
      result = -14;
    else {
      if (b) {
        uint64_t zero[2] = {0};
        process_copyout(t, b, zero, 16);
      }
      t->wake = clock_ticks() + ts[0] * 100 + (ts[1] + 9999999) / 10000000;
      t->state = TASK_SLEEP;
      result = 0;
    }
    break;
  }
  case 39:
  case 186:
    result = t->pid;
    break;
  case 56:
    if ((a & ~UINT64_C(0x12000ff)) || (a & 255) != 17 || b)
      result = -38;
    else if ((a & 0x1200000) &&
             (d > UINT32_MAX || !process_range(t, d, 4, true)))
      result = -14;
    else
      result = process_fork(f);
    break;
  case 57:
    result = process_fork(f);
    break;
  case 59:
    result = a > UINT32_MAX || b > UINT32_MAX || c > UINT32_MAX
                 ? -14
                 : process_execve(a, b, c);
    break;
  case 60:
  case 231:
    process_exit(t, a & 255, 0);
    result = 0;
    break;
  case 61:
    if ((int64_t)a != -1 && ((int64_t)a <= 0 || a > INT32_MAX))
      result = -22;
    else if (b > UINT32_MAX || (b && !process_range(t, b, 4, true)))
      result = -14;
    else if (c & ~UINT64_C(1))
      result = -22;
    else if (d)
      result = -38;
    else
      result = process_wait(t, (int)a, b, c & 1);
    break;
  case 62:
    alias = 37;
    break;
  case 63: {
    char uts[6][65] = {{0}};
    const char *v[] = {"Linux",           "magenta", "6.0.0-avalanche",
                       "Avalanche 0.4.0", "x86_64",  "localdomain"};
    for (unsigned i = 0; i < 6; i++)
      memcpy(uts[i], v[i], strlen(v[i]));
    result = a > UINT32_MAX                            ? -14
             : process_copyout(t, a, uts, sizeof(uts)) ? 0
                                                       : -14;
    break;
  }
  case 72:
    alias = 55;
    break;
  case 74:
  case 75:
    if (a >= PROCESS_FDS || t->fds[a] == FD_UNUSED)
      result = -9;
    else {
      int disk = fs_mounted_disk();
      struct handle *h = get_handle(t, a);
      unsigned secondary;
      if (h && (h->kind == H_FILE || h->kind == H_DIR) &&
          fs_secondary_path(h->name, &secondary, NULL)) {
        int state = fs_secondary_state(secondary);
        result = state < 1 ? -19 : state == 1 || disk_flush(secondary) ? 0 : -5;
      } else
        result = disk < 0 || disk_flush(disk) ? 0 : -5;
    }
    break;
  case 79:
    if (a > UINT32_MAX || b > UINT32_MAX)
      result = -14;
    else {
      const char root[] = "/";
      result = b < 2 ? -34 : process_copyout(t, a, root, 2) ? 2 : -14;
    }
    break;
  case 82:
  case 83:
  case 84: {
    char one[256], two[256];
    if (a > UINT32_MAX || !filename(t, a, one))
      result = -14;
    else if (f->eax == 82)
      result =
          b > UINT32_MAX || !filename(t, b, two) ? -14 : fs_rename(one, two);
    else if (f->eax == 83)
      result = fs_mkdir(one);
    else
      result = fs_directory(one) ? -20 : fs_remove(one);
    break;
  }
  case 87:
    alias = 10;
    break;
  case 89:
    alias = 85;
    break;
  case 96:
    if (b)
      result = -22;
    else
      result = !a ? 0 : a > UINT32_MAX ? -14 : clock64(t, 0, a, true);
    break;
  case 97:
    result = b > UINT32_MAX ? -14 : limit64(t, a, b);
    break;
  case 102:
  case 104:
  case 107:
  case 108:
    result = 0;
    break;
  case 110:
    result = t->parent;
    break;
  case 158:
    if (a == 0x1002) {
      if (b > UINT32_MAX || (b && !process_address(b, 1)))
        result = -1;
      else {
        t->fs_base = b;
        platform_user_fs(b);
        result = 0;
      }
    } else if (a == 0x1003)
      result = b > UINT32_MAX                          ? -14
               : process_copyout(t, b, &t->fs_base, 8) ? 0
                                                       : -14;
    else
      result = -22;
    break;
  case 201: {
    uint32_t days, seconds;
    if (!rtc_time(&days, &seconds) || days < 719528)
      result = -5;
    else {
      int64_t v = (uint64_t)(days - 719528) * 86400 + seconds;
      result = a && (a > UINT32_MAX || !process_copyout(t, a, &v, 8)) ? -14 : v;
    }
    break;
  }
  case 217:
    alias = 220;
    break;
  case 218:
    alias = 258;
    break;
  case 228:
    result = b > UINT32_MAX ? -14 : clock64(t, a, b, false);
    break;
  case 230: {
    uint64_t ts[2];
    if (c > UINT32_MAX || d > UINT32_MAX || !process_copyin(t, ts, c, 16))
      result = -14;
    else if ((a != 0 && a != 1) || b > 1 || ts[1] >= 1000000000)
      result = -22;
    else if (d && !process_range(t, d, 16, true))
      result = -14;
    else {
      uint64_t ticks = ts[0] * 100 + (ts[1] + 9999999) / 10000000;
      if (b) {
        uint64_t now = clock_ticks();
        if (!a) {
          uint32_t days, seconds;
          if (!rtc_time(&days, &seconds) || days < 719528) {
            result = -5;
            break;
          }
          now = ((uint64_t)(days - 719528) * 86400 + seconds) * 100;
        }
        ticks = ticks > now ? ticks - now : 0;
      }
      if (ticks > 360000)
        result = -22;
      else {
        if (d) {
          uint64_t zero[2] = {0};
          process_copyout(t, d, zero, 16);
        }
        t->wake = clock_ticks() + ticks;
        t->state = TASK_SLEEP;
        result = 0;
      }
    }
    break;
  }
  case 257:
    alias = 295;
    break;
  case 262: {
    if (b > UINT32_MAX || c > UINT32_MAX) {
      result = -14;
      break;
    }
    if (d & ~UINT64_C(0x1100)) {
      result = -22;
      break;
    }
    char path[256];
    if (!process_string(t, b, path, sizeof(path)))
      result = -14;
    else if (!*path && (d & 0x1000))
      result = stat64_fd(t, a, c);
    else if ((int64_t)a != -100 && path[0] != '/')
      result = -9;
    else
      result = stat64_path(t, b, c);
    break;
  }
  case 302:
    if (a && a != (unsigned)t->pid)
      result = -3;
    else if (c)
      result = -1;
    else
      result = d > UINT32_MAX ? -14 : !d ? 0 : limit64(t, b, d);
    break;
  case 318: {
    if (a > UINT32_MAX || b > INT32_MAX || !process_range(t, a, b, true))
      result = -14;
    else if (c & ~UINT64_C(3))
      result = -22;
    else {
      uint8_t bytes[256];
      unsigned done = 0;
      while (done < b) {
        unsigned n = b - done;
        if (n > sizeof(bytes))
          n = sizeof(bytes);
        if (!secure_random(bytes, n))
          break;
        process_copyout(t, a + done, bytes, n);
        done += n;
      }
      memset(bytes, 0, sizeof(bytes));
      result = done || !b ? (int64_t)done : -11;
    }
    break;
  }
  default:
    break;
  }
  if (alias) {
    bool valid = true;
    uint64_t args[] = {a, b, c};
    for (unsigned i = 0; i < 3; i++)
      if (args[i] > UINT32_MAX && args[i] < (UINT64_MAX - UINT32_MAX))
        valid = false;
    /* Signed descriptors and process IDs may be sign-extended. Pointers may
       not: a high address must never alias a mapped lower address. */
    if ((alias == 3 || alias == 4 || alias == 85 || alias == 220 ||
         alias == 295) &&
        b > UINT32_MAX)
      valid = false;
    if ((alias == 5 || alias == 10 || alias == 45 || alias == 91 ||
         alias == 125 || alias == 258) &&
        a > UINT32_MAX)
      valid = false;
    if (alias == 54 && c > UINT32_MAX)
      valid = false;
    if (!valid)
      result = -14;
    else {
      struct trap_frame compat = *f;
      compat.eax = alias;
      compat.ebx = (uint32_t)a;
      compat.ecx = (uint32_t)b;
      compat.edx = (uint32_t)c;
      linux_syscall(&compat);
      result = (int64_t)compat.eax;
    }
  }
  f->eax = result;
  if (trace_enabled && result < 0 && trace_count++ < 256) {
    console_write("Linux64 syscall error ");
    print_uint(syscall_number);
    console_putc(' ');
    print_uint(-result);
    console_putc('\n');
  }
}
