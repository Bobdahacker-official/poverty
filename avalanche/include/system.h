#ifndef AVALANCHE_SYSTEM_H
#define AVALANCHE_SYSTEM_H
#include "avalanche.h"
#include "magenta.h"
static inline void io_out8(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline void io_out16(uint16_t p, uint16_t v) {
  __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static inline void io_out32(uint16_t p, uint32_t v) {
  __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t io_in8(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline uint16_t io_in16(uint16_t p) {
  uint16_t v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline uint32_t io_in32(uint16_t p) {
  uint32_t v;
  __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline void cpu_pause(void) { __asm__ volatile("pause"); }
static inline bool str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
void print_uint(uint32_t n);
void print_hex(uint32_t n);
char console_input(void);
char console_getchar(void);
void console_clear(void);
void console_color(uint32_t);
void kernel_status(struct av_status *);
void kernel_power(unsigned);
void console_size(uint16_t *, uint16_t *);
uint32_t parse_uint(const char *, bool *);
uint32_t crc32(const void *, size_t);
void platform_init(void);
void platform_enable(void);
uint32_t clock_ticks(void);
void sleep_ticks(uint32_t);
void devices_init(void);
void devices_poll(void);
void devices_tick(void);
void mouse_interrupt_poll(bool);
void devices_list(void);
void rtc_print(void);
void speaker_beep(uint32_t hz, uint32_t duration);
void mouse_print(void);
void mouse_state(int *, int *, uint8_t *);
void mouse_input_state(int *,int *,uint8_t *);
void mouse_configure(unsigned, unsigned, unsigned);
bool mouse_absolute(void);
int graphics_key_event(void);
void graphics_console_capture(void (*)(char));
bool graphics_size(unsigned *, unsigned *);
void graphics_present(const uint32_t *, unsigned, unsigned);
void graphics_present_region(const uint32_t *, unsigned, unsigned, unsigned,
                             unsigned, unsigned, unsigned);
uint8_t graphics_glyph(char, unsigned);
int gui_run(void);
void kernel_capture(char *, char *, unsigned);
struct pci_device {
  uint8_t bus, slot, function, class_id, subclass, prog_if, irq;
  uint16_t vendor, device;
  uint32_t bar[6];
};
uint32_t pci_read(const struct pci_device *, uint8_t);
void pci_write(const struct pci_device *, uint8_t, uint32_t);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);
const struct pci_device *pci_find_class(uint8_t, uint8_t, uint8_t);
void pci_enable(const struct pci_device *, uint16_t);
struct block_device {
  uint16_t port, control;
  uint8_t slave, backend, unit, read_only;
  uint32_t sectors;
  char model[41];
};
unsigned disk_count(void);
bool disk_register(const struct block_device *);
void ahci_init(void);
bool ahci_transfer(unsigned, uint32_t, void *, bool);
bool ahci_flush(unsigned);
const struct block_device *disk_get(unsigned);
bool disk_read(unsigned, uint32_t, void *);
bool disk_write(unsigned, uint32_t, const void *);
bool disk_flush(unsigned);
#define FS_MAX_FILE 65536u
void fs_init(void);
int fs_mount(unsigned);
int fs_format(unsigned);
void fs_info(void);
int fs_mounted_disk(void);
int fs_browse(const char *, unsigned, char[256], uint64_t *, bool *);
int fs_directory(const char *);
bool fs_writable(void);
void fs_list(void);
int fs_read(const char *, void *, uint32_t);
int fs_write(const char *, const void *, uint32_t);
int fs_remove(const char *);
int fs_size(const char *);
bool fs_entry(unsigned, char *, uint32_t *);
void net_init(void);
void net_poll(void);
void net_info(void);
struct network_info {
  uint32_t ip, mask, gateway, dns, rx, tx, drops;
  bool present;
  char adapter[32];
};
void net_snapshot(struct network_info *);
bool ip_parse(const char *, uint32_t *);
void ip_print(uint32_t);
void net_config(uint32_t, uint32_t, uint32_t, uint32_t);
int net_dhcp(void);
int net_ping(uint32_t);
int net_dns(const char *, uint32_t *);
int net_udp_send(uint32_t, uint16_t, uint16_t, const void *, uint32_t);
int net_udp_recv(uint16_t, void *, uint32_t, uint32_t *, uint16_t *);
int net_tcp_connect(uint32_t, uint16_t);
int net_tcp_send(const void *, uint32_t);
int net_tcp_recv(void *, uint32_t, uint32_t);
int net_tcp_close(void);
void net_http(const char *, const char *);
struct trap_frame {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax, vector,
      error, eip, cs, eflags, useresp, ss;
};
struct trap_frame *trap_dispatch(struct trap_frame *);
_Static_assert(sizeof(void *) == 8, "Avalanche requires an x86-64 compiler");
_Static_assert(offsetof(struct trap_frame, vector) == 160,
               "interrupt frame ABI");
_Static_assert(sizeof(struct trap_frame) == 216, "iretq frame ABI");
void user_init(void);
bool platform_mmio(uint32_t, uint32_t);
int user_run(const void *, uint32_t, const char *);
int user_exec(const char *, const char *);
int user_install(void);
bool user_active(void);
struct trap_frame *user_timer(struct trap_frame *);
struct trap_frame *user_reschedule(struct trap_frame *);
void user_fault(unsigned, uint32_t);
void user_poll(void);
void user_tty_external(bool);
void user_tty_input(char);
struct task_info {
  int pid, parent;
  unsigned state, priority, pages, cpu;
  char name[32];
};
bool user_task_info(unsigned, struct task_info *);
char user_tty_getchar(void);
void user_yield_kernel(void);
int user_spawn(const char *, const char *, bool);
int user_wait_pid(int);
int user_kill(int, int);
int user_priority(int, unsigned);
void user_list(void);
void platform_kernel_stack(uint32_t);
void platform_tls(const uint64_t *);
void platform_fpu_save(void *);
void platform_fpu_restore(const void *);
void platform_fpu_initial(void *);
void user_syscall(struct trap_frame *);
#endif
