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
void console_keyboard_poll(void);
void keyboard_feed_scancode(uint8_t);
void mouse_feed_relative(int, int, uint8_t);
void usb_init(void);
void usb_poll(void);
bool usb_storage_transfer(unsigned, uint32_t, void *, bool);
bool usb_storage_flush(unsigned);
void console_clear(void);
void console_color(uint32_t);
void kernel_status(struct av_status *);
void kernel_power(unsigned);
bool power_flush(void);
bool firmware_reset(void);
bool firmware_poweroff(void);
bool secure_random(void *, unsigned);
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
void devices_display(void);
void audio_init(void);
unsigned audio_output_count(void);
unsigned audio_output_selected(void);
unsigned audio_output_type(unsigned);
int audio_output_select(unsigned);
unsigned audio_volume(void);
bool audio_muted(void);
int audio_set_volume(unsigned, bool);
void audio_info(void);
int audio_play(const char *);
bool audio_paused(void);
int audio_pause(void);
int audio_resume(void);
int audio_seek(unsigned frame);
int audio_status(void);
void audio_stop(void);
void audio_poll(void);
void audio_snapshot(struct av_audio_status *);
void rtc_print(void);
void speaker_beep(uint32_t hz, uint32_t duration);
void mouse_print(void);
void mouse_state(int *, int *, uint8_t *);
void mouse_input_state(int *, int *, uint8_t *);
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
uint64_t pci_memory_bar(const struct pci_device *, unsigned);
void pci_write(const struct pci_device *, uint8_t, uint32_t);
unsigned pci_count_devices(void);
const struct pci_device *pci_device_at(unsigned);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);
const struct pci_device *pci_find_class(uint8_t, uint8_t, uint8_t);
void pci_ecam_init(uint32_t mbi, uint32_t end);
void pci_enable(const struct pci_device *, uint16_t);
struct block_device {
  uint16_t port, control;
  uint8_t slave, backend, unit, read_only;
  uint32_t sectors;
  uint32_t logical_sector_size;
  char model[41];
};
unsigned disk_count(void);
bool disk_register(const struct block_device *);
void nvme_init(void);
bool nvme_transfer(unsigned, uint32_t, void *, bool);
bool nvme_flush(unsigned);
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
bool fs_secondary_path(const char *, unsigned *, const char **);
int fs_secondary_state(unsigned);
int fs_secondary_mount(unsigned, bool);
int fs_secondary_eject(unsigned);
void fs_secondary_detach(unsigned);
bool linux_disk_busy(unsigned);
struct process;
int gui_download_call(struct process *, uint32_t);
void gui_download_disconnect(int);
void disk_cache_forget(unsigned);
void disk_detach_usb(unsigned);
int fs_browse(const char *, unsigned, char[256], uint64_t *, bool *);
int fs_directory(const char *);
int fs_copy(const char *, const char *);
bool fs_writable(void);
void fs_list(void);
int fs_read(const char *, void *, uint32_t);
int fs_read_at(const char *, uint64_t, void *, uint32_t);
int fs_write(const char *, const void *, uint32_t);
int fs_write_new_stream(const char *, const void *, uint32_t);
int fs_remove(const char *);
int fs_mkdir(const char *);
int fs_rename(const char *, const char *);
int fs_size(const char *);
struct file_metadata {
  uint64_t size, blocks;
  uint32_t inode, mode, uid, gid, links, atime, mtime, ctime;
};
int fs_metadata(const char *, struct file_metadata *);
bool rtc_time(uint32_t *, uint32_t *);
bool fs_entry(unsigned, char *, uint32_t *);
bool pcnet_init(uint8_t mac[6]);
bool pcnet_send(const void *, unsigned);
void pcnet_poll(void (*receive)(const uint8_t *, uint32_t));
bool rtl8169_init(uint8_t[6]);
bool rtl8169_send(const void *, unsigned);
bool rtl8169_link(void);
void rtl8169_poll(void (*)(const uint8_t *, uint32_t));
void rtl8169_diagnostics(void);
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
int net_use_usb_ethernet(bool);
bool net_usb_ethernet_selected(void);
int net_ping(uint32_t);
int net_dns(const char *, uint32_t *);
int net_udp_send(uint32_t, uint16_t, uint16_t, const void *, uint32_t);
int net_udp_recv(uint16_t, void *, uint32_t, uint32_t *, uint16_t *);
int net_tcp_connect(uint32_t, uint16_t);
int net_tcp_send(const void *, uint32_t);
int net_tcp_recv(void *, uint32_t, uint32_t);
int net_tcp_close(void);
struct tcp_snapshot {
  bool active, established, closed, reset;
  uint32_t queued, remote;
  uint16_t local_port, remote_port;
};
void net_tcp_snapshot(struct tcp_snapshot *);
void linux_trace_set(bool);
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
bool platform_mmio(uint64_t, uint32_t);
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
uint64_t user_cpu_ticks(void);
char user_tty_getchar(void);
void user_yield_kernel(void);
int user_spawn(const char *, const char *, bool);
int user_wait_pid(int);
int user_kill(int, int);
int user_priority(int, unsigned);
void user_list(void);
void platform_kernel_stack(uint64_t);
void platform_user_fs(uint64_t);
void linux_syscall64(struct trap_frame *);
void platform_tls(const uint64_t *);
void platform_fpu_save(void *);
void platform_fpu_restore(const void *);
void platform_fpu_initial(void *);
void user_syscall(struct trap_frame *);

bool rge_init(uint8_t mac[6]);
bool rge_link(void);
bool rge_send(const void *,unsigned);
void rge_poll(void (*)(const uint8_t *,uint32_t));
void rge_diagnostics(void);

#endif
