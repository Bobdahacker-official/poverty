#include "lib.h"
int main(void) {
  char args[256];
  args_u(args, sizeof(args));
  char *rest = args, *host = word_u(&rest), *port = word_u(&rest);
  uint32_t ip = address_u(host), number = decimal_u(port);
  if (!ip || !number || number > 65535) {
    puts_u("Usage: run udp IP PORT MESSAGE\n");
    return 1;
  }
  int n = send_u(ip, 49152, (uint16_t)number, rest, ulen(rest));
  if (n < 0) {
    puts_u("UDP send failed; configure networking first.\n");
    return 1;
  }
  puts_u("Sent ");
  uint_u((uintptr_t)n);
  puts_u(" bytes.\n");
  return 0;
}
