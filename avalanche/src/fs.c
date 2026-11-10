#include "sandbox.h"
#include "ext4.h"
#include "system.h"
static bool mounted_ext4;
static struct ext4_volume root_ext4;
#define SECONDARY_LIMIT 64
static struct {
  struct ext4_volume volume;
  bool mounted, writable;
} secondary[SECONDARY_LIMIT];
static const char secondary_prefix[] = ".magenta-drive-";
bool fs_secondary_path(const char *path, unsigned *disk, const char **relative) {
  while (*path == '/') path++;
  for (unsigned i = 0; secondary_prefix[i]; i++)
    if (*path++ != secondary_prefix[i]) return false;
  unsigned id = 0;
  if (*path < '0' || *path > '9') return false;
  do {
    id = id * 10 + (*path++ - '0');
    if (id >= SECONDARY_LIMIT) return false;
  } while (*path >= '0' && *path <= '9');
  if (*path && *path != '/') return false;
  *disk = id;
  if (relative) *relative = *path ? path : "/";
  return true;
}
static struct ext4_volume *volume_for(const char **path, bool write, int *error) {
  unsigned disk;
  const char *relative;
  *error = 0;
  if (!fs_secondary_path(*path, &disk, &relative))
    return mounted_ext4 ? &root_ext4 : NULL;
  if (!secondary[disk].mounted || !disk_get(disk)) {
    *error = -19;
    return NULL;
  }
  if (write && !secondary[disk].writable) {
    *error = -30;
    return NULL;
  }
  *path = relative;
  return &secondary[disk].volume;
}
#define FILE_COUNT 32u
#define DATA_START 10u
#define VOLUME_SECTORS (DATA_START + FILE_COUNT * 256u)
struct record {
  char name[32];
  uint32_t size, active, crc, used, reserved[4];
} __attribute__((packed));
struct super {
  char magic[8];
  uint32_t version, generation, count, dir_crc, header_crc;
  uint8_t reserved[484];
} __attribute__((packed));
_Static_assert(sizeof(struct record) == 64, "disk record ABI");
_Static_assert(sizeof(struct super) == 512, "disk superblock ABI");
static struct record directory[FILE_COUNT], candidate[FILE_COUNT],
    banks[2][FILE_COUNT];
static uint8_t sector[512] __attribute__((aligned(4)));
static struct super headers[2];
static int mounted = -1;
static uint32_t base_lba, generation, current_bank;
struct ram_file {
  char name[32];
  void *pages[16];
  uint32_t size;
  bool exists;
};
static struct ram_file ram[FILE_COUNT];
static bool valid_name(const char *s) {
  size_t n = strlen(s);
  if (!n || n > 31)
    return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return false;
  }
  return !str_eq(s, ".") && !str_eq(s, "..");
}
static int find_record(const char *s) {
  for (unsigned i = 0; i < FILE_COUNT; i++)
    if (directory[i].used && str_eq(directory[i].name, s))
      return (int)i;
  return -1;
}
static int find_ram(const char *s) {
  for (unsigned i = 0; i < FILE_COUNT; i++)
    if (ram[i].exists && str_eq(ram[i].name, s))
      return (int)i;
  return -1;
}
static bool get_partition(unsigned d, uint32_t *start) {
  if (!disk_read(d, 0, sector) || sector[510] != 0x55 || sector[511] != 0xaa)
    return false;
  const struct block_device *dev = disk_get(d);
  for (unsigned i = 0; i < 4; i++) {
    const uint8_t *p = sector + 446 + i * 16;
    if (p[4] != 0x7f)
      continue;
    uint32_t s, n;
    memcpy(&s, p + 8, 4);
    memcpy(&n, p + 12, 4);
    if (s >= 1 && n >= VOLUME_SECTORS && s < dev->sectors &&
        n <= dev->sectors - s) {
      *start = s;
      return true;
    }
  }
  return false;
}
static bool header_valid(struct super *s) {
  uint32_t saved = s->header_crc;
  s->header_crc = 0;
  uint32_t actual = crc32(s, sizeof(*s));
  s->header_crc = saved;
  return !__builtin_memcmp(s->magic, "AVLFS02", 8) && s->version == 2 &&
         s->count == FILE_COUNT && actual == saved;
}
static bool bank_valid(unsigned d, unsigned bank) {
  struct super *s = &headers[bank];
  if (!disk_read(d, base_lba + bank * 5, s) || !header_valid(s))
    return false;
  for (unsigned k = 0; k < 4; k++)
    if (!disk_read(d, base_lba + bank * 5 + 1 + k,
                   (uint8_t *)(uintptr_t)banks[bank] + k * 512))
      return false;
  if (crc32(banks[bank], sizeof(directory)) != s->dir_crc)
    return false;
  for (unsigned i = 0; i < FILE_COUNT; i++) {
    struct record *r = &banks[bank][i];
    if (r->used > 1 || r->active > 1 || r->size > FS_MAX_FILE)
      return false;
    if (r->used) {
      if (!__builtin_memchr(r->name, 0, sizeof(r->name)) ||
          !valid_name(r->name))
        return false;
      for (unsigned j = 0; j < i; j++)
        if (banks[bank][j].used && str_eq(r->name, banks[bank][j].name))
          return false;
    }
  }
  return true;
}
int fs_mount(unsigned disk) {
  if (disk < SECONDARY_LIMIT && secondary[disk].mounted)
    return -16;
  struct ext4_volume candidate;
  if (!ext4_discover(&candidate, disk)) {
    root_ext4 = candidate;
    mounted_ext4 = true;
    mounted = disk;
    return 0;
  }
  uint32_t start;
  if (disk >= disk_count() || !get_partition(disk, &start))
    return -1;
  uint32_t oldbase = base_lba;
  base_lba = start;
  bool a = bank_valid(disk, 0), b = bank_valid(disk, 1);
  if (!a && !b) {
    base_lba = oldbase;
    return -2;
  }
  current_bank =
      (!a ||
       (b && (int32_t)(headers[1].generation - headers[0].generation) > 0))
          ? 1
          : 0;
  generation = headers[current_bank].generation;
  memcpy(directory, banks[current_bank], sizeof(directory));
  mounted = (int)disk;
  mounted_ext4 = false;
  return 0;
}
static bool commit(const struct record *records) {
  unsigned next = current_bank ^ 1u;
  for (unsigned k = 0; k < 4; k++)
    if (!disk_write((unsigned)mounted, base_lba + next * 5 + 1 + k,
                    (const uint8_t *)records + k * 512))
      return false;
  if (!disk_flush((unsigned)mounted))
    return false;
  struct super s;
  memset(&s, 0, sizeof(s));
  memcpy(s.magic, "AVLFS02", 8);
  s.version = 2;
  s.generation = generation + 1;
  s.count = FILE_COUNT;
  s.dir_crc = crc32(records, sizeof(directory));
  s.header_crc = crc32(&s, sizeof(s));
  if (!disk_write((unsigned)mounted, base_lba + next * 5, &s) ||
      !disk_flush((unsigned)mounted))
    return false;
  memcpy(directory, records, sizeof(directory));
  generation = s.generation;
  current_bank = next;
  return true;
}
int fs_format(unsigned disk) {
  if (disk < SECONDARY_LIMIT && secondary[disk].mounted)
    return -16;
  const struct block_device *dev = disk_get(disk);
  if (!dev || dev->sectors < 2048 + VOLUME_SECTORS)
    return -1;
  memset(sector, 0, sizeof(sector));
  sector[446 + 4] = 0x7f;
  uint32_t start = 2048, length = dev->sectors - start;
  memcpy(sector + 446 + 8, &start, 4);
  memcpy(sector + 446 + 12, &length, 4);
  sector[510] = 0x55;
  sector[511] = 0xaa;
  if (!disk_write(disk, 0, sector) || !disk_flush(disk))
    return -2;
  memset(candidate, 0, sizeof(candidate));
  for (unsigned bank = 0; bank < 2; bank++) {
    for (unsigned k = 0; k < 4; k++)
      if (!disk_write(disk, start + bank * 5 + 1 + k,
                      (uint8_t *)(uintptr_t)candidate + k * 512))
        return -2;
    struct super s;
    memset(&s, 0, sizeof(s));
    memcpy(s.magic, "AVLFS02", 8);
    s.version = 2;
    s.count = FILE_COUNT;
    s.generation = bank ? 0 : 1;
    s.dir_crc = crc32(candidate, sizeof(candidate));
    s.header_crc = crc32(&s, sizeof(s));
    if (!disk_flush(disk) || !disk_write(disk, start + bank * 5, &s) ||
        !disk_flush(disk))
      return -2;
  }
  return fs_mount(disk);
}
void fs_init(void) {
  for (unsigned d = 0; d < disk_count(); d++) {
    struct ext4_volume volume;
    if (!ext4_discover(&volume, d)) {
      root_ext4 = volume;
      mounted_ext4 = true;
      mounted = d;
      return;
    }
  }
  for (unsigned d = 0; d < disk_count(); d++)
    if (fs_mount(d) == 0)
      return;
}
int fs_size(const char *name) {
  int error;
  struct ext4_volume *volume = volume_for(&name, false, &error);
  if (error) return error;
  if (volume) {
    struct ext4_inode inode;
    int r = ext4_lookup(volume, name, &inode);
    return r ? r : inode.size > INT32_MAX ? -27 : (int)inode.size;
  }
  if (!mounted_ext4)
    while (*name == '/')
      name++;
  if (mounted >= 0) {
    int i = find_record(name);
    return i < 0 ? -1 : (int)directory[i].size;
  }
  int i = find_ram(name);
  return i < 0 ? -1 : (int)ram[i].size;
}
int fs_metadata(const char *name, struct file_metadata *out) {
  memset(out, 0, sizeof(*out));
  int error;
  struct ext4_volume *volume = volume_for(&name, false, &error);
  if (error) return error;
  if (volume) {
    struct ext4_inode inode;
    int r = ext4_lookup(volume, name, &inode);
    if (r) return r;
    *out = (struct file_metadata){inode.size, inode.blocks, inode.number,
      inode.mode, inode.uid, inode.gid, inode.links, inode.atime, inode.mtime,
      inode.ctime};
    return 0;
  }
  while (*name == '/') name++;
  if (!*name || str_eq(name, ".")) {
    out->inode = 1;
    out->mode = 0040755;
    out->links = 2;
    return 0;
  }
  int i = mounted >= 0 ? find_record(name) : find_ram(name);
  if (i < 0) return -2;
  out->size = mounted >= 0 ? directory[i].size : ram[i].size;
  out->blocks = (out->size + 511) / 512;
  out->inode = i + 2;
  out->mode = 0100644;
  out->links = 1;
  return 0;
}
int fs_read_at(const char *name,uint64_t offset,void *buffer,uint32_t capacity) {
  int error;
  struct ext4_volume *volume = volume_for(&name, false, &error);
  if (error) return error;
  if (volume) {
    struct ext4_inode inode;
    int r=ext4_lookup(volume,name,&inode);
    if (r) return r;
    if ((inode.mode&0xf000)!=0x8000) return -21;
    if (offset>=inode.size) return 0;
    if (capacity>inode.size-offset) capacity=inode.size-offset;
    return ext4_read(volume,&inode,offset,buffer,capacity);
  }
  static uint8_t data[FS_MAX_FILE];
  int count=fs_read(name,data,sizeof(data));
  if (count<0) return count;
  if (offset>=(unsigned)count) return 0;
  unsigned length=count-offset;
  if (length>capacity) length=capacity;
  memcpy(buffer,data+offset,length);
  return length;
}
int fs_read(const char *name, void *buffer, uint32_t capacity) {
  int error;
  struct ext4_volume *volume = volume_for(&name, false, &error);
  if (error) return error;
  if (volume) {
    struct ext4_inode inode;
    int r = ext4_lookup(volume, name, &inode);
    if (r)
      return r;
    if ((inode.mode & 0xf000) != 0x8000)
      return -21;
    if (inode.size > capacity)
      return -27;
    return ext4_read(volume, &inode, 0, buffer, inode.size);
  }
  while (*name == '/') name++;
  int size = fs_size(name);
  if (size < 0)
    return -1;
  if ((uintptr_t)size > capacity)
    return -2;
  uint8_t *out = buffer;
  if (mounted < 0) {
    struct ram_file *r = &ram[find_ram(name)];
    for (uint32_t off = 0; off < r->size;) {
      uint32_t n = r->size - off;
      if (n > 4096)
        n = 4096;
      memcpy(out + off, r->pages[off / 4096], n);
      off += n;
    }
    return size;
  }
  int i = find_record(name);
  const struct record *r = &directory[i];
  uint32_t lba = base_lba + DATA_START + (unsigned)i * 256 + r->active * 128;
  for (uint32_t off = 0; off < r->size; off += 512) {
    if (!disk_read((unsigned)mounted, lba + off / 512, sector))
      return -3;
    uint32_t n = r->size - off;
    if (n > 512)
      n = 512;
    memcpy(out + off, sector, n);
  }
  if (crc32(out, r->size) != r->crc)
    return -4;
  return size;
}
int fs_write(const char *name, const void *data, uint32_t size) {
  if (!sandbox_write_allowed(name)) return -13;
  int error;
  struct ext4_volume *volume = volume_for(&name, true, &error);
  if (error) return error;
  if (volume) return ext4_replace(volume, name, data, size);
  while (*name == '/') name++;
  if (!valid_name(name) || size > FS_MAX_FILE)
    return -1;
  if (mounted < 0) {
    int i = find_ram(name);
    if (i < 0)
      for (unsigned k = 0; k < FILE_COUNT; k++)
        if (!ram[k].exists) {
          i = (int)k;
          break;
        }
    if (i < 0)
      return -2;
    void *pages[16] = {0};
    uint32_t count = (size + 4095) / 4096;
    for (uint32_t k = 0; k < count; k++) {
      pages[k] = page_alloc();
      if (!pages[k]) {
        for (uint32_t j = 0; j < k; j++)
          page_free(pages[j]);
        return -3;
      }
      uint32_t n = size - k * 4096;
      if (n > 4096)
        n = 4096;
      memcpy(pages[k], (const uint8_t *)data + k * 4096, n);
    }
    struct ram_file *r = &ram[i];
    for (unsigned k = 0; k < 16; k++)
      if (r->pages[k])
        page_free(r->pages[k]);
    memset(r, 0, sizeof(*r));
    memcpy(r->name, name, strlen(name) + 1);
    memcpy(r->pages, pages, sizeof(pages));
    r->size = size;
    r->exists = true;
    return (int)size;
  }
  int i = find_record(name);
  if (i < 0)
    for (unsigned k = 0; k < FILE_COUNT; k++)
      if (!directory[k].used) {
        i = (int)k;
        break;
      }
  if (i < 0)
    return -2;
  memcpy(candidate, directory, sizeof(candidate));
  struct record *r = &candidate[i];
  unsigned active = r->used ? (r->active ^ 1u) : 0;
  uint32_t lba = base_lba + DATA_START + (unsigned)i * 256 + active * 128;
  for (uint32_t off = 0; off < size; off += 512) {
    memset(sector, 0, 512);
    uint32_t n = size - off;
    if (n > 512)
      n = 512;
    memcpy(sector, (const uint8_t *)data + off, n);
    if (!disk_write((unsigned)mounted, lba + off / 512, sector))
      return -3;
  }
  if (!disk_flush((unsigned)mounted))
    return -3;
  memset(r, 0, sizeof(*r));
  memcpy(r->name, name, strlen(name) + 1);
  r->size = size;
  r->active = active;
  r->used = 1;
  r->crc = crc32(data, size);
  return commit(candidate) ? (int)size : -3;
}
int fs_write_new_stream(const char *path, const void *data, uint32_t size) {
  if (!sandbox_write_allowed(path)) return -13;
  const char *relative = path;
  int error;
  struct ext4_volume *volume = volume_for(&relative, true, &error);
  if (error) return error;
  struct file_metadata existing;
  int status = fs_metadata(path, &existing);
  if (status != -2) return status ? status : -17;
  if (size <= FS_MAX_FILE || !volume) return fs_write(path, data, size);
  struct ext4_inode inode;
  int result = ext4_create(volume, relative, &inode);
  if (result < 0) return result;
  const uint8_t *bytes = data;
  for (uint32_t offset = 0; offset < size;) {
    unsigned count = size - offset > 4096 ? 4096 : size - offset;
    result = ext4_write(volume, &inode, offset, bytes + offset, count);
    if (result != (int)count) return result < 0 ? result : -5;
    offset += count;
    result = ext4_inode_read(volume, inode.number, &inode);
    if (result < 0) return result;
  }
  return size;
}
int fs_mkdir(const char *path) {
  if (!sandbox_write_allowed(path)) return -13;
  int error;
  struct ext4_volume *volume = volume_for(&path, true, &error);
  return volume ? ext4_mkdir(volume, path) : error ? error : -30;
}
int fs_copy(const char *source, const char *target) {
  if (!sandbox_has(process_current(), AV_CAP_READ) || !sandbox_write_allowed(target)) return -13;
  const char *from = source, *to = target;
  int error;
  struct ext4_volume *src = volume_for(&from, false, &error);
  if (error) return error;
  struct ext4_volume *dst = volume_for(&to, true, &error);
  if (error) return error;
  if (src && dst) return ext4_copy_between(src, from, dst, to);
  if (fs_size(target) >= 0)
    return -17;
  static uint8_t copy_buffer[FS_MAX_FILE];
  int n = fs_read(source, copy_buffer, sizeof(copy_buffer));
  if (n < 0)
    return n;
  int r = fs_write(target, copy_buffer, n);
  return r < 0 ? r : 0;
}
int fs_rename(const char *old_path, const char *new_path) {
  if (!sandbox_write_allowed(old_path) || !sandbox_write_allowed(new_path)) return -13;
  int error;
  struct ext4_volume *src = volume_for(&old_path, true, &error);
  if (error) return error;
  struct ext4_volume *dst = volume_for(&new_path, true, &error);
  if (error) return error;
  if (src != dst) return -18;
  if (src) return ext4_rename(src, old_path, new_path);
  static char rename_buffer[FS_MAX_FILE];
  int n = fs_read(old_path, rename_buffer, sizeof(rename_buffer));
  if (n < 0 || fs_size(new_path) >= 0)
    return -1;
  if (fs_write(new_path, rename_buffer, n) < 0)
    return -1;
  return fs_remove(old_path);
}
int fs_remove(const char *name) {
  if (!sandbox_write_allowed(name)) return -13;
  int error;
  struct ext4_volume *volume = volume_for(&name, true, &error);
  if (error) return error;
  if (volume) return ext4_remove(volume, name);
  while (*name == '/') name++;
  if (mounted < 0) {
    int i = find_ram(name);
    if (i < 0)
      return -1;
    for (unsigned k = 0; k < 16; k++)
      if (ram[i].pages[k])
        page_free(ram[i].pages[k]);
    memset(&ram[i], 0, sizeof(ram[i]));
    return 0;
  }
  int i = find_record(name);
  if (i < 0)
    return -1;
  memcpy(candidate, directory, sizeof(candidate));
  memset(&candidate[i], 0, sizeof(candidate[i]));
  return commit(candidate) ? 0 : -3;
}
void fs_info(void) {
  if (mounted_ext4) {
    console_write("ext4 root filesystem on disk ");
    print_uint(root_ext4.disk);
    console_write(" at sector ");
    print_uint(root_ext4.start);
    console_write(
        "; 4096-byte blocks, extents and persistent JBD2 transactions.\n");
    return;
  }
  if (mounted < 0)
    console_write("RAM filesystem (volatile). No Avalanche disk mounted.\n");
  else {
    console_write("Avalanche FS v2 on disk ");
    print_uint((unsigned)mounted);
    console_write(" generation ");
    print_uint(generation);
    console_write("; dual metadata banks + CRC32, copy-on-write file slots.\n");
  }
  console_write("32 flat files, 65536 bytes per file.\n");
}
void fs_list(void) {
  if (mounted_ext4) {
    ext4_list(&root_ext4, "/");
    return;
  }
  unsigned count = 0;
  for (unsigned i = 0; i < FILE_COUNT; i++) {
    const char *name;
    uint32_t n;
    if (mounted < 0) {
      if (!ram[i].exists)
        continue;
      name = ram[i].name;
      n = ram[i].size;
    } else {
      if (!directory[i].used)
        continue;
      name = directory[i].name;
      n = directory[i].size;
    }
    console_write(name);
    console_write("  ");
    print_uint(n);
    console_write(" bytes\n");
    count++;
  }
  if (!count)
    console_write("No files.\n");
}
bool fs_entry(unsigned i, char *name, uint32_t *size) {
  if (mounted_ext4) {
    char full[256];
    uint64_t length;
    if (ext4_entry(&root_ext4, "/", i, full, &length) || strlen(full) > 31)
      return false;
    memcpy(name, full, strlen(full) + 1);
    *size = length > UINT32_MAX ? UINT32_MAX : length;
    return true;
  }
  if (i >= FILE_COUNT)
    return false;
  if (mounted < 0) {
    if (!ram[i].exists)
      return false;
    memcpy(name, ram[i].name, 32);
    *size = ram[i].size;
  } else {
    if (!directory[i].used)
      return false;
    memcpy(name, directory[i].name, 32);
    *size = directory[i].size;
  }
  return true;
}

int fs_mounted_disk(void) { return mounted; }
int fs_secondary_state(unsigned disk) {
  if (disk >= SECONDARY_LIMIT || !disk_get(disk)) return -19;
  if ((int)disk == mounted) return -16;
  return secondary[disk].mounted ? secondary[disk].writable ? 2 : 1 : 0;
}
int fs_secondary_mount(unsigned disk, bool writable) {
  int state = fs_secondary_state(disk);
  if (state < 0) return state;
  if (writable && disk_get(disk)->read_only) return -30;
  if (!state) {
    int result = ext4_discover_clean(&secondary[disk].volume, disk);
    if (result < 0) return result;
    secondary[disk].mounted = true;
  }
  secondary[disk].writable = secondary[disk].writable || writable;
  return 0;
}
int fs_secondary_eject(unsigned disk) {
  int state = fs_secondary_state(disk);
  if (state <= 0) return state ? state : -19;
  if (linux_disk_busy(disk)) return -16;
  if (secondary[disk].writable && !disk_flush(disk)) return -5;
  secondary[disk].mounted = secondary[disk].writable = false;
  disk_cache_forget(disk);
  return 0;
}
void fs_secondary_detach(unsigned disk) {
  if (disk < SECONDARY_LIMIT)
    secondary[disk].mounted = secondary[disk].writable = false;
}
bool fs_writable(void) {
  const struct block_device *d =
      mounted < 0 ? NULL : disk_get((unsigned)mounted);
  return mounted < 0 || (d && !d->read_only);
}
int fs_directory(const char *path) {
  int error;
  struct ext4_volume *volume = volume_for(&path, false, &error);
  if (error) return error;
  if (!volume)
    return str_eq(path, "/") ? 0 : -2;
  struct ext4_inode inode;
  int r = ext4_lookup(volume, path, &inode);
  return r ? r : ((inode.mode & 0xf000) == 0x4000 ? 0 : -20);
}
int fs_browse(const char *path, unsigned index, char name[256], uint64_t *size,
              bool *directory) {
  int error;
  struct ext4_volume *volume = volume_for(&path, false, &error);
  if (error) return error;
  if (volume) {
    int r = ext4_entry(volume, path, index, name, size);
    if (r)
      return r;
    char full[1024];
    unsigned length = strlen(path), count = strlen(name);
    if (length + count + 2 > sizeof(full))
      return -36;
    memcpy(full, path, length);
    if (!length || full[length - 1] != '/')
      full[length++] = '/';
    memcpy(full + length, name, count + 1);
    struct ext4_inode inode;
    r = ext4_lookup(volume, full, &inode);
    if (r)
      return r;
    *directory = (inode.mode & 0xf000) == 0x4000;
    return 0;
  }
  if (!str_eq(path, "/"))
    return -2;
  for (unsigned slot = 0, ordinal = 0; slot < FILE_COUNT; slot++) {
    uint32_t length;
    char entry[32];
    if (fs_entry(slot, entry, &length) && ordinal++ == index) {
      memcpy(name, entry, strlen(entry) + 1);
      *size = length;
      *directory = false;
      return 0;
    }
  }
  return -2;
}

int fs_appdata_prepare(const char *path) {
  if (!mounted_ext4) return -30;
  if (strlen(path) != 73 || __builtin_memcmp(path, "/appdata/", 9)) return -22;
  for (unsigned i=9;i<73;i++) if (!((path[i]>='0' && path[i]<='9') || (path[i]>='a' && path[i]<='f'))) return -22;
  const char *directories[] = {"/appdata", path};
  for (unsigned i=0;i<2;i++) {
    struct ext4_inode inode;
    int result=ext4_lookup(&root_ext4,directories[i],&inode);
    if (!result) { if ((inode.mode&0xf000)!=0x4000) return -20; }
    else { result=ext4_mkdir(&root_ext4,directories[i]); if(result<0)return result; }
  }
  return 0;
}
