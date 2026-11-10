#ifndef MAGENTA_APP_H
#define MAGENTA_APP_H
#include "lib.h"
#define MG_AF_INET 2
#define MG_SOCK_STREAM 1
#define MG_SOCK_NONBLOCK 0x800
#define MG_SOCK_CLOEXEC 0x80000
static inline struct av_sockaddr_in mg_ipv4(uint32_t address,uint16_t port) {
  struct av_sockaddr_in result={.family=MG_AF_INET,.port=(uint16_t)((port>>8)|(port<<8)),.address=__builtin_bswap32(address)};
  return result;
}
static inline int mg_socket(unsigned domain,unsigned type,unsigned protocol) {return av_call(AV_SOCKET,0,domain,type,protocol);}
static inline int mg_bind(unsigned fd,const void *address,unsigned length) {return av_call(AV_SOCKET,1,fd,(uintptr_t)address,length);}
static inline int mg_listen(unsigned fd,unsigned backlog) {return av_call(AV_SOCKET,2,fd,backlog,0);}
static inline int mg_accept(unsigned fd,void *address,unsigned *length) {return av_call(AV_SOCKET,3,fd,(uintptr_t)address,(uintptr_t)length);}
static inline int mg_connect(unsigned fd,const void *address,unsigned length) {return av_call(AV_SOCKET,4,fd,(uintptr_t)address,length);}
static inline int mg_send(unsigned fd,const void *data,unsigned length) {return av_call(AV_SOCKET,5,fd,(uintptr_t)data,length);}
static inline int mg_recv(unsigned fd,void *data,unsigned length) {return av_call(AV_SOCKET,6,fd,(uintptr_t)data,length);}
static inline int mg_socket_close(unsigned fd) {return av_call(AV_SOCKET,7,fd,0,0);}
static inline int mg_setsockopt(unsigned fd,unsigned level,unsigned name,unsigned value) {
  struct av_socket_option option={level,name,value};return av_call(AV_SOCKET,8,fd,(uintptr_t)&option,sizeof(option));
}
static inline int mg_getsockname(unsigned fd,void *address,unsigned *length) {return av_call(AV_SOCKET,9,fd,(uintptr_t)address,(uintptr_t)length);}
static inline int mg_getpeername(unsigned fd,void *address,unsigned *length) {return av_call(AV_SOCKET,10,fd,(uintptr_t)address,(uintptr_t)length);}
static inline int mg_shutdown(unsigned fd,unsigned how) {return av_call(AV_SOCKET,11,fd,how,0);}
static inline int mg_sandbox_info(struct av_sandbox_info *info) {
  return av_call(AV_SANDBOX_INFO, (uintptr_t)info, 0, 0, 0);
}
static inline int mg_sandbox_limit(unsigned capabilities) {
  return av_call(AV_SANDBOX_LIMIT, capabilities, 0, 0, 0);
}
static inline int mg_disk_health(unsigned disk, struct nvme_health *health, int refresh) {
  return av_call(AV_DISK_HEALTH, disk, (uintptr_t)health, sizeof(*health), refresh != 0);
}
static inline int mg_window_open(const struct av_gui_open *request) {
  return av_call(AV_GUI_OPEN, (uintptr_t)request, 0, 0, 0);
}
static inline int mg_window_draw(const struct av_gui_draw *request) {
  return av_call(AV_GUI_DRAW, (uintptr_t)request, 0, 0, 0);
}
static inline int mg_window_event(struct av_gui_event *event) {
  return av_call(AV_GUI_EVENT, (uintptr_t)event, 0, 0, 0);
}
static inline int mg_window_close(void) {
  return av_call(AV_GUI_CLOSE, 0, 0, 0, 0);
}
static inline int mg_clipboard_read(char *buffer, unsigned capacity) {
  return av_call(AV_CLIPBOARD_READ, (uintptr_t)buffer, capacity, 0, 0);
}
static inline int mg_clipboard_write(const char *text, unsigned length) {
  return av_call(AV_CLIPBOARD_WRITE, (uintptr_t)text, length, 0, 0);
}
static inline int mg_write(const char *text) {
  unsigned used = 0;
  while (text[used])
    used++;
  unsigned done = 0;
  while (done < used) {
    unsigned n = used - done;
    if (n > 4096)
      n = 4096;
    int r = av_call(AV_WRITE, (uintptr_t)(text + done), n, 0, 0);
    if (r < 0)
      return r;
    done += r;
  }
  return done;
}
static inline int mg_readline(char *buffer, unsigned size) {
  return av_call(AV_READLINE, (uintptr_t)buffer, size, 0, 0);
}
static inline int mg_read(const char *path, void *data, unsigned size) {
  return read_u(path, data, size);
}
static inline int mg_save(const char *path, const void *data, unsigned size) {
  return write_u(path, data, size);
}
static inline int mg_remove(const char *path) {
  return av_call(AV_FILE_REMOVE, (uintptr_t)path, 0, 0, 0);
}
static inline int mg_mkdir(const char *path) {
  return av_call(AV_FILE_MKDIR, (uintptr_t)path, 0, 0, 0);
}
static inline int mg_rename(const char *source, const char *destination) {
  return av_call(AV_FILE_RENAME, (uintptr_t)source, (uintptr_t)destination, 0,
                 0);
}
static inline int mg_copy(const char *source, const char *destination) {
  return av_call(AV_FILE_COPY, (uintptr_t)source, (uintptr_t)destination, 0, 0);
}
static inline int mg_spawn(const char *path, const char *arguments) {
  return av_call(AV_SPAWN, (uintptr_t)path, (uintptr_t)arguments, 0, 0);
}
static inline int mg_wait(int pid) { return av_call(AV_WAIT, pid, 0, 0, 0); }
static inline int mg_kill(int pid, int signal) {
  return av_call(AV_KILL, pid, signal, 0, 0);
}
static inline int mg_pid(void) { return av_call(AV_GETPID, 0, 0, 0, 0); }
static inline int mg_task(unsigned slot, struct av_task_info *info) {
  return av_call(AV_TASK_INFO, slot, (uintptr_t)info, 0, 0);
}
static inline int mg_status(struct av_status *info) {
  return av_call(AV_STATUS, (uintptr_t)info, 0, 0, 0);
}
static inline int mg_sleep(unsigned ticks) {
  return av_call(AV_SLEEP, ticks, 0, 0, 0);
}
static inline int mg_ticks(void) { return av_call(AV_TICKS, 0, 0, 0, 0); }
static inline int mg_dns(const char *name, uint32_t *ip) {
  return av_call(AV_DNS, (uintptr_t)name, (uintptr_t)ip, 0, 0);
}
static inline int mg_tcp_connect(uint32_t ip, unsigned port) {
  return av_call(AV_TCP_CONNECT, ip, port, 0, 0);
}
static inline int mg_tcp_send(const void *data, unsigned size) {
  return av_call(AV_TCP_SEND, (uintptr_t)data, size, 0, 0);
}
static inline int mg_tcp_receive(void *data, unsigned size, unsigned timeout) {
  return av_call(AV_TCP_RECV, (uintptr_t)data, size, timeout, 0);
}
static inline int mg_tcp_close(void) {
  return av_call(AV_TCP_CLOSE, 0, 0, 0, 0);
}
#endif
