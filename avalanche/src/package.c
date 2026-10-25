#include "package.h"
#include "ext4.h"
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
  unsigned n = 0;
  for (; name[n]; n++)
    if (n >= 27 || !((name[n] >= 'a' && name[n] <= 'z') ||
                     (name[n] >= '0' && name[n] <= '9') || name[n] == '-' ||
                     name[n] == '_'))
      return false;
  return n > 0;
}
static int http_get(const char *host, const char *path, void *out,
                    unsigned capacity) {
  if (!*host || *path != '/' || strlen(host) > 200 || strlen(path) > 200)
    return -22;
  for (const char *p = host; *p; p++)
    if (*p < 33 || *p == 127)
      return -22;
  for (const char *p = path; *p; p++)
    if (*p < 33 || *p == 127)
      return -22;
  uint32_t address;
  if (!ip_parse(host, &address) && net_dns(host, &address) < 0)
    return -2;
  int r = net_tcp_connect(address, 80);
  if (r < 0)
    return r;
  char request[512];
  unsigned used = 0;
  const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host,
                         "\r\nConnection: close\r\n\r\n"};
  for (unsigned i = 0; i < 5; i++)
    for (const char *p = parts[i]; *p; p++) {
      if (used == sizeof(request)) {
        net_tcp_close();
        return -7;
      }
      request[used++] = *p;
    }
  r = net_tcp_send(request, used);
  if (r < 0) {
    net_tcp_close();
    return r;
  }
  unsigned total = 0;
  while (total < sizeof(response)) {
    r = net_tcp_recv(response + total, sizeof(response) - total, 500);
    if (r <= 0)
      break;
    total += r;
  }
  net_tcp_close();
  if (r < 0)
    return r;
  if (total == sizeof(response) || total < 16)
    return -27;
  const char prefix[] = "HTTP/1.";
  for (unsigned i = 0; i < 7; i++)
    if (response[i] != (uint8_t)prefix[i])
      return -5;
  if ((response[7] != '0' && response[7] != '1') || response[8] != ' ' ||
      response[9] != '2' || response[10] != '0' || response[11] != '0')
    return -5;
  unsigned body = 0;
  for (unsigned i = 0; i + 3 < total && i < 4096; i++)
    if (response[i] == '\r' && response[i + 1] == '\n' &&
        response[i + 2] == '\r' && response[i + 3] == '\n') {
      body = i + 4;
      break;
    }
  if (!body || total - body > capacity)
    return -27;
  memcpy(out, response + body, total - body);
  return total - body;
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
  sha256(data, size, expected);
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
    if (old >= 0)
      fs_write(catalog[n].name, previous, old);
    else
      fs_remove(catalog[n].name);
    return -5;
  }
  console_write("Installed ");
  console_write(catalog[n].name);
  console_putc('\n');
  return 0;
}
static int fetch(const char *host, const char *path) {
  char record[512], parsed[512];
  int n = http_get(host, path, record, sizeof(record) - 1);
  if (n < 0)
    return n;
  record[n] = 0;
  memcpy(parsed, record, n + 1);
  char *name = NULL, *version = NULL, *dependency = NULL, *file = NULL,
       *hash = NULL;
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
    else
      return -22;
  }
  if (!name || !version || !file || !hash || !dependency || !safe_name(name) ||
      !str_eq(name, file) || strlen(hash) != 64 || !strlen(version) ||
      strlen(version) > 31)
    return -22;
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
  int count = http_get(host, location, image, sizeof(image));
  if (count < 0)
    return count;
  sha256(image, count, actual);
  for (unsigned i = 0; i < 32; i++)
    if (expected[i] != actual[i])
      return -74;
  int old = fs_size(name);
  if (old >= 0 && fs_read(name, previous, sizeof(previous)) != old)
    return -5;
  int r = fs_write(name, image, count);
  if (r < 0)
    return r;
  char db[32];
  memcpy(db, "pkg-", 4);
  memcpy(db + 4, name, strlen(name) + 1);
  r = fs_write(db, record, n);
  if (r < 0) {
    if (old >= 0)
      fs_write(name, previous, old);
    else
      fs_remove(name);
    return r;
  }
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
  char *action = word(&args), *name = word(&args);
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
      int old = fs_read(name, previous, sizeof(previous));
      if (old >= 0 && fs_remove(name) >= 0 && fs_remove(db) < 0)
        fs_write(name, previous, old);
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
    int old = fs_read(catalog[n].name, previous, sizeof(previous));
    if (old < 0)
      r = old;
    else if ((r = fs_remove(catalog[n].name)) >= 0 &&
             (r = fs_remove(record)) < 0)
      fs_write(catalog[n].name, previous, old);
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
