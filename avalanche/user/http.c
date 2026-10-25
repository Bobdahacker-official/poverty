#include "lib.h"
static char response[4097];
int main(void) {
  char args[256];
  args_u(args, sizeof(args));
  char *rest = args, *host = word_u(&rest), *path = word_u(&rest);
  if (!*path)
    path = "/";
  if (!*host || *path != '/') {
    puts_u("Usage: run http HOST /PATH (HTTP port 80)\n");
    return 1;
  }
  uint32_t address;
  if (av_call(AV_DNS, (uintptr_t)host, (uintptr_t)&address, 0, 0) < 0) {
    puts_u("DNS resolution failed.\n");
    return 1;
  }
  if (av_call(AV_TCP_CONNECT, address, 80, 0, 0) < 0) {
    puts_u("Connection failed.\n");
    return 1;
  }
  char request[512];
  unsigned n = 0;
  const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host,
                         "\r\nConnection: close\r\n\r\n"};
  for (unsigned i = 0; i < 5; i++) {
    const char *s = parts[i];
    while (*s && n < sizeof(request))
      request[n++] = *s++;
    if (*s) {
      puts_u("Request too long.\n");
      av_call(AV_TCP_CLOSE, 0, 0, 0, 0);
      return 1;
    }
  }
  if (av_call(AV_TCP_SEND, (uintptr_t)request, n, 0, 0) < 0) {
    puts_u("Send failed.\n");
    av_call(AV_TCP_CLOSE, 0, 0, 0, 0);
    return 1;
  }
  unsigned total = 0;
  int result = 0;
  while (total < 65536) {
    int count = av_call(AV_TCP_RECV, (uintptr_t)response, 4096, 500, 0);
    if (count < 0) {
      puts_u("\nReceive failed.\n");
      result = 1;
      break;
    }
    if (!count)
      break;
    response[count] = 0;
    puts_u(response);
    total += (unsigned)count;
  }
  av_call(AV_TCP_CLOSE, 0, 0, 0, 0);
  return result;
}
