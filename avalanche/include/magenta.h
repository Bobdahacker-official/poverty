#ifndef MAGENTA_ABI_H
#define MAGENTA_ABI_H
#include <stdint.h>
struct av_gui_open {
  uint32_t width, height;
  char title[64];
};
struct av_gui_draw {
  uint32_t x, y, width, height, stride, reserved;
  uint64_t pixels;
};
struct av_gui_event {
  uint32_t type;
  int32_t a, b, c;
};
enum av_gui_event_type {
  AV_GUI_NONE,
  AV_GUI_TEXT,
  AV_GUI_KEY,
  AV_GUI_MOUSE,
  AV_GUI_QUIT
};
struct av_audio_status {
  uint32_t available, playing, rate, channels, frames_played, frames_total;
  int32_t error;
  char path[256];
};
struct av_status {
  uint32_t ticks, managed_kib, allocated_kib, disks;
  int32_t mounted_disk;
  uint32_t writable;
};
struct av_file_entry {
  char name[32];
  uint32_t size;
};
struct av_task_info {
  int32_t pid, parent;
  uint32_t state, priority, pages, cpu;
  char name[32];
};
enum av_report {
  AV_REPORT_FILESYSTEM,
  AV_REPORT_PROCESSES,
  AV_REPORT_NETWORK,
  AV_REPORT_DEVICES,
  AV_REPORT_DATE
};
enum av_power { AV_POWER_REBOOT, AV_POWER_HALT };
#endif
