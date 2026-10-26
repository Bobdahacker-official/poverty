#include "http.h"
#include "system.h"
#include "tls.h"
static bool starts(const char *s, const char *p) {
  while (*p)
    if (*s++ != *p++)
      return false;
  return true;
}
static bool put(char *out, unsigned cap, const char *s) {
  unsigned n = strlen(s);
  if (n >= cap)
    return false;
  memcpy(out, s, n + 1);
  return true;
}
bool http_resolve(const char *base, const char *ref, char *out, unsigned cap) {
  if (starts(ref, "http://") || starts(ref, "https://"))
    return put(out, cap, ref);
  unsigned scheme = starts(base, "https://")  ? 8
                    : starts(base, "http://") ? 7
                                              : 0;
  if (!scheme)
    return false;
  unsigned end = scheme;
  while (base[end] && base[end] != '/')
    end++;
  if (ref[0] == '/' && ref[1] == '/') {
    unsigned n = scheme - 2;
    if (n + strlen(ref) >= cap)
      return false;
    memcpy(out, base, n);
    return put(out + n, cap - n, ref);
  }
  if (*ref != '/') {
    unsigned last = end;
    while (base[last] && base[last] != '?' && base[last] != '#')
      last++;
    while (last > end && base[last - 1] != '/')
      last--;
    if (last > end)
      end = last;
    else {
      if (end + 1 >= cap)
        return false;
      memcpy(out, base, end);
      out[end++] = '/';
      goto append;
    }
  }
  if (end >= cap)
    return false;
  memcpy(out, base, end);
append:
  return put(out + end, cap - end, ref);
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static bool header(const char *line, const char *name) {
  while (*name) {
    char c = *line++;
    if (c >= 'A' && c <= 'Z')
      c += 32;
    if (c != *name++)
      return false;
  }
  return true;
}
int http_get(const char *url, char *body, unsigned capacity) {
  char location[512];
  if (!put(location, sizeof(location), url) || capacity < 2)
    return -1;
  for (unsigned redirect = 0; redirect < 5; redirect++) {
    bool secure = starts(location, "https://");
    const char *s = location + (secure                        ? 8
                                : starts(location, "http://") ? 7
                                                              : 0);
    char host[256], authority[256], request[1024];
    unsigned n = 0;
    while (*s && *s != '/' && *s != '#' && *s != '?') {
      if (n + 1 >= sizeof(host))
        return -1;
      host[n++] = *s++;
    }
    host[n] = 0;
    if (!n)
      return -1;
    put(authority, sizeof(authority), host);
    unsigned port = secure ? 443 : 80;
    for (unsigned i = 0; host[i]; i++)
      if (host[i] == ':') {
        host[i++] = 0;
        port = 0;
        if (!authority[i])
          return -1;
        while (authority[i]) {
          if (authority[i] < '0' || authority[i] > '9' || port > 6553)
            return -1;
          port = port * 10 + authority[i++] - '0';
        }
        break;
      }
    if (!port || port > 65535)
      return -1;
    n = 0;
    memcpy(request, "GET ", 4);
    n = 4;
    if (*s != '/')
      request[n++] = '/';
    while (*s && *s != '#') {
      if (n >= 700)
        return -1;
      request[n++] = *s++;
    }
    const char *suffix = " HTTP/1.1\r\nHost: ";
    memcpy(request + n, suffix, strlen(suffix));
    n += strlen(suffix);
    memcpy(request + n, authority, strlen(authority));
    n += strlen(authority);
    suffix = "\r\nAccept-Encoding: identity\r\nConnection: "
             "close\r\nUser-Agent: Magenta/0.3\r\n\r\n";
    memcpy(request + n, suffix, strlen(suffix));
    n += strlen(suffix);
    struct network_info status;
    net_snapshot(&status);
    if (!status.ip)
      net_dhcp();
    uint32_t ip;
    if (net_dns(host, &ip) < 0 || net_tcp_connect(ip, port) < 0)
      return -2;
    if (secure && !tls_connect(host)) {
      net_tcp_close();
      return -3;
    }
    int result = secure ? tls_send(request, n) : net_tcp_send(request, n);
    unsigned used = 0;
    while (result >= 0 && used < capacity - 1) {
      result = secure ? tls_recv(body + used, capacity - used - 1)
                      : net_tcp_recv(body + used, capacity - used - 1, 1000);
      if (result <= 0)
        break;
      used += result;
    }
    if (secure)
      tls_close();
    else
      net_tcp_close();
    if (secure && tls_error())
      return -3;
    if (!used || used == capacity - 1)
      return -4;
    body[used] = 0;
    if (!starts(body, "HTTP/1.") || used < 12)
      return -5;
    unsigned code =
        (body[9] - '0') * 100 + (body[10] - '0') * 10 + body[11] - '0';
    bool chunked = false;
    char next[512] = {0};
    unsigned offset = 0;
    while (offset + 1 < used) {
      unsigned start = offset;
      while (offset + 1 < used &&
             !(body[offset] == '\r' && body[offset + 1] == '\n'))
        offset++;
      if (offset + 1 >= used)
        return -5;
      unsigned len = offset - start;
      offset += 2;
      if (!len)
        break;
      if (header(body + start, "transfer-encoding:")) {
        unsigned p = start + 18;
        while (p < start + len && body[p] == ' ')
          p++;
        if (!header(body + p, "chunked"))
          return -5;
        chunked = true;
      }
      if (header(body + start, "location:")) {
        unsigned p = start + 9;
        while (p < start + len && body[p] == ' ')
          p++;
        unsigned count = start + len - p;
        if (count >= sizeof(next))
          return -1;
        memcpy(next, body + p, count);
        next[count] = 0;
      }
    }
    if (code >= 300 && code < 400 && *next) {
      char resolved[512];
      if (!http_resolve(location, next, resolved, sizeof(resolved)))
        return -1;
      /* Never silently downgrade an encrypted navigation. */
      if (secure && !starts(resolved, "https://"))
        return -3;
      put(location, sizeof(location), resolved);
      continue;
    }
    if (code < 200 || code >= 300)
      return -(int)code;
    unsigned out = 0;
    if (chunked) {
      while (offset < used) {
        unsigned size = 0, digits = 0;
        while (offset < used && hex(body[offset]) >= 0) {
          if (size > capacity / 16)
            return -4;
          size = size * 16 + hex(body[offset++]);
          digits++;
        }
        if (!digits)
          return -5;
        while (offset + 1 < used && body[offset] != '\r')
          offset++;
        if (offset + 1 >= used || body[offset + 1] != '\n')
          return -5;
        offset += 2;
        if (!size) {
          body[out] = 0;
          return out;
        }
        if (size > used - offset || used - offset - size < 2 ||
            body[offset + size] != '\r' || body[offset + size + 1] != '\n')
          return -5;
        memmove(body + out, body + offset, size);
        out += size;
        offset += size + 2;
      }
      return -5;
    }
    out = used - offset;
    memmove(body, body + offset, out);
    body[out] = 0;
    return out;
  }
  return -6;
}
