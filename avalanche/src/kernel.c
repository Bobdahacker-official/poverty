#include "ext4.h"
#include "package.h"
#include "system.h"
#define PAGES PHYSICAL_PAGES
#define PAGE 4096u
static inline void outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
void *memset(void *d, int c, size_t n) {
  uint8_t *p = d;
  while (n--)
    *p++ = (uint8_t)c;
  return d;
}
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *a = d;
  const uint8_t *b = s;
  while (n--)
    *a++ = *b++;
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  uint8_t *a = d;
  const uint8_t *b = s;
  if (a < b)
    return memcpy(d, s, n);
  while (n) {
    --n;
    a[n] = b[n];
  }
  return d;
}
size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
static bool eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
struct tag {
  uint32_t type, size;
};
struct mmap_entry {
  uint64_t base, len;
  uint32_t type, reserved;
} __attribute__((packed));
struct fbtag {
  uint32_t type, size;
  uint64_t addr;
  uint32_t pitch, width, height;
  uint8_t bpp, kind;
  uint16_t reserved;
  uint8_t rp, rs, gp, gs, bp, bs;
} __attribute__((packed));
static uint8_t usable[PAGES / 8], allocated[PAGES / 8];
static uint32_t available, used;
extern char kernel_end;
static void mark(uint8_t *b, uint32_t i, bool on) {
  if (on)
    b[i / 8] |= 1u << (i % 8);
  else
    b[i / 8] &= ~(1u << (i % 8));
}
static bool bit(const uint8_t *b, uint32_t i) {
  return b[i / 8] & (1u << (i % 8));
}
static void reserve(uint64_t start, uint64_t end) {
  uint32_t a = start / PAGE, b = end / PAGE + (end % PAGE != 0);
  if (b > PAGES)
    b = PAGES;
  for (uint32_t i = a; i < b; ++i)
    if (bit(usable, i)) {
      mark(usable, i, false);
      --available;
    }
}
void *pages_alloc(unsigned count) {
  if (!count || count > PAGES) return NULL;
  unsigned run = 0;
  for (uint32_t i = 0; i < PAGES; ++i) {
    if (bit(usable, i) && !bit(allocated, i)) run++; else run = 0;
    if (run == count) {
      unsigned first = i + 1 - count;
      for (unsigned j = first; j <= i; j++) mark(allocated, j, true);
      used += count;
      void *p = (void *)((uintptr_t)first * PAGE);
      memset(p, 0, (size_t)count * PAGE);
      return p;
    }
  }
  return NULL;
}
void *page_alloc(void) { return pages_alloc(1); }
bool page_free(void *p) {
  uintptr_t a = (uintptr_t)p, i = a / PAGE;
  if (a % PAGE || i >= PAGES || !bit(allocated, i)) return false;
  mark(allocated, i, false);
  --used;
  return true;
}
static volatile uint8_t *fb;
static uint32_t pitch, fw, fh, fg, bg;
static uint8_t bytespp, rpos, gpos, bpos, rbits, gbits, bbits;
static uint32_t col, row, cols = 80, rows = 25;
static bool serial;
static void serial_sequence(const char *s) {
  if (!serial)
    return;
  while (*s) {
    for (unsigned i = 0; i < 100000 && !(inb(0x3fd) & 0x20); i++) {
    }
    outb(0x3f8, (uint8_t)*s++);
  }
}
/* Original 5x7 bitmap alphabet. Lowercase is displayed as uppercase. */
static const uint8_t glyphs[36][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 18, 18, 12},
    {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
    {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((uintptr_t)(r >> (8 - rbits)) << rpos) |
         ((uintptr_t)(g >> (8 - gbits)) << gpos) |
         ((uintptr_t)(b >> (8 - bbits)) << bpos);
}
static void pixel(uint32_t x, uint32_t y, uint32_t c) {
  volatile uint8_t *p = fb + y * pitch + x * bytespp;
  for (uint8_t i = 0; i < bytespp; ++i)
    p[i] = (uint8_t)(c >> (8 * i));
}
static uint8_t glyph(char c, unsigned y) {
  if (c >= 'a' && c <= 'z')
    c -= 32;
  if (c >= 'A' && c <= 'Z')
    return glyphs[c - 'A'][y];
  if (c >= '0' && c <= '9')
    return glyphs[c - '0' + 26][y];
  switch (c) {
  case '>':
    return y == 1 ? 16 : y == 2 ? 8 : y == 3 ? 4 : y == 4 ? 8 : y == 5 ? 16 : 0;
  case '-':
    return y == 3 ? 14 : 0;
  case '_':
    return y == 6 ? 31 : 0;
  case '.':
    return y == 6 ? 4 : 0;
  case ':':
    return y == 2 || y == 5 ? 4 : 0;
  case '/':
    return y < 5 ? 1u << y : 16;
  case '?':
    return y == 0 ? 14 : y == 1 ? 17 : y == 2 ? 2 : y == 3 ? 4 : y == 5 ? 4 : 0;
  case '=':
    return y == 2 || y == 4 ? 31 : 0;
  case '!':
    return y < 4 || y == 6 ? 4 : 0;
  case '[':
    return y == 0 || y == 6 ? 14 : 8;
  case ']':
    return y == 0 || y == 6 ? 14 : 2;
  case '(':
    return y == 0 || y == 6 ? 2 : 4;
  case ')':
    return y == 0 || y == 6 ? 8 : 4;
  case '+':
    return y == 3 ? 14 : y == 2 || y == 4 ? 4 : 0;
  case ',':
    return y == 5 ? 4 : y == 6 ? 8 : 0;
  case '\'':
    return y < 2 ? 4 : 0;
  default:
    return 0;
  }
}
static void cell(uint32_t x, uint32_t y, char c) {
  if (!fb)
    return;
  for (unsigned py = 0; py < 16; ++py)
    for (unsigned px = 0; px < 12; ++px) {
      uint8_t bits = py / 2 < 7 ? glyph(c, py / 2) : 0;
      pixel(x * 12 + px, y * 16 + py,
            px / 2 < 5 && (bits & (1u << (4 - px / 2))) ? fg : bg);
    }
}
void console_clear(void) {
  serial_sequence("\033[2J\033[H");
  if (fb)
    for (uint32_t y = 0; y < fh; ++y)
      for (uint32_t x = 0; x < fw; ++x)
        pixel(x, y, bg);
  col = row = 0;
}
static void newline(void) {
  col = 0;
  ++row;
  if (row >= rows) {
    if (fb) {
      memmove((void *)(uintptr_t)fb, (const void *)(fb + 16 * pitch),
              (rows - 1) * 16 * pitch);
      for (uint32_t y = (rows - 1) * 16; y < rows * 16; ++y)
        for (uint32_t x = 0; x < fw; ++x)
          pixel(x, y, bg);
    }
    row = rows - 1;
  }
}
static char *capture_buffer;
static unsigned capture_size, capture_used;
static void (*console_observer)(char);
static int gui_key;
void graphics_console_capture(void (*observer)(char)) {
  console_observer = observer;
}
int graphics_key_event(void) {
  int key = gui_key;
  gui_key = 0;
  return key;
}
void console_putc(char c) {
  if (capture_buffer) {
    if (capture_used + 1 < capture_size) {
      capture_buffer[capture_used++] = c;
      capture_buffer[capture_used] = 0;
    }
    return;
  }
  if (serial) {
    for (unsigned i = 0; i < 100000 && !(inb(0x3fd) & 0x20); ++i) {
    }
    if (c == '\n')
      outb(0x3f8, '\r');
    outb(0x3f8, (uint8_t)c);
  }
  if (console_observer) {
    console_observer(c);
    return;
  }
  if (c == '\n') {
    newline();
    return;
  }
  if (c == '\r') {
    col = 0;
    return;
  }
  if (c == '\b') {
    if (col)
      --col;
    else if (row) {
      --row;
      col = cols - 1;
    }
    cell(col, row, ' ');
    return;
  }
  if (c < ' ')
    return;
  cell(col, row, c);
  if (++col >= cols)
    newline();
}
void console_write(const char *s) {
  while (*s)
    console_putc(*s++);
}
void print_uint(uint32_t n) {
  char b[11];
  unsigned i = 0;
  do {
    b[i++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (i)
    console_putc(b[--i]);
}
void print_hex(uint32_t n) {
  console_write("0x");
  for (int i = 7; i >= 0; --i)
    console_putc("0123456789ABCDEF"[(n >> (i * 4)) & 15]);
}
static const char keys[128] = {
    [1] = 27,   [2] = '1',   [3] = '2',   [4] = '3',   [5] = '4',   [6] = '5',
    [7] = '6',  [8] = '7',   [9] = '8',   [10] = '9',  [11] = '0',  [12] = '-',
    [13] = '=', [14] = '\b', [15] = '\t', [16] = 'q',  [17] = 'w',  [18] = 'e',
    [19] = 'r', [20] = 't',  [21] = 'y',  [22] = 'u',  [23] = 'i',  [24] = 'o',
    [25] = 'p', [26] = '[',  [27] = ']',  [28] = '\n', [30] = 'a',  [31] = 's',
    [32] = 'd', [33] = 'f',  [34] = 'g',  [35] = 'h',  [36] = 'j',  [37] = 'k',
    [38] = 'l', [39] = ';',  [40] = '\'', [41] = '`',  [43] = '\\', [44] = 'z',
    [45] = 'x', [46] = 'c',  [47] = 'v',  [48] = 'b',  [49] = 'n',  [50] = 'm',
    [51] = ',', [52] = '.',  [53] = '/',  [57] = ' '};
static uint8_t keyboard_queue[512];
static unsigned keyboard_head, keyboard_count;
void console_keyboard_poll(void) {
  if ((inb(0x64) & 0x21) != 1)
    return;
  uint8_t scancode = inb(0x60);
  if (keyboard_count < sizeof(keyboard_queue)) {
    keyboard_queue[(keyboard_head + keyboard_count) % sizeof(keyboard_queue)] = scancode;
    keyboard_count++;
  }
}
static bool keyboard_scancode(uint8_t *scancode) {
  uintptr_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
  bool present = false;
  if (keyboard_count) {
    *scancode = keyboard_queue[keyboard_head];
    keyboard_head = (keyboard_head + 1) % sizeof(keyboard_queue);
    keyboard_count--;
    present = true;
  } else if ((inb(0x64) & 0x21) == 1) {
    *scancode = inb(0x60);
    present = true;
  }
  if (flags & (1u << 9))
    __asm__ volatile("sti" ::: "memory");
  return present;
}
char console_getchar(void) {
  static bool left_shift, right_shift, left_ctrl, right_ctrl, caps, extended;
  for (;;) {
    if (serial && (inb(0x3fd) & 1)) {
      char c = inb(0x3f8);
      if (c == '\r')
        return '\n';
      if (c == 127)
        return '\b';
      return c;
    }
    uint8_t s;
    if (!keyboard_scancode(&s)) {
      devices_poll();
      return 0;
    }
    if (s == 0xe0) {
      extended = true;
      continue;
    }
    if (extended) {
      extended = false;
      if (console_observer && !(s & 128) &&
          (s == 0x4b || s == 0x4d || s == 0x48 || s == 0x50 || s == 0x47 ||
           s == 0x4f || s == 0x53))
        gui_key = s;
      if (s == 29 || s == 157)
        right_ctrl = s == 29;
      continue;
    }
    if (s == 29 || s == 157) {
      left_ctrl = s == 29;
      continue;
    }
    if (s == 42 || s == 170) {
      left_shift = s == 42;
      continue;
    }
    if (s == 54 || s == 182) {
      right_shift = s == 54;
      continue;
    }
    if (s == 58) {
      caps = !caps;
      continue;
    }
    if (s & 128)
      continue;
    char c = keys[s];
    bool shift = left_shift || right_shift;
    if (c >= 'a' && c <= 'z') {
      if (shift != caps)
        c -= 32;
    } else if (shift) {
      const char *a = "1234567890-=[];',./`\\", *b = "!@#$%^&*()_+{}:\"<>?~|";
      for (unsigned i = 0; a[i]; ++i)
        if (c == a[i]) {
          c = b[i];
          break;
        }
    }
    if (left_ctrl || right_ctrl) {
      if (c >= 'a' && c <= 'z')
        c -= 'a' - 1;
      else if (c >= 'A' && c <= 'Z')
        c -= 'A' - 1;
    }
    if (c)
      return c;
  }
}
void console_size(uint16_t *r, uint16_t *c) {
  *r = (uint16_t)rows;
  *c = (uint16_t)cols;
}
char console_input(void) {
  for (;;) {
    net_poll();
    user_poll();
    char c = user_tty_getchar();
    if (!c)
      c = console_getchar();
    if (c)
      return c;
    user_yield_kernel();
    cpu_pause();
  }
}
static char *token(char **p) {
  while (**p == ' ')
    ++*p;
  char *s = *p;
  while (**p && **p != ' ')
    ++*p;
  if (**p)
    *(*p)++ = 0;
  return s;
}
static void reboot(void) {
  for (unsigned i = 0; i < 100000; ++i)
    if (!(inb(0x64) & 2))
      break;
  outb(0x64, 0xfe);
}
void kernel_status(struct av_status *s) {
  *s = (struct av_status){clock_ticks(), available * 4,     used * 4,
                          disk_count(),  fs_mounted_disk(), fs_writable()};
}
void console_color(uint32_t color) {
  if (color == 1) {
    if (fb)
      fg = rgb(238, 124, 228);
    if (serial)
      serial_sequence("\033[95m");
  } else {
    if (fb)
      fg = rgb(205, 225, 247);
    if (serial)
      serial_sequence("\033[0m");
  }
}
void kernel_power(unsigned action) {
  if (action == AV_POWER_REBOOT) {
    reboot();
    return;
  }
  if (action == AV_POWER_HALT) {
    console_write("System halted. Reset or power off the machine.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
}
static uint8_t file_buffer[FS_MAX_FILE + 1];
static void result(int code) {
  if (code < 0) {
    console_write("Operation failed: ");
    print_uint((uint32_t)-code);
    console_putc('\n');
  } else
    console_write("Done.\n");
}
static void command(char *line) {
  char *rest = line, *cmd = token(&rest);
  if (!*cmd)
    return;
  if (eq(cmd, "priority")) {
    bool pid_ok, level_ok;
    unsigned pid = parse_uint(token(&rest), &pid_ok),
             level = parse_uint(token(&rest), &level_ok);
    result(pid_ok && level_ok ? user_priority(pid, level) : -22);
    return;
  }
  if (eq(cmd, "pkg")) {
    package_command(rest);
    return;
  }
  if (eq(cmd, "gui")) {
    if (capture_buffer)
      return;
    result(gui_run());
    console_clear();
    return;
  }
  if (eq(cmd, "ext4ls") || eq(cmd, "ext4cat") || eq(cmd, "ext4read") ||
      eq(cmd, "ext4write") || eq(cmd, "ext4create")) {
    bool ok;
    unsigned disk = parse_uint(token(&rest), &ok);
    char *path = token(&rest);
    struct ext4_volume volume;
    int r = ok ? ext4_mount(&volume, disk, 0) : -22;
    if (!r && eq(cmd, "ext4create")) {
      struct ext4_inode inode;
      r = ext4_create(&volume, path, &inode);
      if (r < 0)
        result(r);
      return;
    }
    if (!r && eq(cmd, "ext4ls"))
      r = ext4_list(&volume, *path ? path : "/");
    else if (!r) {
      struct ext4_inode inode;
      r = ext4_lookup(&volume, path, &inode);
      if (!r && (inode.mode & 0xf000) != 0x8000)
        r = -22;
      if (!r && eq(cmd, "ext4write")) {
        uint64_t position = 0;
        char *number = token(&rest);
        if (!*number)
          r = -22;
        while (*number && !r) {
          unsigned digit = (unsigned)(*number++ - '0');
          if (digit > 9 || position > (UINT64_MAX - digit) / 10)
            r = -22;
          else
            position = position * 10 + digit;
        }
        while (*rest == ' ')
          rest++;
        if (!r)
          r = ext4_write(&volume, &inode, position, rest, strlen(rest));
        if (r < 0)
          result(r);
        return;
      }
      uint64_t offset = 0, limit = inode.size;
      if (!r && eq(cmd, "ext4read")) {
        char *number = token(&rest);
        if (!*number)
          r = -22;
        while (*number && !r) {
          unsigned digit = (unsigned)(*number++ - '0');
          if (digit > 9 || offset > (UINT64_MAX - digit) / 10)
            r = -22;
          else
            offset = offset * 10 + digit;
        }
        uint32_t count = parse_uint(token(&rest), &ok);
        if (!ok || count > 4096 || offset > UINT64_MAX - count)
          r = -22;
        else if (offset + count < limit)
          limit = offset + count;
      }
      for (; !r && offset < limit;) {
        char data[512];
        uint32_t count =
            limit - offset > sizeof(data) ? sizeof(data) : limit - offset;
        int n = ext4_read(&volume, &inode, offset, data, count);
        if (n <= 0) {
          r = n ? n : -5;
          break;
        }
        for (int i = 0; i < n; i++)
          console_putc(data[i]);
        offset += n;
      }
    }
    if (r < 0)
      result(r);
    return;
  }
  if (eq(cmd, "help"))
    console_write(
        "help version clear echo mem ticks date beep mouse devices ps\n"
        "ls cat NAME write NAME TEXT rm NAME fs mount DISK\n"
        "ext4ls DISK /PATH  ext4cat DISK /PATH\n"
        "ext4read DISK /PATH OFFSET COUNT  - up to 4096 bytes\n"
        "ext4write DISK /PATH OFFSET TEXT\n"
        "format DISK ERASE  - DESTROYS the selected disk's contents\n"
        "install           - copy built-in ELF applications into filesystem\n"
        "run NAME [ARGS]   - foreground program; spawn NAME [ARGS], wait PID, "
        "kill PID\n"
        "Programs: hello, cat, udp, http, ush, worker, linuxhello, linuxfork\n"
        "net dhcp ip ADDRESS MASK GATEWAY DNS ping ADDRESS dns NAME\n"
        "udp ADDRESS PORT TEXT recv LOCALPORT http HOST /PATH reboot halt\n");
  else if (eq(cmd, "version"))
    console_write(
        "Avalanche Kernel 0.4.0 - x86-64 / BIOS + UEFI\nPaging-isolated "
        "ring 3, ELF32, syscalls, ATA/AFS, Ethernet/IPv4.\n");
  else if (eq(cmd, "clear"))
    console_clear();
  else if (eq(cmd, "echo")) {
    console_write(rest);
    console_putc('\n');
  } else if (eq(cmd, "mem")) {
    console_write("Managed RAM: ");
    print_uint(available * 4);
    console_write(" KiB; allocated: ");
    print_uint(used * 4);
    console_write(" KiB; free: ");
    print_uint((available - used) * 4);
    console_write(" KiB (below 128 MiB)\n");
  } else if (eq(cmd, "ticks")) {
    print_uint(clock_ticks());
    console_write(" ticks (100 Hz)\n");
  } else if (eq(cmd, "date"))
    rtc_print();
  else if (eq(cmd, "beep"))
    speaker_beep(880, 20);
  else if (eq(cmd, "mouse"))
    mouse_print();
  else if (eq(cmd, "devices"))
    devices_list();
  else if (eq(cmd, "fs"))
    fs_info();
  else if (eq(cmd, "ls"))
    fs_list();
  else if (eq(cmd, "cat")) {
    char *name = token(&rest);
    int n = fs_read(name, file_buffer, FS_MAX_FILE);
    if (n < 0)
      result(n);
    else {
      for (int i = 0; i < n; i++)
        console_putc((char)file_buffer[i]);
      console_putc('\n');
    }
  } else if (eq(cmd, "write")) {
    char *name = token(&rest);
    result(fs_write(name, rest, strlen(rest)));
  } else if (eq(cmd, "rm"))
    result(fs_remove(token(&rest)));
  else if (eq(cmd, "mount")) {
    bool ok;
    uint32_t d = parse_uint(token(&rest), &ok);
    if (!ok)
      console_write("Usage: mount DISK\n");
    else {
      result(fs_mount(d));
      fs_info();
    }
  } else if (eq(cmd, "format")) {
    bool ok;
    uint32_t d = parse_uint(token(&rest), &ok);
    char *confirm = token(&rest);
    if (!ok || !eq(confirm, "ERASE"))
      console_write("Usage: format DISK ERASE. This destroys that disk's "
                    "partition table and files.\n");
    else {
      result(fs_format(d));
      fs_info();
    }
  } else if (eq(cmd, "install"))
    result(user_install());
  else if (eq(cmd, "spawn")) {
    char *name = token(&rest);
    int pid = user_spawn(name, rest, false);
    if (pid < 0)
      result(pid);
    else {
      console_write("Started PID ");
      print_uint((uintptr_t)pid);
      console_putc('\n');
    }
  } else if (eq(cmd, "ps"))
    user_list();
  else if (eq(cmd, "wait")) {
    bool ok;
    uint32_t pid = parse_uint(token(&rest), &ok);
    if (ok)
      result(user_wait_pid((int)pid));
    else
      console_write("Usage: wait PID\n");
  } else if (eq(cmd, "kill")) {
    bool ok;
    uint32_t pid = parse_uint(token(&rest), &ok);
    if (ok)
      result(user_kill((int)pid, 9));
    else
      console_write("Usage: kill PID\n");
  } else if (eq(cmd, "run")) {
    char *name = token(&rest);
    int status = user_exec(name, rest);
    console_write("Process exit: ");
    if (status < 0) {
      console_putc('-');
      print_uint((uint32_t)-status);
    } else
      print_uint((uintptr_t)status);
    console_putc('\n');
  } else if (eq(cmd, "net"))
    net_info();
  else if (eq(cmd, "dhcp")) {
    console_write("Requesting DHCP lease...\n");
    result(net_dhcp());
    net_info();
  } else if (eq(cmd, "ip")) {
    uint32_t a, m, g, d;
    char *as = token(&rest), *ms = token(&rest), *gs = token(&rest),
         *ds = token(&rest);
    if (!ip_parse(as, &a) || !ip_parse(ms, &m) || !ip_parse(gs, &g) ||
        !ip_parse(ds, &d))
      console_write("Usage: ip ADDRESS MASK GATEWAY DNS\n");
    else {
      net_config(a, m, g, d);
      net_info();
    }
  } else if (eq(cmd, "ping")) {
    uint32_t a;
    if (!ip_parse(token(&rest), &a))
      console_write("Usage: ping IPv4_ADDRESS\n");
    else {
      int ms = net_ping(a);
      if (ms < 0)
        result(ms);
      else {
        console_write("Reply in ");
        print_uint((uintptr_t)ms);
        console_write(" ms\n");
      }
    }
  } else if (eq(cmd, "http")) {
    char *host = token(&rest), *path = token(&rest);
    net_http(host, path);
  } else if (eq(cmd, "dns")) {
    uint32_t a;
    int r = net_dns(token(&rest), &a);
    if (r < 0)
      result(r);
    else {
      ip_print(a);
      console_putc('\n');
    }
  } else if (eq(cmd, "udp")) {
    uint32_t a;
    bool ok;
    char *host = token(&rest), *port = token(&rest);
    uint32_t n = parse_uint(port, &ok);
    if (!ip_parse(host, &a) || !ok || !n || n > 65535)
      console_write("Usage: udp IP PORT TEXT\n");
    else
      result(net_udp_send(a, 49152, (uint16_t)n, rest, strlen(rest)));
  } else if (eq(cmd, "recv")) {
    bool ok;
    uint32_t port = parse_uint(token(&rest), &ok), source = 0;
    uint16_t remote = 0;
    if (!ok || !port || port > 65535)
      console_write("Usage: recv LOCALPORT\n");
    else {
      int n = net_udp_recv((uint16_t)port, file_buffer, 1472, &source, &remote);
      if (n < 0)
        console_write("No datagram available.\n");
      else {
        ip_print(source);
        console_putc(':');
        print_uint(remote);
        console_write(" ");
        for (int i = 0; i < n; i++)
          console_putc((char)file_buffer[i]);
        console_putc('\n');
      }
    }
  } else if (eq(cmd, "reboot")) {
    console_write("Rebooting...\n");
    reboot();
    console_write("Firmware did not respond. Use the reset control.\n");
  } else if (eq(cmd, "halt")) {
    console_write("CPU halted.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  } else
    console_write("Unknown command. Type help.\n");
}
void kernel_capture(char *line, char *out, unsigned size) {
  if (!size)
    return;
  out[0] = 0;
  if (str_eq(line, "gui"))
    return;
  capture_buffer = out;
  capture_size = size;
  capture_used = 0;
  command(line);
  capture_buffer = NULL;
}
bool graphics_size(unsigned *width, unsigned *height) {
  if (!fb || fw < 640 || fh < 480)
    return false;
  *width = fw;
  *height = fh;
  return true;
}
uint8_t graphics_glyph(char c, unsigned y) { return glyph(c, y); }
void graphics_present(const uint32_t *pixels, unsigned width, unsigned height) {
  graphics_present_region(pixels, width, height, 0, 0, width, height);
}
void graphics_present_region(const uint32_t *pixels, unsigned width,
                             unsigned height, unsigned x, unsigned y,
                             unsigned region_width, unsigned region_height) {
  if (!fb || width > fw || height > fh || x >= width || y >= height)
    return;
  if (region_width > width - x)
    region_width = width - x;
  if (region_height > height - y)
    region_height = height - y;
  if (bytespp == 4 && rpos == 16 && gpos == 8 && bpos == 0 && rbits == 8 &&
      gbits == 8 && bbits == 8) {
    for (unsigned row = y; row < y + region_height; row++) {
      void *destination = (void *)(uintptr_t)(fb + row * pitch + x * 4);
      const void *source = pixels + row * width + x;
      size_t count = region_width / 2;
      __asm__ volatile("rep movsq"
                       : "+D"(destination), "+S"(source),
                         "+c"(count)::"memory");
      if (region_width & 1)
        *(volatile uint32_t *)destination = *(const uint32_t *)source;
    }
    return;
  }
  for (unsigned row = y; row < y + region_height; row++)
    for (unsigned column = x; column < x + region_width; column++) {
      uint32_t color = pixels[row * width + column];
      pixel(column, row, rgb(color >> 16, color >> 8, color));
    }
}
void kernel_main(uint32_t magic, uint32_t mbi) {
  /* Initialize the protected-mode platform before enabling timer IRQs. */
  outb(0x21, 255);
  outb(0xa1, 255);
  outb(0x3f9, 0);
  outb(0x3fb, 0x80);
  outb(0x3f8, 1);
  outb(0x3f9, 0);
  outb(0x3fb, 3);
  outb(0x3fa, 0xc7);
  outb(0x3fc, 0x0b);
  serial = inb(0x3fd) != 0xff;
  platform_init();
  if (magic != 0x36d76289) {
    console_write("Avalanche: invalid Multiboot2 handoff.\n");
    for (;;)
      __asm__ volatile("hlt");
  }
  uint32_t total = *(uint32_t *)(uintptr_t)mbi;
  if (total < 16 || total > 0x100000 || mbi > 0xffffffffu - total) {
    console_write("Invalid boot information.\n");
    for (;;)
      __asm__ volatile("hlt");
  }
  uint32_t end = mbi + total;
  bool magenta_boot = false;
  for (struct tag *t = (void *)(uintptr_t)(mbi + 8); (uintptr_t)t <= end - 8;) {
    if (t->size < 8 || t->size > end - (uintptr_t)t)
      break;
    if (t->type == 0)
      break;
    if (t->type == 1 && t->size >= 18) {
      const char *text = (const char *)t + 8;
      uint32_t n = t->size - 8;
      for (uint32_t i = 0; i + 9 < n; i++) {
        if (i && text[i - 1] != ' ')
          continue;
        if (!__builtin_memcmp(text + i, "--magenta", 9) &&
            (text[i + 9] == 0 || text[i + 9] == ' '))
          magenta_boot = true;
      }
    }
    if (t->type == 6 && t->size >= 16) {
      uint32_t step = *(uint32_t *)(uintptr_t)((uint8_t *)(uintptr_t)t + 8);
      if (step >= 24)
        for (uint32_t off = 16; off <= t->size - 24;) {
          struct mmap_entry *e =
              (void *)(uintptr_t)((uint8_t *)(uintptr_t)t + off);
          if (e->type == 1 && e->base < (uint64_t)PAGES * PAGE) {
            uint64_t last = e->base + e->len;
            if (last < e->base || last > (uint64_t)PAGES * PAGE)
              last = (uint64_t)PAGES * PAGE;
            uint32_t a = (uintptr_t)((e->base + PAGE - 1) >> 12),
                     b = (uintptr_t)(last >> 12);
            for (uint32_t i = a; i < b; ++i)
              if (!bit(usable, i)) {
                mark(usable, i, true);
                ++available;
              }
          }
          if (step > t->size - off)
            break;
          off += step;
        }
    }
    if (t->type == 8 && t->size >= 38) {
      struct fbtag *f = (void *)(uintptr_t)t;
      uint64_t span = (uint64_t)f->pitch * f->height;
      if (f->kind == 1 && (f->bpp == 24 || f->bpp == 32) && f->width >= 120 &&
          f->height >= 32 && f->width <= 4096 && f->height <= 2160 &&
          f->pitch >= f->width * (f->bpp / 8) && f->addr + span >= f->addr &&
          f->addr + span <= ((uint64_t)IDENTITY_ENTRIES << 21) && f->rs && f->rs <= 8 && f->gs &&
          f->gs <= 8 && f->bs && f->bs <= 8 && f->rp + f->rs <= f->bpp &&
          f->gp + f->gs <= f->bpp && f->bp + f->bs <= f->bpp) {
        fb = (void *)(uintptr_t)f->addr;
        pitch = f->pitch;
        fw = f->width;
        fh = f->height;
        bytespp = f->bpp / 8;
        rpos = f->rp;
        gpos = f->gp;
        bpos = f->bp;
        rbits = f->rs;
        gbits = f->gs;
        bbits = f->bs;
        cols = fw / 12;
        rows = fh / 16;
        fg = rgb(205, 225, 247);
        bg = rgb(13, 23, 39);
      }
    }
    uint32_t next = (t->size + 7) & ~7u;
    if (next > end - (uintptr_t)t)
      break;
    t = (void *)(uintptr_t)((uint8_t *)(uintptr_t)t + next);
  }
  reserve(0, (uintptr_t)&kernel_end);
  reserve(mbi, end);
  /* User virtual windows replace identity mappings in process CR3s. */
  reserve(0x08000000, 0x20000000);
  reserve(0x40000000, 0x40400000);
  reserve(0xbfc00000, 0xc0000000);
  if (fb)
    reserve((uintptr_t)fb, (uintptr_t)fb + pitch * fh);
  /* Boot modules remain reserved so future distribution payloads are not
   * overwritten. */
  for (struct tag *t = (void *)(uintptr_t)(mbi + 8); (uintptr_t)t <= end - 8;) {
    if (t->size < 8 || t->size > end - (uintptr_t)t || !t->type)
      break;
    if (t->type == 3 && t->size >= 16) {
      uint32_t *m = (void *)(uintptr_t)((uint8_t *)(uintptr_t)t + 8);
      if (m[1] >= m[0])
        reserve(m[0], m[1]);
    }
    uint32_t next = (t->size + 7) & ~7u;
    if (next > end - (uintptr_t)t)
      break;
    t = (void *)(uintptr_t)((uint8_t *)(uintptr_t)t + next);
  }
  console_clear();
  user_init();
  platform_enable();
  pci_ecam_init(mbi,end);
  devices_init();
  net_init();
  fs_init();
  console_write("AVALANCHE KERNEL 0.4.0\n");
  console_write(
      "--------------------------------------------------\nA small independent "
      "operating system kernel.\nBIOS / UEFI through GRUB Multiboot2\n\n");
  if (!fb)
    console_write("No supported RGB framebuffer. Using serial console.\n");
  console_write("Type help. Use run ush for the userspace shell.\n\n");
  fs_info();
  if (magenta_boot) {
    unsigned width, height;
    if (graphics_size(&width, &height))
      gui_run();
    console_write("Starting Magenta...\n");
    int code = user_exec("magenta", "");
    console_color(0);
    console_write("Magenta exited. Rescue console ready.\n");
    if (code < 0)
      result(code);
  }
  char line[4096];
  for (;;) {
    console_write("avalanche> ");
    size_t n = 0;
    for (;;) {
      char c = console_input();
      if (c == '\n') {
        console_putc('\n');
        break;
      }
      if (c == '\b') {
        if (n) {
          --n;
          console_write("\b \b");
        }
        continue;
      }
      if (c >= 32 && c <= 126 && n < sizeof(line) - 1) {
        line[n++] = c;
        console_putc(c);
      }
    }
    line[n] = 0;
    command(line);
  }
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = a, *y = b;
  while (n--) {
    if (*x != *y)
      return (int)*x - (int)*y;
    x++;
    y++;
  }
  return 0;
}
void *memchr(const void *p, int c, size_t n) {
  const uint8_t *s = p;
  while (n--) {
    if (*s == (uint8_t)c)
      return (void *)(uintptr_t)s;
    s++;
  }
  return NULL;
}
