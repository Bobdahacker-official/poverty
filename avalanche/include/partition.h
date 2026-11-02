#ifndef AVALANCHE_PARTITION_H
#define AVALANCHE_PARTITION_H
#include "system.h"
struct disk_partition {
  uint32_t start, sectors;
};
int partition_linux(unsigned disk, struct disk_partition *out,
                    unsigned capacity);
#endif
