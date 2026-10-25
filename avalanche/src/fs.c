#include "system.h"
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
                   (uint8_t *)banks[bank] + k * 512))
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
                      (uint8_t *)candidate + k * 512))
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
  for (unsigned d = 0; d < disk_count(); d++)
    if (fs_mount(d) == 0)
      return;
}
int fs_size(const char *name) {
  if (mounted >= 0) {
    int i = find_record(name);
    return i < 0 ? -1 : (int)directory[i].size;
  }
  int i = find_ram(name);
  return i < 0 ? -1 : (int)ram[i].size;
}
int fs_read(const char *name, void *buffer, uint32_t capacity) {
  int size = fs_size(name);
  if (size < 0)
    return -1;
  if ((uint32_t)size > capacity)
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
int fs_remove(const char *name) {
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
