#ifndef AVALANCHE_PROCESS_H
#define AVALANCHE_PROCESS_H
#include "system.h"
#define PROCESS_MAX 8
#define PROCESS_TABLES 12
#define PROCESS_FDS 16
#define PROCESS_STACK 32768
#define PROCESS_PAGE_LIMIT 2048
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
  TASK_ZOMBIE
};
struct process {
  uint32_t directory[1024] __attribute__((aligned(4096)));
  uint32_t tables[PROCESS_TABLES][1024] __attribute__((aligned(4096)));
  uint8_t kernel_stack[PROCESS_STACK] __attribute__((aligned(16)));
  uint16_t indices[PROCESS_TABLES];
  unsigned table_count, pages;
  struct trap_frame *frame;
  int pid, parent, status, signal;
  enum process_state state;
  bool linux_abi, foreground, yield, read_linux;
  uint32_t cpu_ticks, wake, read_address, read_capacity, read_count;
  int wait_pid;
  uint32_t wait_status;
  uint32_t heap_min, heap_break, mmap_next, clear_tid;
  uint64_t tls[3];
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
uint32_t *process_pte(struct process *, uint32_t);
bool process_map(struct process *, uint32_t, bool);
void process_unmap(struct process *, uint32_t);
bool process_address(uint32_t, uint32_t);
void process_exit(struct process *, int, int);
void process_read(struct process *, uint32_t, uint32_t, bool);
int process_wait(struct process *, int, uint32_t, bool);
int process_fork(struct trap_frame *);
int process_execve(uint32_t, uint32_t, uint32_t);
void linux_syscall(struct trap_frame *);
void linux_init_fds(struct process *);
void linux_close_fds(struct process *);
void linux_clone_fds(struct process *, struct process *);
void linux_exec_fds(struct process *);
#endif
