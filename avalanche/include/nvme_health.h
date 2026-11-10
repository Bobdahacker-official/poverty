#ifndef AV_NVME_HEALTH_H
#define AV_NVME_HEALTH_H
#include "magenta.h"
#include <stdbool.h>
int nvme_health_read(unsigned, struct nvme_health *, bool);
void nvme_counter_format(char *, unsigned, const uint8_t *);
void nvme_health_command(char *);
#endif
