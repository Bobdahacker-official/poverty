#include "partition.h"
/* GPT uses the device's native logical sectors; the block API uses 512 bytes.
 */
static uint8_t header[4096], sector[512];
static struct {
  uint64_t first, last;
} ranges[4096];
static uint32_t le32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) {
  return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static uint32_t crc_step(uint32_t crc, const uint8_t *p, unsigned n) {
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
  }
  return crc;
}
static bool read_bytes(unsigned disk, uint64_t offset, void *data, unsigned n) {
  const struct block_device *device = disk_get(disk);
  uint64_t limit = device ? (uint64_t)device->sectors * 512 : 0;
  if (offset > limit || n > limit - offset)
    return false;
  uint8_t *out = data;
  while (n) {
    if (offset / 512 > UINT32_MAX || !disk_read(disk, offset / 512, sector))
      return false;
    unsigned within = offset % 512, count = 512 - within;
    if (count > n)
      count = n;
    memcpy(out, sector + within, count);
    offset += count;
    out += count;
    n -= count;
  }
  return true;
}
static int gpt(unsigned disk, uint64_t at, unsigned bytes, unsigned ratio,
               struct disk_partition *out, unsigned capacity) {
  const struct block_device *device = disk_get(disk);
  uint64_t total = device->sectors / ratio;
  if (!read_bytes(disk, at * bytes, header, bytes) ||
      __builtin_memcmp(header, "EFI PART", 8))
    return -22;
  unsigned size = le32(header + 12);
  if (le32(header + 8) != 0x10000 || size < 92 || size > bytes ||
      le32(header + 20) || le64(header + 24) != at ||
      le64(header + 32) != (at == 1 ? total - 1 : 1))
    return -22;
  uint32_t expected = le32(header + 16);
  memset(header + 16, 0, 4);
  if (~crc_step(~0u, header, size) != expected)
    return -117;
  uint64_t first = le64(header + 40), last = le64(header + 48),
           table = le64(header + 72);
  unsigned count = le32(header + 80), entry_size = le32(header + 84);
  expected = le32(header + 88);
  if (!count || count > 4096 || entry_size < 128 || entry_size > 4096 ||
      (entry_size & (entry_size - 1)) || first < 2 || last < first ||
      last >= total - 1)
    return -22;
  uint64_t length = (uint64_t)count * entry_size,
           table_blocks = (length + bytes - 1) / bytes;
  if (table < 2 || table >= total || table_blocks > total - table ||
      (table <= last && table + table_blocks > first) ||
      (at >= table && at < table + table_blocks))
    return -22;
  uint32_t crc = ~0u;
  for (uint64_t offset = 0; offset < length;) {
    unsigned n = length - offset > 512 ? 512 : length - offset;
    uint8_t block[512];
    if (!read_bytes(disk, table * bytes + offset, block, n))
      return -5;
    crc = crc_step(crc, block, n);
    offset += n;
  }
  if (~crc != expected)
    return -117;
  static const uint8_t linux_type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84,
                                         0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69,
                                         0xd8, 0x47, 0x7d, 0xe4};
  unsigned used = 0, found = 0;
  for (unsigned i = 0; i < count; i++) {
    uint8_t entry[128];
    if (!read_bytes(disk, table * bytes + (uint64_t)i * entry_size, entry,
                    sizeof(entry)))
      return -5;
    uint8_t present = 0;
    for (unsigned j = 0; j < 16; j++)
      present |= entry[j];
    if (!present)
      continue;
    uint64_t a = le64(entry + 32), b = le64(entry + 40);
    if (a < first || b < a || b > last)
      return -22;
    for (unsigned j = 0; j < used; j++)
      if (a <= ranges[j].last && ranges[j].first <= b)
        return -22;
    ranges[used].first = a;
    ranges[used++].last = b;
    if (!__builtin_memcmp(entry, linux_type, 16) &&
        !(le64(entry + 48) & (UINT64_C(1) << 60))) {
      uint64_t start = a * ratio, sectors = (b - a + 1) * ratio;
      if (start > UINT32_MAX || sectors > UINT32_MAX ||
          sectors > device->sectors - start)
        return -22;
      if (found < capacity)
        out[found++] = (struct disk_partition){start, sectors};
    }
  }
  return found;
}
int partition_linux(unsigned disk, struct disk_partition *out,
                    unsigned capacity) {
  const struct block_device *device = disk_get(disk);
  if (!device || !capacity || device->sectors < 4)
    return -22;
  unsigned bytes =
      device->logical_sector_size ? device->logical_sector_size : 512;
  if (bytes < 512 || bytes > 4096 || (bytes & (bytes - 1)))
    return -95;
  unsigned ratio = bytes / 512;
  if (device->sectors < 4 * ratio)
    return -22;
  int result = gpt(disk, 1, bytes, ratio, out, capacity);
  if (result >= 0)
    return result;
  result = gpt(disk, device->sectors / ratio - 1, bytes, ratio, out, capacity);
  if (result >= 0)
    return result;
  uint8_t mbr[512];
  if (!read_bytes(disk, 0, mbr, sizeof(mbr)))
    return -5;
  if (mbr[510] != 0x55 || mbr[511] != 0xaa)
    return -22;
  unsigned used = 0, found = 0;
  for (unsigned i = 0; i < 4; i++) {
    const uint8_t *p = mbr + 446 + i * 16;
    if (!p[4])
      continue;
    if (p[4] == 0xee)
      return -117; /* Never reinterpret a damaged protective MBR. */
    uint64_t start = (uint64_t)le32(p + 8) * ratio,
             sectors = (uint64_t)le32(p + 12) * ratio;
    if (!start || !sectors || start >= device->sectors ||
        sectors > device->sectors - start)
      return -22;
    for (unsigned j = 0; j < used; j++)
      if (start <= ranges[j].last && ranges[j].first < start + sectors)
        return -22;
    ranges[used].first = start;
    ranges[used++].last = start + sectors - 1;
    if (p[4] == 0x83 && found < capacity)
      out[found++] = (struct disk_partition){start, sectors};
  }
  return found;
}
