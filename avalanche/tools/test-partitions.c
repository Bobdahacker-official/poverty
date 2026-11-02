#include "../src/partition.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t image[2 * 1024 * 1024];
static struct block_device device = {.sectors = sizeof(image) / 512};
const struct block_device *disk_get(unsigned disk) {
  return disk ? NULL : &device;
}
bool disk_read(unsigned disk, uint32_t lba, void *out) {
  if (disk || lba >= device.sectors)
    return false;
  memcpy(out, image + (size_t)lba * 512, 512);
  return true;
}
static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = v >> (i * 8);
}
static void put64(uint8_t *p, uint64_t v) {
  put32(p, v);
  put32(p + 4, v >> 32);
}
static unsigned native, total, table_blocks;
static uint8_t *entry_table;
static void finish(void) {
  uint32_t crc = ~crc_step(~0u, entry_table, 16384);
  memcpy(image + (total - 1 - table_blocks) * native, entry_table, 16384);
  for (unsigned backup = 0; backup < 2; backup++) {
    unsigned at = backup ? total - 1 : 1;
    uint8_t *h = image + at * native;
    memset(h, 0, native);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x10000);
    put32(h + 12, 92);
    put64(h + 24, at);
    put64(h + 32, backup ? 1 : total - 1);
    put64(h + 40, 2 + table_blocks);
    put64(h + 48, total - 2 - table_blocks);
    put64(h + 72, backup ? total - 1 - table_blocks : 2);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, crc);
    put32(h + 16, ~crc_step(~0u, h, 92));
  }
}
static void fixture(unsigned bytes) {
  memset(image, 0, sizeof(image));
  native = bytes;
  total = sizeof(image) / bytes;
  device.logical_sector_size = bytes;
  table_blocks = 16384 / bytes;
  image[450] = 0xee;
  image[510] = 0x55;
  image[511] = 0xaa;
  put32(image + 454, 1);
  put32(image + 458, total - 1);
  entry_table = image + 2 * bytes;
  const uint8_t linux_type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84,
                                  0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69,
                                  0xd8, 0x47, 0x7d, 0xe4};
  memcpy(entry_table, linux_type, 16);
  put64(entry_table + 32, 128);
  put64(entry_table + 40, 159);
  entry_table[128] = 1; /* An unrelated partition must be ignored. */
  put64(entry_table + 128 + 32, 64);
  put64(entry_table + 128 + 40, 95);
  finish();
}
int main(void) {
  struct disk_partition partitions[16];
  for (unsigned bytes = 512; bytes <= 4096; bytes *= 8) {
    fixture(bytes);
    assert(partition_linux(0, partitions, 16) == 1);
    assert(partitions[0].start == 128 * (bytes / 512) &&
           partitions[0].sectors == 32 * (bytes / 512));
    image[bytes + 16] ^= 1;
    assert(partition_linux(0, partitions, 16) == 1); /* Backup GPT recovery. */
    image[(total - 1) * bytes + 16] ^= 1;
    assert(partition_linux(0, partitions, 16) < 0);
    fixture(bytes);
    entry_table[0] ^= 1;
    image[(total - 1 - table_blocks) * bytes] ^= 1;
    assert(partition_linux(0, partitions, 16) < 0);
    fixture(bytes);
    put64(entry_table + 128 + 32, 140);
    finish();
    assert(partition_linux(0, partitions, 16) < 0); /* Reversed interval. */
    fixture(bytes);
    put64(entry_table + 128 + 40, 140);
    finish();
    assert(partition_linux(0, partitions, 16) <
           0); /* Overlaps Linux partition. */
    fixture(bytes);
    put64(entry_table + 48, UINT64_C(1) << 60);
    finish();
    assert(partition_linux(0, partitions, 16) ==
           0); /* Read-only GPT attribute. */
  }
  memset(image, 0, sizeof(image));
  device.logical_sector_size = 512;
  image[510] = 0x55;
  image[511] = 0xaa;
  image[450] = 0x83;
  put32(image + 454, 128);
  put32(image + 458, 32);
  assert(partition_linux(0, partitions, 16) == 1 && partitions[0].start == 128);
  image[466] = 7;
  put32(image + 470, 140);
  put32(image + 474, 32);
  assert(partition_linux(0, partitions, 16) < 0);
  assert(partition_linux(1, partitions, 16) < 0);
  puts("PASS GPT native-sector translation, backup headers and Linux partition "
       "selection");
  puts("PASS GPT checksums, overlaps, read-only flags and MBR boundary "
       "validation");
  return 0;
}
