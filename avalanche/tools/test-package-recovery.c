#include "../src/package.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef fs_size
#undef fs_read
#undef fs_write
#undef fs_remove
#undef fs_entry
/* Fault-injection filesystem: tests the production transaction code above. */
static struct {
  char name[32];
  unsigned size;
  uint8_t data[FS_MAX_FILE];
} files[12];
static bool fail_flush;
static int slot(const char *name) {
  for (unsigned i = 0; i < 12; i++)
    if (!strcmp(files[i].name, name))
      return i;
  return -1;
}
int fs_size(const char *name) {
  int i = slot(name);
  return i < 0 ? -2 : (int)files[i].size;
}
int fs_read(const char *name, void *data, uint32_t capacity) {
  int i = slot(name);
  if (i < 0)
    return -2;
  if (files[i].size > capacity)
    return -27;
  memcpy(data, files[i].data, files[i].size);
  return files[i].size;
}
int fs_write(const char *name, const void *data, uint32_t size) {
  if (size > FS_MAX_FILE)
    return -27;
  int i = slot(name);
  if (i < 0)
    for (unsigned j = 0; j < 12; j++)
      if (!files[j].name[0]) {
        i = j;
        break;
      }
  if (i < 0)
    return -28;
  assert(strlen(name) < 32);
  strcpy(files[i].name, name);
  memcpy(files[i].data, data, size);
  files[i].size = size;
  return size;
}
int fs_remove(const char *name) {
  int i = slot(name);
  if (i < 0)
    return -2;
  files[i].name[0] = 0;
  return 0;
}
bool power_flush(void) { return !fail_flush; }
void console_write(const char *s) { (void)s; }
uint32_t crc32(const void *data, size_t size) {
  const uint8_t *p = data;
  uint32_t crc = ~0u;
  while (size--) {
    crc ^= *p++;
    for (unsigned b = 0; b < 8; b++)
      crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320u);
  }
  return ~crc;
}
static void check(const char *name, const char *expected) {
  char data[128];
  int n = fs_read(name, data, sizeof(data));
  assert(n == (int)strlen(expected));
  assert(!memcmp(data, expected, n));
}
int main(int argc, char **argv) {
  uint32_t parts[3];
  assert(version_parts("1.23.456", parts) && parts[0] == 1 && parts[1] == 23 &&
         parts[2] == 456);
  assert(!version_parts("1.0", parts));
  assert(!version_parts("4294967296.0.0", parts));
  assert(!version_parts("1.0.0bad", parts));
  fs_write("pkg-check", "version 2.1.0\n", 14);
  assert(version_allowed("check", "2.1.0"));
  assert(version_allowed("check", "2.2.0"));
  assert(!version_allowed("check", "2.0.9"));
  fs_remove("pkg-check");
  puts("PASS semantic version parsing and downgrade rejection");
  if (argc == 3) {
    uint8_t cert[4096], signature[512];
    FILE *input = fopen(argv[1], "rb");
    assert(input);
    size_t cn = fread(cert, 1, sizeof(cert), input);
    fclose(input);
    input = fopen(argv[2], "rb");
    assert(input);
    size_t sn = fread(signature, 1, sizeof(signature), input);
    fclose(input);
    const br_x509_pkey *key = trust_key(cert, cn);
    assert(key);
    assert(signature_valid(key, signature, sn, "manifest", 8));
    assert(!signature_valid(key, signature, sn, "tampered", 8));
    signature[0] ^= 1;
    assert(!signature_valid(key, signature, sn, "manifest", 8));
    assert(!signature_valid(key, signature, sn - 1, "manifest", 8));
    puts("PASS pinned-certificate RSA signature verification and tamper "
         "rejection");
  }
  assert(fs_write("hello", "old binary", 10) == 10);
  assert(fs_write("pkg-hello", "old record", 10) == 10);
  assert(transaction_begin("hello") == 0);
  fs_write("hello", "new binary", 10);
  fs_write("pkg-hello", "new record", 10);
  assert(transaction_recover() == 0);
  check("hello", "old binary");
  check("pkg-hello", "old record");
  puts("PASS interrupted upgrade restores executable and database");
  assert(transaction_begin("hello") == 0);
  fs_remove("hello");
  fs_remove("pkg-hello");
  assert(transaction_recover() == 0);
  check("hello", "old binary");
  puts("PASS interrupted removal restores executable and database");
  assert(transaction_begin("fresh") == 0);
  fs_write("fresh", "new", 3);
  fs_write("pkg-fresh", "record", 6);
  assert(transaction_recover() == 0);
  assert(fs_size("fresh") == -2 && fs_size("pkg-fresh") == -2);
  puts("PASS interrupted installation removes partial package");
  assert(transaction_begin("hello") == 0);
  fs_write("hello", "new binary", 10);
  fs_write("pkg-hello", "new record", 10);
  assert(transaction_commit() == 0);
  assert(transaction_recover() == 0);
  check("hello", "new binary");
  check("pkg-hello", "new record");
  puts("PASS committed upgrade survives recovery");
  fail_flush = true;
  assert(transaction_begin("hello") < 0);
  assert(fs_size(transaction_file) < 0);
  check("hello", "new binary");
  fail_flush = false;
  puts("PASS failed backup flush leaves live package unchanged");
  assert(transaction_begin("hello") == 0);
  fs_write(binary_backup, "corrupt", 7);
  assert(transaction_recover() == -74);
  check("hello", "new binary");
  puts("PASS corrupt backup blocks recovery before altering live files");
  return 0;
}
