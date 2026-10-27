#include "ext4.h"
#define TX_LIMIT 32
static struct {
  uint64_t block;
  uint8_t data[4096];
} transaction[TX_LIMIT];
static unsigned transaction_count;
static struct ext4_volume *transaction_volume;
static int journal_replay(struct ext4_volume *);
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) {
  return u16(p) | (uint32_t)u16(p + 2) << 16;
}
static int bytes(struct ext4_volume *v, uint64_t offset, void *out,
                 uint32_t n) {
  uint8_t sector[512], *p = out;
  while (n) {
    if (transaction_volume == v) {
      bool found = false;
      for (unsigned i = 0; i < transaction_count; i++)
        if (transaction[i].block == offset / 4096) {
          unsigned count = 4096 - offset % 4096;
          if (count > n)
            count = n;
          memcpy(p, transaction[i].data + offset % 4096, count);
          p += count;
          offset += count;
          n -= count;
          found = true;
          break;
        }
      if (found)
        continue;
    }
    uint64_t lba = v->start + offset / 512;
    if (lba > UINT32_MAX || !disk_read(v->disk, lba, sector))
      return -5;
    unsigned within = offset % 512, count = 512 - within;
    if (count > n)
      count = n;
    memcpy(p, sector + within, count);
    p += count;
    offset += count;
    n -= count;
  }
  return 0;
}
int ext4_mount(struct ext4_volume *v, unsigned disk, uint32_t start) {
  uint8_t super[1024];
  memset(v, 0, sizeof(*v));
  v->disk = disk;
  v->start = start;
  if (bytes(v, 1024, super, sizeof(super)))
    return -5;
  if (u16(super + 56) != 0xef53 || u32(super + 24) != 2)
    return -22;
  uint32_t incompatible = u32(super + 96);
  if (u16(super + 58) != 1)
    return -117;
  /* Refuse dirty journals and layouts requiring a different block translator.
   */
  if (incompatible & ~(2u | 4u | 0x40u | 0x80u | 0x200u | 0x2000u))
    return -95;
  v->blocks = u32(super + 4);
  if (incompatible & 0x80)
    v->blocks |= (uint64_t)u32(super + 0x150) << 32;
  v->inodes_per_group = u32(super + 40);
  v->inode_size = u16(super + 88);
  v->descriptor_size = incompatible & 0x80 ? u16(super + 254) : 32;
  const struct block_device *d = disk_get(disk);
  if (!d || start >= d->sectors || !v->blocks ||
      v->blocks > (d->sectors - (uint64_t)start) / 8 || !v->inodes_per_group ||
      v->inodes_per_group > 32768 || v->inode_size < 128 ||
      v->inode_size > 4096 || 4096 % v->inode_size || v->descriptor_size < 32 ||
      v->descriptor_size > 4096 || 4096 % v->descriptor_size)
    return -22;
  return incompatible & 4 ? journal_replay(v) : 0;
}
int ext4_inode_read(struct ext4_volume *v, uint32_t ino,
                    struct ext4_inode *out) {
  if (!ino || !v->inodes_per_group)
    return -22;
  uint8_t descriptor[64], inode[128];
  uint32_t group = (ino - 1) / v->inodes_per_group,
           index = (ino - 1) % v->inodes_per_group;
  if (bytes(v, 4096 + (uint64_t)group * v->descriptor_size, descriptor,
            v->descriptor_size >= 64 ? 64 : 32))
    return -5;
  uint64_t table = u32(descriptor + 8);
  if (v->descriptor_size >= 64)
    table |= (uint64_t)u32(descriptor + 40) << 32;
  if (!table || table >= v->blocks ||
      bytes(v, table * 4096 + (uint64_t)index * v->inode_size, inode, 128))
    return -5;
  out->mode = u16(inode);
  out->number = ino;
  out->flags = u32(inode + 32);
  out->size = u32(inode + 4) | (uint64_t)u32(inode + 108) << 32;
  memcpy(out->extent_root, inode + 40, 60);
  return 0;
}
static int extent(struct ext4_volume *v, const struct ext4_inode *inode,
                  uint32_t logical, uint64_t *physical) {
  if (!(inode->flags & 0x80000))
    return -95;
  uint8_t node[4096];
  memcpy(node, inode->extent_root, 60);
  unsigned capacity = 60, expected = u16(node + 6);
  if (expected > 5)
    return -5;
  for (;;) {
    unsigned count = u16(node + 2), depth = u16(node + 6);
    if (u16(node) != 0xf30a || depth != expected ||
        count > (capacity - 12) / 12)
      return -5;
    const uint8_t *found = NULL;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; i++) {
      const uint8_t *e = node + 12 + i * 12;
      uint32_t first = u32(e);
      if (i && first <= previous)
        return -5;
      previous = first;
      if (first <= logical)
        found = e;
    }
    *physical = 0;
    if (!found)
      return 0;
    if (!depth) {
      unsigned length = u16(found + 4);
      bool unwritten = length > 32768;
      if (unwritten)
        length -= 32768;
      uint32_t delta = logical - u32(found);
      if (delta >= length || unwritten)
        return 0;
      *physical = (u32(found + 8) | (uint64_t)u16(found + 6) << 32) + delta;
      return *physical < v->blocks ? 0 : -5;
    }
    uint64_t child = u32(found + 4) | (uint64_t)u16(found + 8) << 32;
    if (!child || child >= v->blocks ||
        bytes(v, child * 4096, node, sizeof(node)))
      return -5;
    capacity = sizeof(node);
    expected--;
  }
}
int ext4_read(struct ext4_volume *v, const struct ext4_inode *inode,
              uint64_t offset, void *out, uint32_t n) {
  uint8_t *p = out;
  uint32_t done = 0;
  if (offset >= inode->size)
    return 0;
  if (n > inode->size - offset)
    n = inode->size - offset;
  while (done < n) {
    if (offset / 4096 > UINT32_MAX)
      return -27;
    uint64_t physical;
    int r = extent(v, inode, offset / 4096, &physical);
    if (r)
      return r;
    unsigned within = offset % 4096, count = 4096 - within;
    if (count > n - done)
      count = n - done;
    if (!physical)
      memset(p + done, 0, count);
    else if (bytes(v, physical * 4096 + within, p + done, count))
      return -5;
    done += count;
    offset += count;
  }
  return done;
}
static int directory(struct ext4_volume *v, const struct ext4_inode *inode,
                     const char *name) {
  uint8_t block[4096];
  if ((inode->mode & 0xf000) != 0x4000)
    return -20;
  for (uint64_t offset = 0; offset < inode->size; offset += 4096) {
    int n = ext4_read(v, inode, offset, block, sizeof(block));
    if (n < 0)
      return n;
    for (unsigned p = 0; p < (unsigned)n;) {
      if ((unsigned)n - p < 8)
        return -5;
      uint32_t ino = u32(block + p);
      unsigned len = u16(block + p + 4), size = block[p + 6];
      if (len < 8 || (len & 3) || len > (unsigned)n - p || size > len - 8)
        return -5;
      if (ino && size) {
        char entry[256];
        memcpy(entry, block + p + 8, size);
        entry[size] = 0;
        if (name && str_eq(name, entry))
          return ino;
        if (!name) {
          console_write(entry);
          console_putc('\n');
        }
      }
      p += len;
    }
  }
  return name ? -2 : 0;
}
int ext4_lookup(struct ext4_volume *v, const char *path,
                struct ext4_inode *inode) {
  int r = ext4_inode_read(v, 2, inode);
  while (!r && *path) {
    while (*path == '/')
      path++;
    if (!*path)
      break;
    char name[256];
    unsigned n = 0;
    while (*path && *path != '/') {
      if (n == 255)
        return -36;
      name[n++] = *path++;
    }
    name[n] = 0;
    int ino = directory(v, inode, name);
    if (ino < 0)
      return ino;
    r = ext4_inode_read(v, ino, inode);
  }
  return r;
}
int ext4_list(struct ext4_volume *v, const char *path) {
  struct ext4_inode inode;
  int r = ext4_lookup(v, path, &inode);
  return r ? r : directory(v, &inode, NULL);
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void putbe(uint8_t *p, uint32_t n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static int store(struct ext4_volume *v, uint64_t offset, const void *data,
                 unsigned n) {
  const uint8_t *p = data;
  uint8_t sector[512];
  while (n) {
    uint64_t lba = v->start + offset / 512;
    unsigned within = offset % 512, count = 512 - within;
    if (count > n)
      count = n;
    if (lba > UINT32_MAX || !disk_read(v->disk, lba, sector))
      return -5;
    memcpy(sector + within, p, count);
    if (!disk_write(v->disk, lba, sector))
      return -5;
    n -= count;
    p += count;
    offset += count;
  }
  return disk_flush(v->disk) ? 0 : -5;
}
static int recovery_flag(struct ext4_volume *v, bool active) {
  uint8_t super[1024];
  if (bytes(v, 1024, super, 1024))
    return -5;
  if (active)
    super[96] |= 4;
  else
    super[96] &= ~4;
  if (u32(super + 100) & 0x400) {
    uint32_t crc = ~0u;
    for (unsigned i = 0; i < 1020; i++) {
      crc ^= super[i];
      for (unsigned j = 0; j < 8; j++)
        crc = (crc >> 1) ^ (0x82f63b78u & -(crc & 1));
    }
    for (unsigned i = 0; i < 4; i++)
      super[1020 + i] = crc >> (i * 8);
  }
  return store(v, 1024, super, 1024);
}
static int journal_open(struct ext4_volume *v, struct ext4_inode *inode,
                        uint8_t *super) {
  uint8_t fs[1024];
  if (bytes(v, 1024, fs, sizeof(fs)))
    return -5;
  if (!(u32(fs + 92) & 4) || !u32(fs + 224))
    return -95;
  int r = ext4_inode_read(v, u32(fs + 224), inode);
  if (r)
    return r;
  r = ext4_read(v, inode, 0, super, 4096);
  if (r != 4096)
    return -5;
  if (be32(super) != 0xc03b3998 || be32(super + 4) != 4 ||
      be32(super + 12) != 4096 || be32(super + 36) || be32(super + 40) ||
      be32(super + 44) || be32(super + 16) < 4 ||
      (uint64_t)be32(super + 16) * 4096 > inode->size || !be32(super + 20) ||
      be32(super + 20) >= be32(super + 16))
    return -95;
  return 0;
}
static int journal_store(struct ext4_volume *v, struct ext4_inode *inode,
                         uint32_t block, const void *data) {
  uint64_t physical;
  if ((uint64_t)block * 4096 >= inode->size)
    return -5;
  int r = extent(v, inode, block, &physical);
  if (r || !physical)
    return r ? r : -5;
  return store(v, physical * 4096, data, 4096);
}
static int journal_finish(struct ext4_volume *v, struct ext4_inode *inode,
                          uint8_t *super) {
  putbe(super + 24, be32(super + 24) + 1);
  putbe(super + 28, 0);
  int r = journal_store(v, inode, 0, super);
  return r ? r : recovery_flag(v, false);
}
static int journal_replay(struct ext4_volume *v) {
  struct ext4_inode inode;
  uint8_t super[4096], descriptor[4096], commit[4096], data[4096];
  int r = journal_open(v, &inode, super);
  if (r)
    return r;
  uint32_t start = be32(super + 28), sequence = be32(super + 24);
  if (!start)
    return recovery_flag(v, false);
  if (start >= be32(super + 16) ||
      ext4_read(v, &inode, (uint64_t)start * 4096, descriptor, 4096) != 4096)
    return -5;
  if (be32(descriptor) != 0xc03b3998 || be32(descriptor + 8) != sequence)
    return journal_finish(v, &inode, super);
  if (be32(descriptor + 4) != 1)
    return -95;
  uint32_t targets[TX_LIMIT], flags[TX_LIMIT];
  unsigned count = 0, at = 12;
  while (count < TX_LIMIT && at + 8 <= 4096) {
    targets[count] = be32(descriptor + at);
    flags[count] = be32(descriptor + at + 4);
    at += 8;
    if (flags[count] & ~11u || targets[count] >= v->blocks)
      return -95;
    if (!(flags[count] & 2))
      at += 16;
    count++;
    if (flags[count - 1] & 8)
      break;
  }
  if (!count || !(flags[count - 1] & 8) ||
      start + count + 1 >= be32(super + 16))
    return -95;
  if (ext4_read(v, &inode, (uint64_t)(start + count + 1) * 4096, commit,
                4096) != 4096)
    return -5;
  bool committed = be32(commit) == 0xc03b3998 && be32(commit + 4) == 2 &&
                   be32(commit + 8) == sequence;
  if (committed) {
    for (unsigned i = 0; i < count; i++) {
      if (ext4_read(v, &inode, (uint64_t)(start + i + 1) * 4096, data, 4096) !=
          4096)
        return -5;
      if (flags[i] & 1)
        putbe(data, 0xc03b3998);
      if ((r = store(v, (uint64_t)targets[i] * 4096, data, 4096)))
        return r;
    }
  }
  return journal_finish(v, &inode, super);
}
static int journal_write(struct ext4_volume *v, uint64_t block,
                         const void *data) {
  struct ext4_inode inode;
  uint8_t super[4096], descriptor[4096], commit[4096], payload[4096];
  int r = journal_open(v, &inode, super);
  if (r)
    return r;
  if (block > UINT32_MAX || be32(super + 28))
    return -16;
  uint32_t start = be32(super + 20), sequence = be32(super + 24);
  if (start + 2 >= be32(super + 16))
    return -28;
  memset(descriptor, 0, sizeof(descriptor));
  memset(commit, 0, sizeof(commit));
  putbe(descriptor, 0xc03b3998);
  putbe(descriptor + 4, 1);
  putbe(descriptor + 8, sequence);
  putbe(descriptor + 12, block);
  putbe(descriptor + 16, 10);
  memcpy(payload, data, 4096);
  if (be32(payload) == 0xc03b3998) {
    putbe(payload, 0);
    putbe(descriptor + 16, 11);
  }
  putbe(commit, 0xc03b3998);
  putbe(commit + 4, 2);
  putbe(commit + 8, sequence);
  if ((r = recovery_flag(v, true)))
    return r;
  putbe(super + 28, start);
  if ((r = journal_store(v, &inode, 0, super)) ||
      (r = journal_store(v, &inode, start, descriptor)) ||
      (r = journal_store(v, &inode, start + 1, payload)) ||
      (r = journal_store(v, &inode, start + 2, commit)) ||
      (r = store(v, block * 4096, data, 4096)))
    return r;
  return journal_finish(v, &inode, super);
}
static void little16(uint8_t *p, uint16_t n) {
  p[0] = n;
  p[1] = n >> 8;
}
static void little32(uint8_t *p, uint32_t n) {
  little16(p, n);
  little16(p + 2, n >> 16);
}
static uint32_t checksum(uint32_t crc, const void *data, unsigned size) {
  const uint8_t *p = data;
  while (size--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0x82f63b78u & -(crc & 1));
  }
  return crc;
}
static void super_checksum(uint8_t *super) {
  if (u32(super + 100) & 0x400)
    little32(super + 1020, checksum(~0u, super, 1020));
}
static uint8_t *edit_block(struct ext4_volume *v, uint64_t block) {
  if (transaction_volume != v || block >= v->blocks)
    return NULL;
  for (unsigned i = 0; i < transaction_count; i++)
    if (transaction[i].block == block)
      return transaction[i].data;
  if (transaction_count == TX_LIMIT)
    return NULL;
  unsigned i = transaction_count;
  if (bytes(v, block * 4096, transaction[i].data, 4096))
    return NULL;
  transaction[i].block = block;
  transaction_count++;
  return transaction[i].data;
}
static void begin_transaction(struct ext4_volume *v) {
  transaction_volume = v;
  transaction_count = 0;
}
static int commit_transaction(struct ext4_volume *v) {
  transaction_volume = NULL;
  if (!transaction_count)
    return 0;
  struct ext4_inode inode;
  uint8_t super[4096], descriptor[4096], payload[4096];
  int r = journal_open(v, &inode, super);
  if (r)
    return r;
  if (be32(super + 28))
    return -16;
  uint32_t start = be32(super + 20), sequence = be32(super + 24);
  if (start + transaction_count + 1 >= be32(super + 16))
    return -28;
  memset(descriptor, 0, sizeof(descriptor));
  putbe(descriptor, 0xc03b3998);
  putbe(descriptor + 4, 1);
  putbe(descriptor + 8, sequence);
  for (unsigned i = 0; i < transaction_count; i++) {
    if (transaction[i].block > UINT32_MAX)
      return -27;
    if (transaction[i].block == 0) {
      transaction[i].data[1024 + 96] |= 4;
      super_checksum(transaction[i].data + 1024);
    }
    putbe(descriptor + 12 + i * 8, transaction[i].block);
    unsigned flags = 2 | (i + 1 == transaction_count ? 8 : 0);
    if (be32(transaction[i].data) == 0xc03b3998)
      flags |= 1;
    putbe(descriptor + 16 + i * 8, flags);
  }
  if ((r = recovery_flag(v, true)))
    return r;
  putbe(super + 28, start);
  if ((r = journal_store(v, &inode, 0, super)) ||
      (r = journal_store(v, &inode, start, descriptor)))
    return r;
  for (unsigned i = 0; i < transaction_count; i++) {
    memcpy(payload, transaction[i].data, 4096);
    if (be32(descriptor + 16 + i * 8) & 1)
      putbe(payload, 0);
    if ((r = journal_store(v, &inode, start + i + 1, payload)))
      return r;
  }
  memset(payload, 0, sizeof(payload));
  putbe(payload, 0xc03b3998);
  putbe(payload + 4, 2);
  putbe(payload + 8, sequence);
  if ((r = journal_store(v, &inode, start + transaction_count + 1, payload)))
    return r;
  for (unsigned i = 0; i < transaction_count; i++)
    if ((r = store(v, transaction[i].block * 4096, transaction[i].data, 4096)))
      return r;
  return journal_finish(v, &inode, super);
}
static uint32_t metadata_seed(const uint8_t *super) {
  return u32(super + 96) & 0x2000 ? u32(super + 624)
                                  : checksum(~0u, super + 104, 16);
}
static uint8_t *descriptor_edit(struct ext4_volume *v, uint32_t group) {
  uint64_t address = 4096 + (uint64_t)group * v->descriptor_size;
  uint8_t *block = edit_block(v, address / 4096);
  return block ? block + address % 4096 : NULL;
}
static void descriptor_checksum(struct ext4_volume *v, uint32_t group,
                                uint8_t *descriptor, uint8_t *super) {
  if (!(u32(super + 100) & 0x400))
    return;
  uint8_t number[4];
  little32(number, group);
  little16(descriptor + 30, 0);
  uint32_t crc = checksum(metadata_seed(super), number, 4);
  little16(descriptor + 30, checksum(crc, descriptor, v->descriptor_size));
}
static uint32_t inode_seed(const uint8_t *super, uint32_t number,
                           const uint8_t *inode) {
  uint8_t encoded[4];
  little32(encoded, number);
  return checksum(checksum(metadata_seed(super), encoded, 4), inode + 100, 4);
}
static void inode_checksum(struct ext4_volume *v, uint32_t number,
                           uint8_t *inode, uint8_t *super) {
  if (!(u32(super + 100) & 0x400))
    return;
  little16(inode + 124, 0);
  bool high = v->inode_size >= 132 && u16(inode + 128) >= 4;
  if (high)
    little16(inode + 130, 0);
  uint32_t crc =
      checksum(inode_seed(super, number, inode), inode, v->inode_size);
  little16(inode + 124, crc);
  if (high)
    little16(inode + 130, crc >> 16);
}
static uint8_t *inode_edit(struct ext4_volume *v, uint32_t number) {
  if (!number)
    return NULL;
  unsigned group = (number - 1) / v->inodes_per_group,
           index = (number - 1) % v->inodes_per_group;
  uint8_t *descriptor = descriptor_edit(v, group);
  if (!descriptor)
    return NULL;
  uint64_t table = u32(descriptor + 8);
  if (v->descriptor_size >= 64)
    table |= (uint64_t)u32(descriptor + 40) << 32;
  uint64_t offset = table * 4096 + (uint64_t)index * v->inode_size;
  uint8_t *block = edit_block(v, offset / 4096);
  return block ? block + offset % 4096 : NULL;
}
static uint64_t allocate_block(struct ext4_volume *v) {
  uint8_t *block = edit_block(v, 0);
  if (!block)
    return 0;
  uint8_t *super = block + 1024;
  uint32_t per_group = u32(super + 32);
  if (!per_group || per_group > 32768)
    return 0;
  uint64_t groups = (v->blocks + per_group - 1) / per_group;
  for (uint32_t group = 0; group < groups; group++) {
    uint8_t *descriptor = descriptor_edit(v, group);
    if (!descriptor)
      return 0;
    uint32_t free = u16(descriptor + 12);
    if (v->descriptor_size >= 64)
      free |= (uint32_t)u16(descriptor + 44) << 16;
    if (!free || (u16(descriptor + 18) & 2))
      continue;
    uint64_t bitmap_block = u32(descriptor);
    if (v->descriptor_size >= 64)
      bitmap_block |= (uint64_t)u32(descriptor + 32) << 32;
    uint8_t *bitmap = edit_block(v, bitmap_block);
    if (!bitmap)
      return 0;
    unsigned count = per_group;
    if ((uint64_t)(group + 1) * per_group > v->blocks)
      count = v->blocks - (uint64_t)group * per_group;
    for (unsigned i = 0; i < count; i++)
      if (!(bitmap[i / 8] & (1u << (i % 8)))) {
        uint64_t physical = (uint64_t)group * per_group + i;
        if (!physical)
          continue;
        bitmap[i / 8] |= 1u << (i % 8);
        free--;
        little16(descriptor + 12, free);
        if (v->descriptor_size >= 64)
          little16(descriptor + 44, free >> 16);
        uint32_t crc = checksum(metadata_seed(super), bitmap, per_group / 8);
        little16(descriptor + 24, crc);
        if (v->descriptor_size >= 64)
          little16(descriptor + 56, crc >> 16);
        descriptor_checksum(v, group, descriptor, super);
        uint64_t available = u32(super + 12) | (uint64_t)u32(super + 344) << 32;
        if (!available)
          return 0;
        available--;
        little32(super + 12, available);
        little32(super + 344, available >> 32);
        super_checksum(super);
        uint8_t *data = edit_block(v, physical);
        if (!data)
          return 0;
        memset(data, 0, 4096);
        return physical;
      }
  }
  return 0;
}
static uint32_t allocate_inode(struct ext4_volume *v) {
  uint8_t *block = edit_block(v, 0);
  if (!block)
    return 0;
  uint8_t *super = block + 1024;
  uint32_t groups =
      (u32(super) + v->inodes_per_group - 1) / v->inodes_per_group;
  for (uint32_t group = 0; group < groups; group++) {
    uint8_t *descriptor = descriptor_edit(v, group);
    if (!descriptor)
      return 0;
    unsigned free = u16(descriptor + 14);
    if (v->descriptor_size >= 64)
      free |= (uint32_t)u16(descriptor + 46) << 16;
    if (!free || (u16(descriptor + 18) & 1))
      continue;
    uint64_t location = u32(descriptor + 4);
    if (v->descriptor_size >= 64)
      location |= (uint64_t)u32(descriptor + 36) << 32;
    uint8_t *bitmap = edit_block(v, location);
    if (!bitmap)
      return 0;
    for (unsigned i = 0; i < v->inodes_per_group; i++) {
      uint32_t number = group * v->inodes_per_group + i + 1;
      if (number < u32(super + 84) || number > u32(super) ||
          bitmap[i / 8] & (1u << (i % 8)))
        continue;
      bitmap[i / 8] |= 1u << (i % 8);
      free--;
      little16(descriptor + 14, free);
      if (v->descriptor_size >= 64)
        little16(descriptor + 46, free >> 16);
      uint32_t unused = u16(descriptor + 28);
      if (v->descriptor_size >= 64)
        unused |= (uint32_t)u16(descriptor + 50) << 16;
      if (unused > v->inodes_per_group - i - 1)
        unused = v->inodes_per_group - i - 1;
      little16(descriptor + 28, unused);
      if (v->descriptor_size >= 64)
        little16(descriptor + 50, unused >> 16);
      uint32_t crc =
          checksum(metadata_seed(super), bitmap, v->inodes_per_group / 8);
      little16(descriptor + 26, crc);
      if (v->descriptor_size >= 64)
        little16(descriptor + 58, crc >> 16);
      descriptor_checksum(v, group, descriptor, super);
      little32(super + 16, u32(super + 16) - 1);
      super_checksum(super);
      uint8_t *inode = inode_edit(v, number);
      if (!inode)
        return 0;
      memset(inode, 0, v->inode_size);
      little16(inode, 0x81a4);
      little16(inode + 26, 1);
      little32(inode + 32, 0x80000);
      little32(inode + 100, clock_ticks() ^ number);
      if (v->inode_size >= 160)
        little16(inode + 128, 32);
      little16(inode + 40, 0xf30a);
      little16(inode + 44, 4);
      inode_checksum(v, number, inode, super);
      return number;
    }
  }
  return 0;
}
static int grow_file(struct ext4_volume *, uint32_t, uint64_t, const void *,
                     uint32_t);
int ext4_write(struct ext4_volume *v, const struct ext4_inode *inode,
               uint64_t offset, const void *input, uint32_t n) {
  if ((inode->mode & 0xf000) != 0x8000 ||
      (inode->flags & (0x10 | 0x20 | 0x800)))
    return -1;
  if (offset > UINT64_MAX - n)
    return -27;
  if (offset + n > inode->size)
    return grow_file(v, inode->number, offset, input, n);
  const uint8_t *data = input;
  uint32_t done = 0;
  while (done < n) {
    if (offset / 4096 > UINT32_MAX)
      return -27;
    uint64_t physical;
    int r = extent(v, inode, offset / 4096, &physical);
    if (r)
      return r;
    if (!physical) {
      r = grow_file(v, inode->number, offset, data + done, n - done);
      return r < 0 ? r : (int)done + r;
    }
    uint8_t block[4096];
    if (bytes(v, physical * 4096, block, 4096))
      return -5;
    unsigned within = offset % 4096, count = 4096 - within;
    if (count > n - done)
      count = n - done;
    memcpy(block + within, data + done, count);
    if ((r = journal_write(v, physical, block)))
      return r;
    done += count;
    offset += count;
  }
  return done;
}
static void inode_blocks_add(uint8_t *inode, unsigned n) {
  uint64_t blocks = u32(inode + 28) | (uint64_t)u16(inode + 116) << 32;
  blocks += n;
  little32(inode + 28, blocks);
  little16(inode + 116, blocks >> 32);
}
static int add_extent(struct ext4_volume *v, uint32_t number, uint8_t *inode,
                      uint32_t logical, uint64_t physical) {
  uint8_t *root = inode + 40, *node = root;
  uint8_t *super = edit_block(v, 0);
  if (!super)
    return -12;
  super += 1024;
  unsigned depth = u16(root + 6);
  if (depth > 1 || u16(root) != 0xf30a)
    return -95;
  if (depth == 0 && u16(root + 2) == 4) {
    uint64_t leaf = allocate_block(v);
    if (!leaf)
      return -28;
    node = edit_block(v, leaf);
    if (!node)
      return -12;
    memcpy(node, root, 60);
    little16(node + 4, 340);
    memset(root, 0, 60);
    little16(root, 0xf30a);
    little16(root + 2, 1);
    little16(root + 4, 4);
    little16(root + 6, 1);
    little32(root + 12, u32(node + 12));
    little32(root + 16, leaf);
    little16(root + 20, leaf >> 32);
    inode_blocks_add(inode, 8);
    depth = 1;
  } else if (depth == 1) {
    unsigned count = u16(root + 2);
    if (!count || count > 4)
      return -5;
    unsigned index = 0;
    for (unsigned i = 0; i < count; i++)
      if (u32(root + 12 + i * 12) <= logical)
        index = i;
    uint8_t *entry = root + 12 + index * 12;
    uint64_t leaf = u32(entry + 4) | (uint64_t)u16(entry + 8) << 32;
    node = edit_block(v, leaf);
    if (!node)
      return -12;
  }
  unsigned count = u16(node + 2), maximum = depth ? 340 : 4, position = 0;
  if (u16(node) != 0xf30a || u16(node + 6) != 0 || count > maximum)
    return -5;
  while (position < count && u32(node + 12 + position * 12) < logical)
    position++;
  if (position < count && u32(node + 12 + position * 12) == logical)
    return -95;
  if (position) {
    uint8_t *previous = node + 12 + (position - 1) * 12;
    uint32_t first = u32(previous);
    unsigned length = u16(previous + 4);
    if (length > 32768) {
      if (logical - first < length - 32768)
        return -95;
    } else {
      if (logical - first < length)
        return -5;
      uint64_t start = u32(previous + 8) | (uint64_t)u16(previous + 6) << 32;
      if (logical - first == length && physical - start == length &&
          length < 32768) {
        little16(previous + 4, length + 1);
        goto finish;
      }
    }
  }
  if (count == maximum)
    return -27;
  memmove(node + 12 + (position + 1) * 12, node + 12 + position * 12,
          (count - position) * 12);
  uint8_t *entry = node + 12 + position * 12;
  little32(entry, logical);
  little16(entry + 4, 1);
  little16(entry + 6, physical >> 32);
  little32(entry + 8, physical);
  little16(node + 2, count + 1);
finish:
  if (depth) {
    if (u32(node + 12) < u32(root + 12))
      little32(root + 12, u32(node + 12));
    little32(node + 4092,
             checksum(inode_seed(super, number, inode), node, 4092));
  }
  return 0;
}
static int grow_file(struct ext4_volume *v, uint32_t number, uint64_t offset,
                     const void *input, uint32_t n) {
  if (n > 65536 || offset > UINT64_MAX - n || (offset + n) / 4096 > UINT32_MAX)
    return -27;
  bool owned = transaction_volume != v;
  if (owned)
    begin_transaction(v);
  int r = -12;
  uint8_t *inode = inode_edit(v, number), *super = edit_block(v, 0);
  if (!inode || !super)
    goto abort;
  super += 1024;
  if ((u16(inode) & 0xf000) != 0x8000 ||
      (u32(inode + 32) & (0x10 | 0x20 | 0x800 | 0x40000))) {
    r = -1;
    goto abort;
  }
  const uint8_t *data = input;
  uint32_t done = 0;
  while (done < n) {
    struct ext4_inode view = {0};
    view.flags = u32(inode + 32);
    memcpy(view.extent_root, inode + 40, 60);
    uint64_t physical;
    r = extent(v, &view, offset / 4096, &physical);
    if (r)
      goto abort;
    if (!physical) {
      physical = allocate_block(v);
      if (!physical) {
        r = -28;
        goto abort;
      }
      r = add_extent(v, number, inode, offset / 4096, physical);
      if (r)
        goto abort;
      inode_blocks_add(inode, 8);
    }
    uint8_t *block = edit_block(v, physical);
    if (!block) {
      r = -12;
      goto abort;
    }
    unsigned within = offset % 4096, count = 4096 - within;
    if (count > n - done)
      count = n - done;
    memcpy(block + within, data + done, count);
    offset += count;
    done += count;
  }
  uint64_t size = u32(inode + 4) | (uint64_t)u32(inode + 108) << 32;
  if (offset > size) {
    little32(inode + 4, offset);
    little32(inode + 108, offset >> 32);
  }
  inode_checksum(v, number, inode, super);
  r = owned ? commit_transaction(v) : 0;
  return r ? r : (int)n;
abort:
  if (owned) {
    transaction_volume = NULL;
    transaction_count = 0;
  }
  return r;
}
static uint8_t directory_copy[24 * 4096];
static int directory_add(struct ext4_volume *v, const struct ext4_inode *parent,
                         const char *name, uint32_t number) {
  if (parent->size > sizeof(directory_copy) || !parent->size ||
      (parent->size & 4095))
    return -27;
  int count = ext4_read(v, parent, 0, directory_copy, parent->size);
  if (count != (int)parent->size)
    return -5;
  uint8_t *inode = inode_edit(v, parent->number), *super = edit_block(v, 0);
  if (!inode || !super)
    return -12;
  super += 1024;
  bool checksummed = u32(super + 100) & 0x400;
  unsigned limit = checksummed ? 4084 : 4096, used = 0;
  uint8_t *output = NULL;
  uint32_t seed = inode_seed(super, parent->number, inode);
  for (uint64_t offset = 0; offset < parent->size; offset += 4096) {
    uint64_t physical;
    int r = extent(v, parent, offset / 4096, &physical);
    if (r || !physical)
      return -5;
    output = edit_block(v, physical);
    if (!output)
      return -12;
    memset(output, 0, 4096);
  }
  unsigned out_block = 0, last_entry = 0;
  uint64_t physical;
  if (extent(v, parent, 0, &physical))
    return -5;
  output = edit_block(v, physical);
  if (!output)
    return -12;
  for (unsigned offset = 0; offset < parent->size;) {
    unsigned within = offset % 4096;
    if (4096 - within < 8)
      return -5;
    uint32_t ino = u32(directory_copy + offset);
    unsigned length = u16(directory_copy + offset + 4),
             size = directory_copy[offset + 6];
    if (length < 8 || (length & 3) || length > 4096 - within ||
        size > length - 8)
      return -5;
    bool omit = size == strlen(name);
    for (unsigned i = 0; i < size && omit; i++)
      if (directory_copy[offset + 8 + i] != (uint8_t)name[i])
        omit = false;
    if (ino && size && !omit) {
      unsigned required = (8 + size + 3) & ~3u;
      if (used + required > limit) {
        little16(output + last_entry + 4, limit - last_entry);
        if (checksummed) {
          little16(output + 4088, 12);
          output[4091] = 0xde;
          little32(output + 4092, checksum(seed, output, 4084));
        }
        if (++out_block >= parent->size / 4096 ||
            extent(v, parent, out_block, &physical))
          return -27;
        output = edit_block(v, physical);
        if (!output)
          return -12;
        used = 0;
      }
      last_entry = used;
      memcpy(output + used, directory_copy + offset, 8 + size);
      little16(output + used + 4, required);
      used += required;
    }
    offset += length;
  }
  unsigned size = strlen(name), required = (8 + size + 3) & ~3u;
  if (number && used + required > limit) {
    little16(output + last_entry + 4, limit - last_entry);
    if (checksummed) {
      little16(output + 4088, 12);
      output[4091] = 0xde;
      little32(output + 4092, checksum(seed, output, 4084));
    }
    if (++out_block >= parent->size / 4096 ||
        extent(v, parent, out_block, &physical))
      return -28;
    output = edit_block(v, physical);
    if (!output)
      return -12;
    used = 0;
  }
  if (number) {
    little32(output + used, number);
    little16(output + used + 4, limit - used);
    output[used + 6] = size;
    uint8_t *added_inode = inode_edit(v, number);
    output[used + 7] =
        added_inode && (u16(added_inode) & 0xf000) == 0x4000 ? 2 : 1;
    memcpy(output + used + 8, name, size);
  } else if (used)
    little16(output + last_entry + 4, limit - last_entry);
  else
    little16(output + 4, limit);
  if (checksummed) {
    little16(output + 4088, 12);
    output[4091] = 0xde;
    little32(output + 4092, checksum(seed, output, 4084));
  }
  while (++out_block < parent->size / 4096) {
    if (extent(v, parent, out_block, &physical))
      return -5;
    output = edit_block(v, physical);
    if (!output)
      return -12;
    little16(output + 4, limit);
    if (checksummed) {
      little16(output + 4088, 12);
      output[4091] = 0xde;
      little32(output + 4092, checksum(seed, output, 4084));
    }
  }
  little32(inode + 32, u32(inode + 32) & ~0x1000u);
  inode_checksum(v, parent->number, inode, super);
  return 0;
}
static int resolve_parent(struct ext4_volume *v, const char *path,
                          struct ext4_inode *parent, char name[256]) {
  char parent_path[1024];
  unsigned length = strlen(path), split = 0;
  if (!length || length >= sizeof(parent_path))
    return -36;
  for (unsigned i = 0; i < length; i++)
    if (path[i] == '/')
      split = i + 1;
  unsigned name_length = length - split;
  if (!name_length || name_length > 255)
    return -36;
  memcpy(name, path + split, name_length);
  name[name_length] = 0;
  if (str_eq(name, ".") || str_eq(name, ".."))
    return -22;
  if (split) {
    memcpy(parent_path, path, split);
    parent_path[split] = 0;
  } else {
    parent_path[0] = '/';
    parent_path[1] = 0;
  }
  int r = ext4_lookup(v, parent_path, parent);
  if (r)
    return r;
  if ((parent->mode & 0xf000) != 0x4000)
    return -20;
  if (parent->flags & 0x10)
    return -1;
  return 0;
}
int ext4_create(struct ext4_volume *v, const char *path,
                struct ext4_inode *out) {
  struct ext4_inode existing, parent;
  char name[256];
  int r = ext4_lookup(v, path, &existing);
  if (!r)
    return -17;
  if (r != -2)
    return r;
  r = resolve_parent(v, path, &parent, name);
  if (r)
    return r;
  begin_transaction(v);
  uint32_t number = allocate_inode(v);
  if (!number)
    r = -28;
  else
    r = directory_add(v, &parent, name, number);
  if (r) {
    transaction_volume = NULL;
    transaction_count = 0;
    return r;
  }
  r = commit_transaction(v);
  return r ? r : ext4_inode_read(v, number, out);
}
static int free_block(struct ext4_volume *v, uint64_t physical) {
  uint8_t *super = edit_block(v, 0);
  if (!super)
    return -12;
  super += 1024;
  uint32_t per_group = u32(super + 32);
  if (!physical || physical >= v->blocks || !per_group)
    return -5;
  uint32_t group = physical / per_group, index = physical % per_group;
  uint8_t *descriptor = descriptor_edit(v, group);
  if (!descriptor)
    return -12;
  uint64_t location = u32(descriptor);
  if (v->descriptor_size >= 64)
    location |= (uint64_t)u32(descriptor + 32) << 32;
  uint8_t *bitmap = edit_block(v, location);
  if (!bitmap)
    return -12;
  if (!(bitmap[index / 8] & (1u << (index % 8))))
    return -5;
  bitmap[index / 8] &= ~(1u << (index % 8));
  uint32_t free = u16(descriptor + 12);
  if (v->descriptor_size >= 64)
    free |= (uint32_t)u16(descriptor + 44) << 16;
  free++;
  little16(descriptor + 12, free);
  if (v->descriptor_size >= 64)
    little16(descriptor + 44, free >> 16);
  uint32_t crc = checksum(metadata_seed(super), bitmap, per_group / 8);
  little16(descriptor + 24, crc);
  if (v->descriptor_size >= 64)
    little16(descriptor + 56, crc >> 16);
  descriptor_checksum(v, group, descriptor, super);
  uint64_t available = u32(super + 12) | (uint64_t)u32(super + 344) << 32;
  available++;
  little32(super + 12, available);
  little32(super + 344, available >> 32);
  super_checksum(super);
  return 0;
}
static uint8_t freeing_nodes[6][4096];
static int free_tree(struct ext4_volume *v, const uint8_t *node,
                     unsigned capacity, unsigned expected) {
  unsigned count = u16(node + 2), depth = u16(node + 6);
  if (u16(node) != 0xf30a || depth != expected ||
      count > (capacity - 12) / 12 || depth > 5)
    return -5;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *entry = node + 12 + i * 12;
    if (depth) {
      uint64_t block = u32(entry + 4) | (uint64_t)u16(entry + 8) << 32;
      if (block >= v->blocks ||
          bytes(v, block * 4096, freeing_nodes[depth], 4096))
        return -5;
      int r = free_tree(v, freeing_nodes[depth], 4096, depth - 1);
      if (r)
        return r;
      if ((r = free_block(v, block)))
        return r;
    } else {
      unsigned length = u16(entry + 4);
      if (length > 32768)
        length -= 32768;
      uint64_t first = u32(entry + 8) | (uint64_t)u16(entry + 6) << 32;
      if (!length || first + length > v->blocks)
        return -5;
      for (unsigned j = 0; j < length; j++) {
        int r = free_block(v, first + j);
        if (r)
          return r;
      }
    }
  }
  return 0;
}
static int release_inode(struct ext4_volume *v, uint32_t number) {
  uint8_t *inode = inode_edit(v, number), *super = edit_block(v, 0);
  if (!inode || !super)
    return -12;
  super += 1024;
  bool directory_inode = (u16(inode) & 0xf000) == 0x4000;
  if (((u16(inode) & 0xf000) != 0x8000 && !directory_inode) ||
      (u32(inode + 32) & (0x10 | 0x20 | 0x800)))
    return -1;
  unsigned links = u16(inode + 26);
  if (!links)
    return -5;
  if (links > 1 && !directory_inode) {
    little16(inode + 26, links - 1);
    inode_checksum(v, number, inode, super);
    return 0;
  }
  if (!(u32(inode + 32) & 0x80000))
    return -95;
  int r = free_tree(v, inode + 40, 60, u16(inode + 46));
  if (r)
    return r;
  uint32_t group = (number - 1) / v->inodes_per_group,
           index = (number - 1) % v->inodes_per_group;
  uint8_t *descriptor = descriptor_edit(v, group);
  if (!descriptor)
    return -12;
  uint64_t location = u32(descriptor + 4);
  if (v->descriptor_size >= 64)
    location |= (uint64_t)u32(descriptor + 36) << 32;
  uint8_t *bitmap = edit_block(v, location);
  if (!bitmap)
    return -12;
  if (!(bitmap[index / 8] & (1u << (index % 8))))
    return -5;
  bitmap[index / 8] &= ~(1u << (index % 8));
  uint32_t free = u16(descriptor + 14);
  if (v->descriptor_size >= 64)
    free |= (uint32_t)u16(descriptor + 46) << 16;
  free++;
  little16(descriptor + 14, free);
  if (v->descriptor_size >= 64)
    little16(descriptor + 46, free >> 16);
  uint32_t crc =
      checksum(metadata_seed(super), bitmap, v->inodes_per_group / 8);
  little16(descriptor + 26, crc);
  if (v->descriptor_size >= 64)
    little16(descriptor + 58, crc >> 16);
  descriptor_checksum(v, group, descriptor, super);
  little32(super + 16, u32(super + 16) + 1);
  super_checksum(super);
  memset(inode, 0, v->inode_size);
  uint32_t deletion = u32(super + 48) + clock_ticks() / 100;
  if (deletion <= u32(super))
    deletion = UINT32_MAX;
  little32(inode + 20, deletion);
  if (v->inode_size >= 160)
    little16(inode + 128, 32);
  inode_checksum(v, number, inode, super);
  return 0;
}
int ext4_replace(struct ext4_volume *v, const char *path, const void *data,
                 uint32_t size) {
  if (size > 65536)
    return -27;
  struct ext4_inode old, parent;
  char name[256];
  int found = ext4_lookup(v, path, &old);
  if (found && found != -2)
    return found;
  if (!found && (old.mode & 0xf000) != 0x8000)
    return -1;
  int r = resolve_parent(v, path, &parent, name);
  if (r)
    return r;
  begin_transaction(v);
  uint32_t number = allocate_inode(v);
  if (!number) {
    r = -28;
    goto abort_replace;
  }
  r = grow_file(v, number, 0, data, size);
  if (r < 0)
    goto abort_replace;
  r = directory_add(v, &parent, name, number);
  if (r)
    goto abort_replace;
  if (!found && (r = release_inode(v, old.number)))
    goto abort_replace;
  r = commit_transaction(v);
  return r ? r : (int)size;
abort_replace:
  transaction_volume = NULL;
  transaction_count = 0;
  return r;
}
int ext4_mkdir(struct ext4_volume *v, const char *path) {
  struct ext4_inode existing, parent;
  char name[256];
  if (ext4_lookup(v, path, &existing) == 0)
    return -17;
  int r = resolve_parent(v, path, &parent, name);
  if (r)
    return r;
  begin_transaction(v);
  uint32_t number = allocate_inode(v);
  if (!number) {
    r = -28;
    goto fail_mkdir;
  }
  uint8_t data[4096] = {0};
  uint8_t *super = edit_block(v, 0);
  if (!super) {
    r = -12;
    goto fail_mkdir;
  }
  super += 1024;
  bool tail = (u32(super + 100) & 0x400) != 0;
  unsigned limit = tail ? 4084 : 4096;
  little32(data, number);
  little16(data + 4, 12);
  data[6] = 1;
  data[7] = 2;
  data[8] = '.';
  little32(data + 12, parent.number);
  little16(data + 16, limit - 12);
  data[18] = 2;
  data[19] = 2;
  data[20] = '.';
  data[21] = '.';
  r = grow_file(v, number, 0, data, sizeof(data));
  if (r < 0)
    goto fail_mkdir;
  uint8_t *inode = inode_edit(v, number);
  if (!inode) {
    r = -12;
    goto fail_mkdir;
  }
  little16(inode, 0x41ed);
  little16(inode + 26, 2);
  inode_checksum(v, number, inode, super);
  if (tail) {
    struct ext4_inode view;
    r = ext4_inode_read(v, number, &view);
    if (r)
      goto fail_mkdir;
    uint64_t physical;
    r = extent(v, &view, 0, &physical);
    if (r || !physical) {
      r = -5;
      goto fail_mkdir;
    }
    uint8_t *block = edit_block(v, physical);
    if (!block) {
      r = -12;
      goto fail_mkdir;
    }
    little16(block + 4088, 12);
    block[4090] = 0;
    block[4091] = 0xde;
    little32(block + 4092,
             checksum(inode_seed(super, number, inode), block, 4084));
  }
  r = directory_add(v, &parent, name, number);
  if (r)
    goto fail_mkdir;
  uint8_t *parent_inode = inode_edit(v, parent.number);
  if (!parent_inode) {
    r = -12;
    goto fail_mkdir;
  }
  little16(parent_inode + 26, u16(parent_inode + 26) + 1);
  inode_checksum(v, parent.number, parent_inode, super);
  uint8_t *descriptor = descriptor_edit(v, (number - 1) / v->inodes_per_group);
  if (!descriptor) {
    r = -12;
    goto fail_mkdir;
  }
  little16(descriptor + 16, u16(descriptor + 16) + 1);
  descriptor_checksum(v, (number - 1) / v->inodes_per_group, descriptor, super);
  return commit_transaction(v);
fail_mkdir:
  transaction_volume = NULL;
  transaction_count = 0;
  return r;
}
int ext4_rename(struct ext4_volume *v, const char *old_path,
                const char *new_path) {
  struct ext4_inode old, parent, destination_parent, existing;
  char old_name[256], new_name[256];
  int r = ext4_lookup(v, old_path, &old);
  if (r)
    return r;
  if (old.flags & (0x10 | 0x20))
    return -1;
  if (!ext4_lookup(v, new_path, &existing))
    return -17;
  r = resolve_parent(v, old_path, &parent, old_name);
  if (r)
    return r;
  r = resolve_parent(v, new_path, &destination_parent, new_name);
  if (r)
    return r;
  bool moving_directory = (old.mode & 0xf000) == 0x4000 &&
                          parent.number != destination_parent.number;
  if (moving_directory) {
    /* Reject cycles by inode, including paths containing . and ... */
    struct ext4_inode ancestor = destination_parent;
    for (unsigned depth = 0;; depth++) {
      if (ancestor.number == old.number)
        return -22;
      if (ancestor.number == 2)
        break;
      if (depth == 255)
        return -40;
      int above = directory(v, &ancestor, "..");
      if (above < 0)
        return above;
      r = ext4_inode_read(v, above, &ancestor);
      if (r)
        return r;
    }
  }
  begin_transaction(v);
  r = directory_add(v, &destination_parent, new_name, old.number);
  if (!r)
    r = directory_add(v, &parent, old_name, 0);
  if (!r && moving_directory)
    r = directory_add(v, &old, "..", destination_parent.number);
  if (!r && moving_directory) {
    uint8_t *from = inode_edit(v, parent.number);
    uint8_t *to = inode_edit(v, destination_parent.number);
    uint8_t *super = edit_block(v, 0);
    if (!from || !to || !super)
      r = -12;
    else if (!u16(from + 26) || u16(to + 26) == UINT16_MAX)
      r = -31;
    else {
      super += 1024;
      little16(from + 26, u16(from + 26) - 1);
      little16(to + 26, u16(to + 26) + 1);
      inode_checksum(v, parent.number, from, super);
      inode_checksum(v, destination_parent.number, to, super);
    }
  }
  if (r) {
    transaction_volume = NULL;
    transaction_count = 0;
    return r;
  }
  return commit_transaction(v);
}
int ext4_copy(struct ext4_volume *v, const char *source, const char *target) {
  struct ext4_inode src, dst;
  int r = ext4_lookup(v, source, &src);
  if (r)
    return r;
  if ((src.mode & 0xf000) != 0x8000)
    return -95;
  r = ext4_create(v, target, &dst);
  if (r)
    return r;
  /* Bounded transactions allow copying files larger than the editor limit. */
  uint8_t buffer[4096];
  for (uint64_t offset = 0; offset < src.size;) {
    unsigned count = src.size - offset > sizeof(buffer)
                         ? sizeof(buffer) : (unsigned)(src.size - offset);
    r = ext4_read(v, &src, offset, buffer, count);
    if (r != (int)count) {
      if (r >= 0)
        r = -5;
      goto failed_copy;
    }
    r = ext4_write(v, &dst, offset, buffer, count);
    if (r != (int)count) {
      if (r >= 0)
        r = -5;
      goto failed_copy;
    }
    offset += count;
    r = ext4_inode_read(v, dst.number, &dst);
    if (r)
      goto failed_copy;
  }
  begin_transaction(v);
  uint8_t *inode = inode_edit(v, dst.number), *super = edit_block(v, 0);
  if (!inode || !super) {
    transaction_volume = NULL;
    transaction_count = 0;
    r = -12;
    goto failed_copy;
  }
  little16(inode, src.mode);
  inode_checksum(v, dst.number, inode, super + 1024);
  r = commit_transaction(v);
  if (!r)
    return 0;
failed_copy:
  ext4_remove(v, target);
  return r;
}
int ext4_remove(struct ext4_volume *v, const char *path) {
  struct ext4_inode old, parent;
  char name[256];
  int r = ext4_lookup(v, path, &old);
  if (r)
    return r;
  r = resolve_parent(v, path, &parent, name);
  if (r)
    return r;
  if ((old.mode & 0xf000) == 0x4000) {
    char entry[256];
    uint64_t entry_size;
    if (ext4_entry(v, path, 0, entry, &entry_size) == 0)
      return -39;
  }
  begin_transaction(v);
  r = directory_add(v, &parent, name, 0);
  if (!r && (old.mode & 0xf000) == 0x4000) {
    uint8_t *super = edit_block(v, 0), *inode = inode_edit(v, parent.number);
    if (!super || !inode)
      r = -12;
    else {
      super += 1024;
      little16(inode + 26, u16(inode + 26) - 1);
      inode_checksum(v, parent.number, inode, super);
    }
  }
  if (!r)
    r = release_inode(v, old.number);
  if (!r && (old.mode & 0xf000) == 0x4000) {
    uint8_t *super = edit_block(v, 0);
    uint8_t *descriptor =
        descriptor_edit(v, (old.number - 1) / v->inodes_per_group);
    if (!super || !descriptor)
      r = -12;
    else {
      super += 1024;
      little16(descriptor + 16, u16(descriptor + 16) - 1);
      descriptor_checksum(v, (old.number - 1) / v->inodes_per_group, descriptor,
                          super);
    }
  }
  if (r) {
    transaction_volume = NULL;
    transaction_count = 0;
    return r;
  }
  return commit_transaction(v);
}
int ext4_entry(struct ext4_volume *v, const char *path, unsigned index,
               char name[256], uint64_t *size) {
  struct ext4_inode parent;
  int r = ext4_lookup(v, path, &parent);
  if (r)
    return r;
  if ((parent.mode & 0xf000) != 0x4000)
    return -20;
  uint8_t block[4096];
  unsigned ordinal = 0;
  for (uint64_t offset = 0; offset < parent.size; offset += 4096) {
    int n = ext4_read(v, &parent, offset, block, 4096);
    if (n < 0)
      return n;
    for (unsigned at = 0; at < (unsigned)n;) {
      if ((unsigned)n - at < 8)
        return -5;
      unsigned length = u16(block + at + 4), count = block[at + 6];
      uint32_t number = u32(block + at);
      if (length < 8 || length & 3 || length > (unsigned)n - at ||
          count > length - 8)
        return -5;
      if (number && count) {
        memcpy(name, block + at + 8, count);
        name[count] = 0;
        if (!str_eq(name, ".") && !str_eq(name, "..") && ordinal++ == index) {
          struct ext4_inode inode;
          r = ext4_inode_read(v, number, &inode);
          if (r)
            return r;
          *size = inode.size;
          return 0;
        }
      }
      at += length;
    }
  }
  return -2;
}
