#include "system.h"
bool disk_raw_read(unsigned, uint32_t, void *);
bool disk_raw_write(unsigned, uint32_t, const void *);
bool disk_raw_flush(unsigned);
#define CACHE_SLOTS 128
static struct cache_entry {
  unsigned disk;
  uint32_t sector;
  bool valid, dirty;
  uint8_t data[512];
} cache[CACHE_SLOTS];
static unsigned cursor;
static struct cache_entry *acquire(unsigned disk, uint32_t sector, bool read) {
  for (unsigned i=0;i<CACHE_SLOTS;i++)
    if (cache[i].valid && cache[i].disk==disk && cache[i].sector==sector) return &cache[i];
  struct cache_entry *entry = &cache[cursor];
  if (entry->valid && entry->dirty) {
    if (!disk_raw_write(entry->disk,entry->sector,entry->data)) return NULL;
    entry->dirty=false;
  }
  uint8_t data[512];
  if (read && !disk_raw_read(disk,sector,data)) return NULL;
  entry->valid=true; entry->disk=disk; entry->sector=sector;
  if (read) memcpy(entry->data,data,512);
  cursor=(cursor+1)%CACHE_SLOTS;
  return entry;
}
bool disk_read(unsigned disk, uint32_t sector, void *data) {
  const struct block_device *d=disk_get(disk);
  if (!d || sector>=d->sectors) return false;
  struct cache_entry *entry=acquire(disk,sector,true);
  if (!entry) return false;
  memcpy(data,entry->data,512); return true;
}
bool disk_write(unsigned disk, uint32_t sector, const void *data) {
  const struct block_device *d=disk_get(disk);
  if (!d || d->read_only || sector>=d->sectors) return false;
  struct cache_entry *entry=acquire(disk,sector,false);
  if (!entry) return false;
  memcpy(entry->data,data,512); entry->dirty=true; return true;
}
bool disk_flush(unsigned disk) {
  for (unsigned i=0;i<CACHE_SLOTS;i++) {
    struct cache_entry *entry=&cache[i];
    if (entry->valid && entry->disk==disk && entry->dirty) {
      if (!disk_raw_write(disk,entry->sector,entry->data)) return false;
      entry->dirty=false;
    }
  }
  return disk_raw_flush(disk);
}
