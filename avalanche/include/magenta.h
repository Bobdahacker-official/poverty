#ifndef MAGENTA_ABI_H
#define MAGENTA_ABI_H
#include <stdint.h>
struct av_status {
  uint32_t ticks, managed_kib, allocated_kib, disks;
  int32_t mounted_disk;
  uint32_t writable;
};
struct av_file_entry {
  char name[32];
  uint32_t size;
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
