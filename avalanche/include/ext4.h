#ifndef AVALANCHE_EXT4_H
#define AVALANCHE_EXT4_H
#include "system.h"
struct ext4_volume {
  unsigned disk;
  uint32_t start, inodes_per_group, inode_size, descriptor_size;
  uint64_t blocks;
};
struct ext4_inode {
  uint32_t number;
  uint16_t mode;
  uint32_t flags;
  uint64_t size;
  uint8_t extent_root[60];
};
int ext4_mount(struct ext4_volume *, unsigned, uint32_t);
int ext4_inode_read(struct ext4_volume *, uint32_t, struct ext4_inode *);
int ext4_read(struct ext4_volume *, const struct ext4_inode *, uint64_t, void *,
              uint32_t);
int ext4_lookup(struct ext4_volume *, const char *, struct ext4_inode *);
int ext4_list(struct ext4_volume *, const char *);
int ext4_write(struct ext4_volume *, const struct ext4_inode *, uint64_t,
               const void *, uint32_t);
int ext4_create(struct ext4_volume *, const char *, struct ext4_inode *);
int ext4_replace(struct ext4_volume *, const char *, const void *, uint32_t);
int ext4_mkdir(struct ext4_volume *, const char *);
int ext4_rename(struct ext4_volume *, const char *, const char *);
int ext4_remove(struct ext4_volume *, const char *);
int ext4_entry(struct ext4_volume *, const char *, unsigned, char[256],
               uint64_t *);
#endif
