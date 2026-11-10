#include "sandbox.h"
#include "package.h"
#include "../vendor/bearssl/bearssl.h"
#include "http.h"
#define STBI_NO_STDIO
#include "../vendor/stb/stb_image.h"
#define PACKAGE_RECORD_CAPACITY 640
static unsigned package_entries;
static bool package_entry(unsigned index, char *name, uint32_t *size) {
  char full[256];
  uint64_t length;
  bool directory;
  if (fs_browse("/", index, full, &length, &directory) ||
      directory || strlen(full) > 31 || length > UINT32_MAX)
    return false;
  memcpy(name, full, strlen(full) + 1);
  *size = length;
  return true;
}
#define fs_entry package_entry
struct package {
  const char *name, *version;
  int dependency;
};
static const struct package catalog[] = {{"hello", "1.0.0", -1},
                                         {"cat", "1.0.0", -1},
                                         {"worker", "1.0.0", -1},
                                         {"udp", "1.0.0", 0},
                                         {"http", "1.0.0", 3}};
#define COUNT (sizeof(catalog) / sizeof(catalog[0]))
static uint8_t image[FS_MAX_FILE], previous[FS_MAX_FILE];
static uint8_t response[FS_MAX_FILE + 4096];
static bool safe_name(const char *name) {
  unsigned length = strlen(name);
  bool magenta = length > 6 && str_eq(name + length - 6, ".mgnta");
  unsigned n = 0;
  for (; name[n]; n++)
    if (n >= 27 || !((magenta && n == length - 6 && name[n] == '.') ||
                     (name[n] >= 'a' && name[n] <= 'z') ||
                     (name[n] >= '0' && name[n] <= '9') || name[n] == '-' ||
                     name[n] == '_'))
      return false;
  return n > 0;
}
struct package_transaction {
  uint32_t magic, checksum;
  char name[28];
  int32_t binary_size, record_size;
  uint8_t binary_hash[32], record_hash[32];
};
static const char transaction_file[] = ".pkg-transaction";
static const char binary_backup[] = ".pkg-previous";
static const char record_backup[] = ".pkg-record";
static bool same_digest(const uint8_t *a, const uint8_t *b) {
  unsigned difference = 0;
  for (unsigned i = 0; i < 32; i++)
    difference |= a[i] ^ b[i];
  return difference == 0;
}
static int transaction_recover(void) {
  int pending_size = fs_size(transaction_file);
  if (pending_size < -2)
    return pending_size;
  if (pending_size < 0)
    return 0;
  struct package_transaction t;
  if (fs_read(transaction_file, &t, sizeof(t)) != sizeof(t))
    return -74;
  uint32_t saved = t.checksum;
  t.checksum = 0;
  if (t.magic != 0x50545831 || crc32(&t, sizeof(t)) != saved ||
      !__builtin_memchr(t.name, 0, sizeof(t.name)) || !safe_name(t.name) ||
      t.binary_size < -1 || t.binary_size > (int)sizeof(previous) ||
      t.record_size < -1 || t.record_size > 512)
    return -74;
  char record[PACKAGE_RECORD_CAPACITY], db[32];
  uint8_t digest[32];
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, t.name, strlen(t.name) + 1);
  /* Validate both backups before restoring either member of the transaction. */
  if (t.binary_size >= 0) {
    if (fs_read(binary_backup, previous, sizeof(previous)) != t.binary_size)
      return -74;
    sha256(previous, t.binary_size, digest);
    if (!same_digest(digest, t.binary_hash))
      return -74;
  }
  if (t.record_size >= 0) {
    if (fs_read(record_backup, record, sizeof(record)) != t.record_size)
      return -74;
    sha256(record, t.record_size, digest);
    if (!same_digest(digest, t.record_hash))
      return -74;
  }
  if (t.binary_size >= 0) {
    if (fs_write(t.name, previous, t.binary_size) < 0)
      return -5;
  } else if (fs_size(t.name) >= 0 && fs_remove(t.name) < 0)
    return -5;
  if (t.record_size >= 0) {
    if (fs_write(db, record, t.record_size) < 0)
      return -5;
  } else if (fs_size(db) >= 0 && fs_remove(db) < 0)
    return -5;
  if (!power_flush() || fs_remove(transaction_file) < 0 || !power_flush())
    return -5;
  console_write("Recovered interrupted package update.\n");
  return 0;
}
static int transaction_begin(const char *name) {
  if (!safe_name(name) || fs_size(transaction_file) >= 0)
    return -16;
  struct package_transaction t = {0};
  char db[32], record[PACKAGE_RECORD_CAPACITY];
  t.magic = 0x50545831;
  memcpy(t.name, name, strlen(name) + 1);
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, name, strlen(name) + 1);
  t.binary_size = fs_size(name);
  t.record_size = fs_size(db);
  if (t.binary_size < -2 || t.record_size < -2)
    return -5;
  if (t.binary_size < 0)
    t.binary_size = -1;
  if (t.record_size < 0)
    t.record_size = -1;
  if (t.binary_size > (int)sizeof(previous) ||
      t.record_size > (int)sizeof(record))
    return -27;
  if (t.binary_size >= 0) {
    if (fs_read(name, previous, sizeof(previous)) != t.binary_size)
      return -5;
    sha256(previous, t.binary_size, t.binary_hash);
    if (fs_write(binary_backup, previous, t.binary_size) < 0)
      return -5;
  }
  if (t.record_size >= 0) {
    if (fs_read(db, record, sizeof(record)) != t.record_size)
      return -5;
    sha256(record, t.record_size, t.record_hash);
    if (fs_write(record_backup, record, t.record_size) < 0)
      return -5;
  }
  if (!power_flush())
    return -5;
  t.checksum = crc32(&t, sizeof(t));
  if (fs_write(transaction_file, &t, sizeof(t)) < 0 || !power_flush())
    return -5;
  return 0;
}
static int transaction_commit(void) {
  if (!power_flush() || fs_remove(transaction_file) < 0 || !power_flush())
    return -5;
  return 0;
}
static int package_download(const char *host, const char *path, void *out,
                            unsigned capacity) {
  if (!*host || *path != '/' || strlen(host) > 200 || strlen(path) > 200)
    return -22;
  for (const char *p = host; *p; p++)
    if (*p < 33 || *p == 127)
      return -22;
  for (const char *p = path; *p; p++)
    if (*p < 33 || *p == 127)
      return -22;
  char url[512];
  const char *scheme =
      (strlen(host) >= 8 && !__builtin_memcmp(host, "https://", 8)) ||
              (strlen(host) >= 7 && !__builtin_memcmp(host, "http://", 7))
          ? ""
          : "https://";
  unsigned n = strlen(scheme), h = strlen(host), p = strlen(path);
  if (n + h + p >= sizeof(url))
    return -36;
  memcpy(url, scheme, n);
  memcpy(url + n, host, h);
  memcpy(url + n + h, path, p + 1);
  int count = http_get(url, (char *)response, sizeof(response));
  if (count < 0)
    return count;
  if ((unsigned)count > capacity)
    return -27;
  memcpy(out, response, count);
  return count;
}
static uint8_t trust_certificate[4096];
static br_x509_decoder_context trust_decoder;
static const br_x509_pkey *trust_key(const void *certificate, unsigned size) {
  br_x509_decoder_init(&trust_decoder, NULL, NULL);
  br_x509_decoder_push(&trust_decoder, certificate, size);
  const br_x509_pkey *key = br_x509_decoder_get_pkey(&trust_decoder);
  if (!key || key->key_type != BR_KEYTYPE_RSA || key->key.rsa.nlen < 256 ||
      key->key.rsa.nlen > 512)
    return NULL;
  return key;
}
static bool signature_valid(const br_x509_pkey *key, const void *signature,
                            unsigned signature_size, const void *data,
                            unsigned size) {
  if (!key || key->key_type != BR_KEYTYPE_RSA ||
      signature_size != key->key.rsa.nlen)
    return false;
  uint8_t expected[32], actual[32];
  sha256(data, size, expected);
  br_rsa_pkcs1_vrfy verify = br_rsa_pkcs1_vrfy_get_default();
  return verify(signature, signature_size, BR_HASH_OID_SHA256, 32,
                &key->key.rsa, actual) &&
         same_digest(expected, actual);
}
struct bundle_entry {
  char name[48];
  const uint8_t *data;
  unsigned method, packed, size;
  uint32_t checksum;
};
struct package_bundle {
  struct bundle_entry entries[4];
  unsigned count;
};
static int bundle_read(const struct package_bundle *bundle, const char *name,
                       void *out, unsigned capacity) {
  for (unsigned i = 0; i < bundle->count; i++) {
    const struct bundle_entry *entry = &bundle->entries[i];
    if (!str_eq(entry->name, name)) continue;
    if (entry->size > capacity) return -27;
    if (!entry->method)
      memcpy(out, entry->data, entry->size);
    else if (stbi_zlib_decode_noheader_buffer(out, entry->size,
                    (const char *)entry->data, entry->packed) != (int)entry->size)
      return -74;
    return crc32(out, entry->size) == entry->checksum ? (int)entry->size : -74;
  }
  return -2;
}
static int package_source(const char *host, const struct package_bundle *bundle,
                          const char *path, void *out, unsigned capacity) {
  if (host) return package_download(host, path, out, capacity);
  if (bundle) return bundle_read(bundle, path, out, capacity);
  return fs_read(path, out, capacity);
}
static int verify_manifest(const char *host, const struct package_bundle *bundle,
                           const char *path, const void *data, unsigned size,
                           uint8_t publisher[32]) {
  int count =
      fs_read(".pkg-trust.der", trust_certificate, sizeof(trust_certificate));
  if (count < 0)
    return -13;
  sha256(trust_certificate, count, publisher);
  const br_x509_pkey *key = trust_key(trust_certificate, count);
  if (!key)
    return -74;
  char location[256];
  unsigned length = strlen(path);
  if (length + 4 >= sizeof(location))
    return -36;
  memcpy(location, path, length);
  memcpy(location + length, ".sig", 5);
  uint8_t signature[512];
  count = package_source(host, bundle, location, signature, sizeof(signature));
  if (count < 0)
    return count;
  if (!signature_valid(key, signature, count, data, size))
    return -74;
  return 0;
}
static void database_name(unsigned n, char *name) {
  memcpy(name, "pkg-", 4);
  memcpy(name + 4, catalog[n].name, strlen(catalog[n].name) + 1);
}
static bool installed(unsigned n) {
  char name[32];
  database_name(n, name);
  return fs_size(name) >= 0;
}
static bool required(const char *name) {
  for (unsigned slot = 0; slot < package_entries; slot++) {
    char db[32], record[PACKAGE_RECORD_CAPACITY];
    uint32_t size;
    if (!package_entry(slot, db, &size) || size >= sizeof(record) || db[0] != 'p' ||
        db[1] != 'k' || db[2] != 'g' || db[3] != '-')
      continue;
    int n = fs_read(db, record, sizeof(record) - 1);
    if (n < 0)
      continue;
    record[n] = 0;
    for (char *p = record; *p;) {
      char *line = p;
      while (*p && *p != '\n')
        p++;
      if (*p)
        *p++ = 0;
      const char prefix[] = "depends ";
      unsigned i = 0;
      while (prefix[i] && line[i] == prefix[i])
        i++;
      if (!prefix[i] && str_eq(line + i, name))
        return true;
    }
  }
  return false;
}
static char *word(char **p) {
  while (**p == ' ')
    ++*p;
  char *start = *p;
  while (**p && **p != ' ')
    ++*p;
  if (**p)
    *(*p)++ = 0;
  return start;
}
static bool match(const char *a, const char *b) {
  if (!*b)
    return true;
  for (; *a; a++) {
    unsigned n = 0;
    while (b[n] && a[n] && b[n] == a[n])
      n++;
    if (!b[n])
      return true;
  }
  return false;
}
static bool version_parts(const char *text, uint32_t parts[3]) {
  for (unsigned i = 0; i < 3; i++) {
    if (*text < '0' || *text > '9')
      return false;
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
      unsigned digit = *text++ - '0';
      if (value > (UINT32_MAX - digit) / 10)
        return false;
      value = value * 10 + digit;
    }
    parts[i] = value;
    if (i < 2) {
      if (*text++ != '.')
        return false;
    }
  }
  return !*text;
}
static bool version_allowed(const char *name, const char *version) {
  uint32_t incoming[3], installed_version[3];
  if (!version_parts(version, incoming))
    return false;
  char db[32], record[PACKAGE_RECORD_CAPACITY];
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, name, strlen(name) + 1);
  int count = fs_read(db, record, sizeof(record) - 1);
  if (count < 0)
    return count == -1 || count == -2;
  record[count] = 0;
  for (char *line = record; *line;) {
    char *end = line;
    while (*end && *end != '\n')
      end++;
    char *next = *end ? end + 1 : end;
    *end = 0;
    if (strlen(line) >= 8 && !__builtin_memcmp(line, "version ", 8)) {
      if (!version_parts(line + 8, installed_version))
        return false;
      for (unsigned i = 0; i < 3; i++) {
        if (incoming[i] > installed_version[i])
          return true;
        if (incoming[i] < installed_version[i])
          return false;
      }
      return true;
    }
    line = next;
  }
  return false;
}
static void append(char *out, unsigned *used, const char *s) {
  while (*s && *used < 510)
    out[(*used)++] = *s++;
  out[*used] = 0;
}
static int manifest(unsigned n, char out[512]) {
  const void *data;
  uint32_t size;
  uint8_t hash[32];
  int r = user_builtin(catalog[n].name, &data, &size);
  if (r)
    return r;
  sha256(data, size, hash);
  unsigned used = 0;
  out[0] = 0;
  append(out, &used, "name ");
  append(out, &used, catalog[n].name);
  append(out, &used, "\nversion ");
  append(out, &used, catalog[n].version);
  append(out, &used, "\ndepends ");
  if (catalog[n].dependency >= 0)
    append(out, &used, catalog[catalog[n].dependency].name);
  append(out, &used, "\nfile ");
  append(out, &used, catalog[n].name);
  append(out, &used, "\nsha256 ");
  const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++) {
    out[used++] = digits[hash[i] >> 4];
    out[used++] = digits[hash[i] & 15];
  }
  out[used++] = '\n';
  out[used] = 0;
  return used;
}
static int install(unsigned n, unsigned visiting) {
  if (visiting & (1u << n))
    return -40;
  if (catalog[n].dependency >= 0) {
    unsigned dep = catalog[n].dependency;
    if (!installed(dep)) {
      int r = install(dep, visiting | (1u << n));
      if (r)
        return r;
    }
  }
  const void *data;
  uint32_t size;
  int r = user_builtin(catalog[n].name, &data, &size);
  if (r)
    return r;
  int old = fs_size(catalog[n].name);
  if (old >= 0 && fs_read(catalog[n].name, previous, sizeof(previous)) != old)
    return -5;
  uint8_t expected[32], actual[32];
  if (!version_allowed(catalog[n].name, catalog[n].version))
    return -1;
  sha256(data, size, expected);
  r = transaction_begin(catalog[n].name);
  if (r)
    return r;
  r = fs_write(catalog[n].name, data, size);
  if (r < 0)
    return r;
  r = fs_read(catalog[n].name, image, sizeof(image));
  if (r >= 0)
    sha256(image, r, actual);
  bool equal = r == (int)size;
  for (unsigned i = 0; i < 32 && equal; i++)
    if (expected[i] != actual[i])
      equal = false;
  char name[32], record[512];
  database_name(n, name);
  int count = manifest(n, record);
  if (!equal || count < 0 || fs_write(name, record, count) < 0) {
    transaction_recover();
    return -5;
  }
  r = transaction_commit();
  if (r)
    return r;
  console_write("Installed ");
  console_write(catalog[n].name);
  console_putc('\n');
  return 0;
}
static int fetch(const char *host, const struct package_bundle *bundle,
                 const char *path) {
  char record[512], parsed[512];
  int n = package_source(host, bundle, path, record, sizeof(record) - 1);
  if (n < 0)
    return n;
  if (__builtin_memchr(record, 0, n))
    return -22;
  uint8_t publisher[32];
  int verification = verify_manifest(host, bundle, path, record, n, publisher);
  if (verification)
    return verification;
  record[n] = 0;
  memcpy(parsed, record, n + 1);
  char *name = NULL, *version = NULL, *dependency = NULL, *file = NULL,
       *hash = NULL, *capabilities = NULL;
  char *p = parsed;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n')
      p++;
    if (*p)
      *p++ = 0;
    char *key = word(&line);
    if (str_eq(key, "name") && !name)
      name = line;
    else if (str_eq(key, "version") && !version)
      version = line;
    else if (str_eq(key, "depends") && !dependency)
      dependency = line;
    else if (str_eq(key, "file") && !file)
      file = line;
    else if (str_eq(key, "sha256") && !hash)
      hash = line;
    else if (str_eq(key, "capabilities") && !capabilities)
      capabilities = line;
    else
      return -22;
  }
  if (!name || !version || !file || !hash || !dependency || !safe_name(name) ||
      !str_eq(name, file) || strlen(hash) != 64 || !strlen(version) ||
      strlen(version) > 31)
    return -22;
  if (!host && (strlen(name) <= 6 || !str_eq(name + strlen(name) - 6, ".mgnta")))
    return -22;
  uint32_t capability_mask = AV_CAP_ALL;
  if (capabilities) {
    bool valid;
    capability_mask = parse_uint(capabilities, &valid);
    if (!valid || (capability_mask & ~AV_CAP_ALL)) return -22;
  }
  if (!version_allowed(name, version))
    return -1;
  uint8_t expected[32], actual[32];
  for (unsigned i = 0; i < 64; i++) {
    char c = hash[i];
    unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                            : 16;
    if (digit > 15)
      return -22;
    if (!(i & 1))
      expected[i / 2] = digit << 4;
    else
      expected[i / 2] |= digit;
  }
  char location[256];
  unsigned prefix = 0;
  for (unsigned i = 0; path[i]; i++)
    if (path[i] == '/')
      prefix = i + 1;
  if (prefix + strlen(file) >= sizeof(location))
    return -36;
  memcpy(location, path, prefix);
  memcpy(location + prefix, file, strlen(file) + 1);
  static uint8_t payload[FS_MAX_FILE];
  int count = package_source(host, bundle, location, payload, sizeof(payload));
  if (count < 0)
    return count;
  sha256(payload, count, actual);
  for (unsigned i = 0; i < 32; i++)
    if (expected[i] != actual[i])
      return -74;
  if (count < 4 || __builtin_memcmp(payload, "\177ELF", 4))
    return -22;
  if (*dependency) {
    unsigned dep = 0;
    while (dep < COUNT && !str_eq(dependency, catalog[dep].name))
      dep++;
    if (dep == COUNT)
      return -2;
    int result = install(dep, 0);
    if (result)
      return result;
  }
  if (capabilities) {
    int security = sandbox_profile(expected, capability_mask);
    if (security < 0) return security;
  }
  int old = fs_size(name);
  if (old >= 0 && fs_read(name, previous, sizeof(previous)) != old)
    return -5;
  int r = transaction_begin(name);
  if (r)
    return r;
  r = fs_write(name, payload, count);
  if (r < 0)
    return r;
  char db[32];
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, name, strlen(name) + 1);
  char receipt[sizeof(record) + 90];
  memcpy(receipt, record, n);
  unsigned used = n;
  if (!used || receipt[used - 1] != '\n') receipt[used++] = '\n';
  static const char publisher_key[] = "publisher-sha256 ";
  memcpy(receipt + used, publisher_key, sizeof(publisher_key) - 1);
  used += sizeof(publisher_key) - 1;
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++) {
    receipt[used++] = hex[publisher[i] >> 4];
    receipt[used++] = hex[publisher[i] & 15];
  }
  receipt[used++] = '\n';
  r = fs_write(db, receipt, used);
  if (r < 0) {
    transaction_recover();
    return r;
  }
  r = fs_read(name, previous, sizeof(previous));
  if (r != count) {
    transaction_recover();
    return -5;
  }
  sha256(previous, r, actual);
  if (!same_digest(actual, expected)) {
    transaction_recover();
    return -74;
  }
  r = transaction_commit();
  if (r)
    return r;
  console_write("Installed ");
  console_write(name);
  console_putc('\n');
  return 0;
}
static unsigned zip16(const uint8_t *data) {
  return data[0] | ((unsigned)data[1] << 8);
}
static uint32_t zip32(const uint8_t *data) {
  return zip16(data) | ((uint32_t)zip16(data + 2) << 16);
}
static int install_bundle(const char *path) {
  static uint8_t archive[FS_MAX_FILE + 16384];
  int bytes = fs_read(path, archive, sizeof(archive));
  if (bytes < 0) return bytes;
  struct package_bundle bundle = {0};
  unsigned size = bytes, offset = 0;
  const char *manifest = NULL;
  while (offset + 4 <= size && zip32(archive + offset) == 0x04034b50) {
    if (bundle.count == 4 || size - offset < 30) return -22;
    const uint8_t *header = archive + offset;
    unsigned flags = zip16(header + 6), method = zip16(header + 8);
    unsigned packed = zip32(header + 18), unpacked = zip32(header + 22);
    unsigned name_length = zip16(header + 26), extra = zip16(header + 28);
    struct bundle_entry *entry = &bundle.entries[bundle.count];
    unsigned prefix = 30 + name_length + extra;
    if ((flags & ~0x800u) || (method != 0 && method != 8) ||
        !name_length || name_length >= sizeof(entry->name) ||
        prefix > size - offset || packed > size - offset - prefix ||
        unpacked > FS_MAX_FILE || (!method && packed != unpacked))
      return -22;
    for (unsigned i = 0; i < name_length; i++) {
      uint8_t c = header[30 + i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_'))
        return -22;
      entry->name[i] = c;
    }
    for (unsigned i = 0; i < bundle.count; i++)
      if (str_eq(bundle.entries[i].name, entry->name)) return -22;
    entry->data = header + prefix;
    entry->method = method;
    entry->packed = packed;
    entry->size = unpacked;
    entry->checksum = zip32(header + 14);
    if (name_length > 9 && str_eq(entry->name + name_length - 9, ".manifest")) {
      if (manifest) return -22;
      manifest = entry->name;
    }
    bundle.count++;
    offset += prefix + packed;
  }
  if (bundle.count != 4 || !manifest || size - offset < 4 ||
      zip32(archive + offset) != 0x02014b50)
    return -22;
  unsigned basename = strlen(manifest) - 9;
  char executable[48], signature[52];
  memcpy(executable, manifest, basename);
  executable[basename] = 0;
  if (!safe_name(executable)) return -22;
  unsigned manifest_length = strlen(manifest);
  memcpy(signature, manifest, manifest_length);
  memcpy(signature + manifest_length, ".sig", 5);
  unsigned matches = 0;
  for (unsigned i = 0; i < bundle.count; i++) {
    const char *name = bundle.entries[i].name;
    matches += str_eq(name, executable) || str_eq(name, signature) ||
               str_eq(name, "trust.der");
  }
  if (matches != 3) return -22;
  return fetch(NULL, &bundle, manifest);
}
int package_install_local(const char *manifest_path) {
  int recovery = transaction_recover();
  if (recovery) return recovery;
  unsigned length = strlen(manifest_path);
  if (length >= 4 && str_eq(manifest_path + length - 4, ".zip"))
    return install_bundle(manifest_path);
  return fetch(NULL, NULL, manifest_path);
}
void package_command(char *args) {
  package_entries = 0;
  char entry[256];
  uint64_t size;
  bool directory;
  while (package_entries < 65536 &&
         !fs_browse("/", package_entries, entry, &size, &directory))
    package_entries++;
  int recovery = transaction_recover();
  if (recovery) {
    console_write("Package recovery failed. Package changes blocked.\n");
    return;
  }
  char *action = word(&args), *name = word(&args);
  if (str_eq(action, "recover"))
    return;
  if (str_eq(action, "trust") || str_eq(action, "trust-url")) {
    if (!*name) {
      console_write(
          "pkg trust LOCAL-DER-CERTIFICATE pins an RSA signing certificate\n");
      return;
    }
    int count =
        str_eq(action, "trust-url")
            ? package_download(name, word(&args), trust_certificate,
                               sizeof(trust_certificate))
            : fs_read(name, trust_certificate, sizeof(trust_certificate));
    if (count < 0 || !trust_key(trust_certificate, count)) {
      console_write(
          "Expected a DER certificate with a 2048 to 4096 bit RSA key.\n");
      return;
    }
    if (fs_write(".pkg-trust.der", trust_certificate, count) < 0 ||
        !power_flush()) {
      console_write("Could not save the trust certificate.\n");
      return;
    }
    uint8_t digest[32];
    sha256(trust_certificate, count, digest);
    const char *hex = "0123456789abcdef";
    console_write("Signing certificate SHA-256: ");
    for (unsigned i = 0; i < 32; i++) {
      console_putc(hex[digest[i] >> 4]);
      console_putc(hex[digest[i] & 15]);
    }
    console_putc('\n');
    return;
  }
  if (str_eq(action, "fetch")) {
    int r = fetch(name, NULL, word(&args));
    if (r < 0) {
      console_write("Package download error ");
      print_uint(-r);
      console_putc('\n');
    }
    return;
  }
  if (str_eq(action, "list") || str_eq(action, "search")) {
    for (unsigned n = 0; n < COUNT; n++)
      if (!*name || match(catalog[n].name, name)) {
        console_write(catalog[n].name);
        console_putc(' ');
        console_write(catalog[n].version);
        console_write(installed(n) ? " installed\n" : " available\n");
      }
    for (unsigned slot = 0; slot < package_entries; slot++) {
      char db[32];
      uint32_t size;
      if (!package_entry(slot, db, &size) || db[0] != 'p' || db[1] != 'k' ||
          db[2] != 'g' || db[3] != '-')
        continue;
      bool known = false;
      for (unsigned i = 0; i < COUNT; i++)
        if (str_eq(db + 4, catalog[i].name))
          known = true;
      if (!known && match(db + 4, name)) {
        char version[32] = "unknown", record[PACKAGE_RECORD_CAPACITY];
        int count = fs_read(db, record, sizeof(record) - 1);
        if (count >= 0) {
          record[count] = 0;
          for (char *line = record; *line;) {
            char *end = line;
            while (*end && *end != '\n') end++;
            unsigned length = end - line;
            if (length > 8 && length < sizeof(version) + 8 &&
                !__builtin_memcmp(line, "version ", 8)) {
              memcpy(version, line + 8, length - 8);
              version[length - 8] = 0;
              break;
            }
            line = *end ? end + 1 : end;
          }
        }
        console_write(db + 4);
        console_putc(' ');
        console_write(version);
        console_write(" installed\n");
      }
    }
    return;
  }
  unsigned n = 0;
  while (n < COUNT && !str_eq(name, catalog[n].name))
    n++;
  if (n == COUNT) {
    if (!safe_name(name)) {
      console_write("Package not found\n");
      return;
    }
    char db[32], record[PACKAGE_RECORD_CAPACITY];
    memcpy(db, "pkg-", 4);
    memcpy(db + 4, name, strlen(name) + 1);
    int size = fs_read(db, record, sizeof(record) - 1);
    if (size < 0) {
      console_write("Package not found\n");
      return;
    }
    if (str_eq(action, "query")) {
      record[size] = 0;
      console_write(record);
      return;
    }
    if (str_eq(action, "remove") && !required(name)) {
      int r = transaction_begin(name);
      if (!r && fs_remove(name) >= 0 && fs_remove(db) >= 0)
        r = transaction_commit();
      else if (!r) {
        transaction_recover();
        r = -5;
      }
      if (r)
        console_write("Package removal failed.\n");
      return;
    }
    console_write(
        "Use pkg fetch HOST /MANIFEST to upgrade a downloaded package\n");
    return;
  }
  int r = 0;
  if (str_eq(action, "install") || str_eq(action, "upgrade"))
    r = install(n, 0);
  else if (str_eq(action, "query")) {
    char out[PACKAGE_RECORD_CAPACITY], db[32];
    database_name(n, db);
    r = installed(n) ? fs_read(db, out, sizeof(out) - 1) : manifest(n, out);
    if (r >= 0) {
      out[r] = 0;
      console_write(out);
      r = 0;
    }
  } else if (str_eq(action, "remove")) {
    if (required(name)) {
      console_write("Package has installed dependents\n");
      return;
    }
    for (unsigned i = 0; i < COUNT; i++)
      if (catalog[i].dependency == (int)n && installed(i)) {
        console_write("Required by ");
        console_write(catalog[i].name);
        console_putc('\n');
        return;
      }
    if (!installed(n)) {
      console_write("Package is not installed\n");
      return;
    }
    char record[32];
    database_name(n, record);
    r = transaction_begin(catalog[n].name);
    if (!r) {
      if (fs_remove(catalog[n].name) < 0 || fs_remove(record) < 0) {
        transaction_recover();
        r = -5;
      } else
        r = transaction_commit();
    }
  } else {
    console_write("pkg install|remove|upgrade|list|query|search NAME\n");
    return;
  }
  if (r < 0) {
    console_write("Package error ");
    print_uint(-r);
    console_putc('\n');
  }
}
