#include "ext4.h"
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
static int bytes(struct ext4_volume *v, uint64_t offset, void *out, uint32_t n) {
  uint8_t sector[512], *p = out;
  while (n) {
    uint64_t lba = v->start + offset / 512;
    if (lba > UINT32_MAX || !disk_read(v->disk, lba, sector)) return -5;
    unsigned within = offset % 512, count = 512 - within;
    if (count > n) count = n;
    memcpy(p, sector + within, count);
    p += count; offset += count; n -= count;
  }
  return 0;
}
int ext4_mount(struct ext4_volume *v, unsigned disk, uint32_t start) {
  uint8_t super[1024];
  memset(v, 0, sizeof(*v)); v->disk = disk; v->start = start;
  if (bytes(v, 1024, super, sizeof(super))) return -5;
  if (u16(super+56) != 0xef53 || u32(super+24) != 2) return -22;
  uint32_t incompatible = u32(super+96);
  if (u16(super+58) != 1) return -117;
  /* Refuse dirty journals and layouts requiring a different block translator. */
  if (incompatible & ~(2u | 0x40u | 0x80u | 0x200u | 0x2000u)) return -95;
  v->blocks = u32(super+4);
  if (incompatible & 0x80) v->blocks |= (uint64_t)u32(super+0x150) << 32;
  v->inodes_per_group = u32(super+40);
  v->inode_size = u16(super+88);
  v->descriptor_size = incompatible & 0x80 ? u16(super+254) : 32;
  const struct block_device *d = disk_get(disk);
  if (!d || start >= d->sectors || !v->blocks || v->blocks > (d->sectors-(uint64_t)start)/8 ||
      !v->inodes_per_group || v->inode_size < 128 || v->inode_size > 4096 ||
      4096 % v->inode_size || v->descriptor_size < 32 || v->descriptor_size > 4096 ||
      4096 % v->descriptor_size) return -22;
  return 0;
}
int ext4_inode_read(struct ext4_volume *v, uint32_t ino, struct ext4_inode *out) {
  if (!ino || !v->inodes_per_group) return -22;
  uint8_t descriptor[64], inode[128];
  uint32_t group = (ino-1)/v->inodes_per_group, index = (ino-1)%v->inodes_per_group;
  if (bytes(v, 4096+(uint64_t)group*v->descriptor_size, descriptor,
            v->descriptor_size >= 64 ? 64 : 32)) return -5;
  uint64_t table = u32(descriptor+8);
  if (v->descriptor_size >= 64) table |= (uint64_t)u32(descriptor+40) << 32;
  if (!table || table >= v->blocks || bytes(v, table*4096+(uint64_t)index*v->inode_size, inode, 128)) return -5;
  out->mode = u16(inode); out->flags = u32(inode+32);
  out->size = u32(inode+4) | (uint64_t)u32(inode+108) << 32;
  memcpy(out->extent_root, inode+40, 60);
  return 0;
}
static int extent(struct ext4_volume *v, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical) {
  if (!(inode->flags & 0x80000)) return -95;
  uint8_t node[4096]; memcpy(node, inode->extent_root, 60);
  unsigned capacity = 60, expected = u16(node+6);
  if (expected > 5) return -5;
  for (;;) {
    unsigned count = u16(node+2), depth = u16(node+6);
    if (u16(node) != 0xf30a || depth != expected || count > (capacity-12)/12) return -5;
    const uint8_t *found = NULL;
    uint32_t previous = 0;
    for (unsigned i=0;i<count;i++) {
      const uint8_t *e = node+12+i*12;
      uint32_t first = u32(e);
      if (i && first <= previous) return -5;
      previous = first;
      if (first <= logical) found = e;
    }
    *physical = 0;
    if (!found) return 0;
    if (!depth) {
      unsigned length = u16(found+4);
      bool unwritten = length > 32768;
      if (unwritten) length -= 32768;
      uint32_t delta = logical-u32(found);
      if (delta >= length || unwritten) return 0;
      *physical = (u32(found+8) | (uint64_t)u16(found+6)<<32) + delta;
      return *physical < v->blocks ? 0 : -5;
    }
    uint64_t child = u32(found+4) | (uint64_t)u16(found+8)<<32;
    if (!child || child >= v->blocks || bytes(v, child*4096, node, sizeof(node))) return -5;
    capacity = sizeof(node); expected--;
  }
}
int ext4_read(struct ext4_volume *v, const struct ext4_inode *inode, uint64_t offset, void *out, uint32_t n) {
  uint8_t *p = out; uint32_t done = 0;
  if (offset >= inode->size) return 0;
  if (n > inode->size-offset) n = inode->size-offset;
  while (done < n) {
    if (offset/4096 > UINT32_MAX) return -27;
    uint64_t physical;
    int r = extent(v, inode, offset/4096, &physical);
    if (r) return r;
    unsigned within = offset%4096, count = 4096-within;
    if (count > n-done) count = n-done;
    if (!physical) memset(p+done, 0, count);
    else if (bytes(v, physical*4096+within, p+done, count)) return -5;
    done += count; offset += count;
  }
  return done;
}
static int directory(struct ext4_volume *v, const struct ext4_inode *inode, const char *name) {
  uint8_t block[4096];
  if ((inode->mode & 0xf000) != 0x4000) return -20;
  for (uint64_t offset=0;offset<inode->size;offset+=4096) {
    int n = ext4_read(v, inode, offset, block, sizeof(block));
    if (n < 0) return n;
    for (unsigned p=0;p<(unsigned)n;) {
      if ((unsigned)n-p < 8) return -5;
      uint32_t ino = u32(block+p); unsigned len = u16(block+p+4), size = block[p+6];
      if (len < 8 || (len & 3) || len > (unsigned)n-p || size > len-8) return -5;
      if (ino && size) {
        char entry[256]; memcpy(entry,block+p+8,size); entry[size]=0;
        if (name && str_eq(name,entry)) return ino;
        if (!name) { console_write(entry); console_putc('\n'); }
      }
      p += len;
    }
  }
  return name ? -2 : 0;
}
int ext4_lookup(struct ext4_volume *v, const char *path, struct ext4_inode *inode) {
  int r = ext4_inode_read(v, 2, inode);
  while (!r && *path) {
    while (*path=='/') path++;
    if (!*path) break;
    char name[256]; unsigned n=0;
    while (*path && *path!='/') { if (n==255) return -36; name[n++]=*path++; }
    name[n]=0;
    int ino = directory(v,inode,name);
    if (ino < 0) return ino;
    r = ext4_inode_read(v,ino,inode);
  }
  return r;
}
int ext4_list(struct ext4_volume *v, const char *path) {
  struct ext4_inode inode;
  int r = ext4_lookup(v,path,&inode);
  return r ? r : directory(v,&inode,NULL);
}
