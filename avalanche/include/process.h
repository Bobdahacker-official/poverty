#ifndef AVALANCHE_PROCESS_H
#define AVALANCHE_PROCESS_H
#include "system.h"
#define PROCESS_MAX 256
#define PROCESS_TABLES 32
#define PROCESS_DIRECTORY_ENTRIES 2048
#define PROCESS_FDS 16
#define PROCESS_STACK 32768
#define PROCESS_PAGE_LIMIT 16384
#define PROCESS_IMAGE_LIMIT (32u * 1024 * 1024)
#define PROCESS_PHYSICAL_MASK UINT64_C(0x000ffffffffff000)
#define PROCESS_NO_EXECUTE UINT64_C(0x8000000000000000)
#define FD_UNUSED (-1)
#define FD_INPUT (-2)
#define FD_OUTPUT (-3)
#define FD_ERROR (-4)
enum process_state {
  TASK_EMPTY,
  TASK_READY,
  TASK_RUNNING,
  TASK_SLEEP,
  TASK_INPUT,
  TASK_WAIT,
  TASK_ZOMBIE,
  TASK_POLL,
  TASK_FUTEX
};
struct process {
  uint64_t root[512] __attribute__((aligned(4096)));
  uint64_t pdpt[512] __attribute__((aligned(4096)));
  uint64_t directory[PROCESS_DIRECTORY_ENTRIES] __attribute__((aligned(4096)));
  uint64_t tables[PROCESS_TABLES][512] __attribute__((aligned(4096)));
  uint8_t kernel_stack[PROCESS_STACK] __attribute__((aligned(16)));
  uint16_t indices[PROCESS_TABLES];
  unsigned table_count, pages;
  struct trap_frame *frame;
  int pid, parent, status, signal;
  enum process_state state;
  bool linux_abi, linux64, foreground, yield, read_linux;
  uint32_t cpu_ticks, wake, read_address, read_capacity, read_count;
  uint32_t ready_since;
  unsigned priority, slice_left;
  int wait_pid;
  uint32_t wait_status;
  uint32_t heap_min, heap_break, mmap_next, clear_tid;
  uint64_t tls[3];
  uint64_t fs_base;
  uint32_t poll_address, poll_count, poll_deadline;
  bool poll_infinite;
  uint64_t futex_key, ignored_signals, blocked_signals, pending_signals;
  uint32_t futex_mask;
  bool futex_private, futex_timed;
  uint8_t fpu[512] __attribute__((aligned(16)));
  int fds[PROCESS_FDS];
  uint16_t cloexec;
  char name[32], arguments[256];
  char exec_argv[512], exec_env[512];
  uint16_t arg_offsets[16], env_offsets[8];
  unsigned argc, envc;
  bool exec_vectors;
};
struct process *process_current(void);
bool process_range(struct process *, uint32_t, uint32_t, bool);
bool process_copyin(struct process *, void *, uint32_t, uint32_t);
bool process_copyout(struct process *, uint32_t, const void *, uint32_t);
bool process_string(struct process *, uint32_t, char *, unsigned);
uint64_t *process_pte(struct process *, uint32_t);
bool process_map(struct process *, uint32_t, bool);
bool process_map_lazy(struct process *, uint32_t, bool);
bool process_execute(struct process *, uint32_t, bool);
bool process_private(struct process *, uint32_t);
bool process_page_fault(struct process *, uint32_t, bool);
void process_unmap(struct process *, uint32_t);
bool process_address(uint32_t, uint32_t);
void process_exit(struct process *, int, int);
void process_read(struct process *, uint32_t, uint32_t, bool);
int process_wait(struct process *, int, uint32_t, bool);
int process_fork(struct trap_frame *);
int process_execve(uint32_t, uint32_t, uint32_t);
void linux_syscall(struct trap_frame *);
void linux_poll_wake(struct process *);
int process_tcp_claim(int);
int process_futex_wake(uint64_t, bool, unsigned, uint32_t);
void process_pending_signals(struct process *);
void process_tcp_release(int);
void linux_init_fds(struct process *);
void linux_close_fds(struct process *);
void linux_clone_fds(struct process *, struct process *);
void linux_exec_fds(struct process *);
#endif
