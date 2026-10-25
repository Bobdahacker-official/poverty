#include "lib.h"
static char data[65537];
int main(void) {
  char line[512];
  puts_u("Avalanche userspace shell (ring 3). Type help.\n");
  for (;;) {
    puts_u("user> ");
    if (av_call(AV_READLINE, (uintptr_t)line, sizeof(line), 0, 0) < 0)
      return 1;
    char *rest = line, *cmd = word_u(&rest);
    if (!*cmd)
      continue;
    if (ueq(cmd, "exit"))
      return 0;
    if (ueq(cmd, "help"))
      puts_u("help echo ls cat write rm ticks sleep udp recv spawn wait kill "
             "pid exit\nwrite NAME "
             "TEXT; udp IP PORT TEXT; recv LOCALPORT\n");
    else if (ueq(cmd, "spawn")) {
      char *name = word_u(&rest);
      int pid = av_call(AV_SPAWN, (uintptr_t)name, (uintptr_t)rest, 0, 0);
      if (pid < 0)
        puts_u("Spawn failed.\n");
      else {
        puts_u("Started PID ");
        uint_u((uintptr_t)pid);
        puts_u("\n");
      }
    } else if (ueq(cmd, "wait")) {
      int r = av_call(AV_WAIT, decimal_u(rest), 0, 0, 0);
      puts_u(r < 0 ? "Wait failed.\n" : "Child completed.\n");
    } else if (ueq(cmd, "kill")) {
      puts_u(av_call(AV_KILL, decimal_u(rest), 9, 0, 0) < 0 ? "Kill failed.\n"
                                                            : "Terminated.\n");
    } else if (ueq(cmd, "pid")) {
      uint_u((uintptr_t)av_call(AV_GETPID, 0, 0, 0, 0));
      puts_u("\n");
    } else if (ueq(cmd, "echo")) {
      puts_u(rest);
      puts_u("\n");
    } else if (ueq(cmd, "ls"))
      av_call(AV_FILE_LIST, 0, 0, 0, 0);
    else if (ueq(cmd, "cat")) {
      char *name = word_u(&rest);
      int n = read_u(name, data, 65536);
      if (n < 0)
        puts_u("Read failed.\n");
      else {
        data[n] = 0;
        puts_u(data);
        puts_u("\n");
      }
    } else if (ueq(cmd, "write")) {
      char *name = word_u(&rest);
      puts_u(write_u(name, rest, ulen(rest)) < 0 ? "Write failed.\n"
                                                 : "Saved.\n");
    } else if (ueq(cmd, "rm")) {
      char *name = word_u(&rest);
      puts_u(av_call(AV_FILE_REMOVE, (uintptr_t)name, 0, 0, 0) < 0
                 ? "Remove failed.\n"
                 : "Removed.\n");
    } else if (ueq(cmd, "ticks")) {
      uint_u((uintptr_t)av_call(AV_TICKS, 0, 0, 0, 0));
      puts_u("\n");
    } else if (ueq(cmd, "sleep")) {
      uint32_t n = decimal_u(rest);
      if (n > 1000)
        puts_u("Maximum 1000 ticks.\n");
      else
        av_call(AV_SLEEP, n, 0, 0, 0);
    } else if (ueq(cmd, "udp")) {
      char *host = word_u(&rest), *port = word_u(&rest);
      uint32_t p = decimal_u(port), ip = address_u(host);
      if (!ip || !p || p > 65535 ||
          send_u(ip, 49152, (uint16_t)p, rest, ulen(rest)) < 0)
        puts_u("UDP send failed.\n");
      else
        puts_u("Sent.\n");
    } else if (ueq(cmd, "recv")) {
      uint32_t port = decimal_u(rest), source;
      uint16_t remote;
      int n = port && port <= 65535
                  ? av_recv((uint16_t)port, data, 1472, &source, &remote)
                  : -1;
      if (n < 0)
        puts_u("No datagram available.\n");
      else {
        data[n] = 0;
        puts_u(data);
        puts_u("\n");
      }
    } else
      puts_u("Unknown command.\n");
  }
}
