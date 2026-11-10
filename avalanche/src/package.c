#include "sandbox.h"
#include "package.h"
#include "../vendor/bearssl/bearssl.h"
#include "ext4.h"
#include "http.h"
static struct ext4_volume package_volume;
static bool package_ext4;
static unsigned package_entries = 32;
static int package_size(const char *name) {
  if (!package_ext4)
    return fs_size(name);
  struct ext4_inode inode;
  int r = ext4_lookup(&package_volume, name, &inode);
  return r ? r : inode.size > INT32_MAX ? -27 : (int)inode.size;
}
static int package_read(const char *name, void *data, uint32_t capacity) {
  if (!package_ext4)
    return fs_read(name, data, capacity);
  struct ext4_inode inode;
  int r = ext4_lookup(&package_volume, name, &inode);
  if (r)
    return r;
  if (inode.size > capacity)
    return -27;
  return ext4_read(&package_volume, &inode, 0, data, inode.size);
}
static int package_write(const char *name, const void *data, uint32_t size) {
  return package_ext4 ? ext4_replace(&package_volume, name, data, size)
                      : fs_write(name, data, size);
}
static int package_remove(const char *name) {
  return package_ext4 ? ext4_remove(&package_volume, name) : fs_remove(name);
}
static bool package_entry(unsigned index, char *name, uint32_t *size) {
  if (!package_ext4)
    return fs_entry(index, name, size);
  char full[256];
  uint64_t length;
  if (ext4_entry(&package_volume, "/", index, full, &length) ||
      strlen(full) > 31 || length > UINT32_MAX)
    return false;
  memcpy(name, full, strlen(full) + 1);
  *size = length;
  return true;
}
#define fs_size package_size
#define fs_read package_read
#define fs_write package_write
#define fs_remove package_remove
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
  char record[512], db[32];
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
  char db[32], record[512];
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
static int verify_manifest(const char *host, const char *path, const void *data,
                           unsigned size) {
  int count =
      fs_read(".pkg-trust.der", trust_certificate, sizeof(trust_certificate));
  if (count < 0)
    return -13;
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
  count = package_download(host, location, signature, sizeof(signature));
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
    char db[32], record[512];
    uint32_t size;
    if (!fs_entry(slot, db, &size) || size >= sizeof(record) || db[0] != 'p' ||
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
  char db[32], record[512];
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
static int fetch(const char *host, const char *path) {
  char record[512], parsed[512];
  int n = package_download(host, path, record, sizeof(record) - 1);
  if (n < 0)
    return n;
  int verification = verify_manifest(host, path, record, n);
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
  if (*dependency) {
    unsigned dep = 0;
    while (dep < COUNT && !str_eq(dependency, catalog[dep].name))
      dep++;
    if (dep == COUNT)
      return -2;
    int r = install(dep, 0);
    if (r)
      return r;
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
  int count = package_download(host, location, image, sizeof(image));
  if (count < 0)
    return count;
  sha256(image, count, actual);
  for (unsigned i = 0; i < 32; i++)
    if (expected[i] != actual[i])
      return -74;
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
  r = fs_write(name, image, count);
  if (r < 0)
    return r;
  char db[32];
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, name, strlen(name) + 1);
  r = fs_write(db, record, n);
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
void package_command(char *args) {
  package_ext4 = false;
  package_entries = 32;
  for (unsigned disk = 0; disk < disk_count(); disk++)
    if (!ext4_mount(&package_volume, disk, 0)) {
      package_ext4 = true;
      package_entries = 0;
      char name[256];
      uint64_t size;
      while (package_entries < 65536 &&
             !ext4_entry(&package_volume, "/", package_entries, name, &size))
        package_entries++;
      break;
    }
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
    int r = fetch(name, word(&args));
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
      if (!fs_entry(slot, db, &size) || db[0] != 'p' || db[1] != 'k' ||
          db[2] != 'g' || db[3] != '-')
        continue;
      bool known = false;
      for (unsigned i = 0; i < COUNT; i++)
        if (str_eq(db + 4, catalog[i].name))
          known = true;
      if (!known && match(db + 4, name)) {
        console_write(db + 4);
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
    char db[32], record[512];
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
    char out[512], db[32];
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
