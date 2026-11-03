#ifndef MAGENTA_APP_H
#define MAGENTA_APP_H
#include "lib.h"
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
