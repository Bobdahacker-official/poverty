#ifndef MAGENTA_ABI_H
#define MAGENTA_ABI_H
#include <stdint.h>
struct av_socket_option {uint32_t level, name, value;};
struct av_sockaddr_in { uint16_t family, port; uint32_t address; uint8_t zero[8]; };
enum av_capability {
  AV_CAP_READ = 1u << 0, AV_CAP_WRITE = 1u << 1, AV_CAP_NETWORK = 1u << 2,
  AV_CAP_EXEC = 1u << 3, AV_CAP_GUI = 1u << 4, AV_CAP_AUDIO = 1u << 5,
  AV_CAP_CLIPBOARD_READ = 1u << 6, AV_CAP_CLIPBOARD_WRITE = 1u << 7,
  AV_CAP_PROCESSES = 1u << 8, AV_CAP_ADMIN = 1u << 9,
  AV_CAP_EXEC_MEMORY = 1u << 10, AV_CAP_ALL = (1u << 11) - 1
};
struct av_sandbox_info {
  uint32_t version, capabilities;
  uint8_t executable_sha256[32];
  char private_directory[80];
};

#define NVME_HEALTH_COUNTERS 10
struct nvme_health {
  uint32_t version, namespace_id, logical_block_size, sampled_ticks;
  uint64_t namespace_blocks;
  char model[41], serial[21], firmware[9];
  uint8_t critical_warning, available_spare, spare_threshold, percentage_used;
  uint16_t temperature_kelvin, temperature_sensors[8];
  uint8_t counters[NVME_HEALTH_COUNTERS][16];
  uint32_t warning_temperature_minutes, critical_temperature_minutes;
  uint32_t thermal_transitions[2], thermal_seconds[2];
};

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
#define AV_LINUX_DOWNLOAD 4101
enum av_download_operation {
  AV_DOWNLOAD_BEGIN, AV_DOWNLOAD_DATA, AV_DOWNLOAD_DONE, AV_DOWNLOAD_FAILED,
  AV_DOWNLOAD_STATUS
};
struct av_download_request {
  uint32_t operation, id, size;
  uint64_t total, data;
  char filename[128];
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
