#include "magenta.h"
#include "lib.h"
#define LIMIT 65536u
static char content[LIMIT + 1], draft[LIMIT + 1];
static struct av_status status;
static int call(unsigned n, unsigned a, unsigned b, unsigned c) {
  return av_call(n, a, b, c, 0);
}
static void color(int on) { call(AV_COLOR, (unsigned)on, 0, 0); }
static void error(int result) {
  puts_u("Operation failed (error ");
  uint_u((uint32_t)-result);
  puts_u(").\n");
}
static int input(const char *prompt, char *out, unsigned size) {
  puts_u(prompt);
  int n = call(AV_READLINE, (uintptr_t)out, size, 0);
  if (n < 0) {
    error(n);
    puts_u("Magenta requires an available foreground terminal.\n");
    call(AV_EXIT, 1, 0, 0);
    out[0] = 0;
  }
  return n;
}
static void pause_u(void) {
  char line[8];
  input("\nPress Enter to return... ", line, sizeof(line));
}
static void header(const char *title) {
  call(AV_CLEAR, 0, 0, 0);
  color(1);
  puts_u("============================================================\n  "
         "MAGENTA 0.1  |  ");
  puts_u(title);
  puts_u("\n============================================================\n");
  color(0);
  if (call(AV_STATUS, (uintptr_t)&status, 0, 0) < 0) {
    puts_u("System status unavailable.\n");
    return;
  }
  puts_u(status.mounted_disk < 0 ? "Storage: RAM / files disappear at shutdown"
                                 : "Storage: persistent disk");
  if (!status.writable)
    puts_u(" (read only)");
  puts_u("\n\n");
}
static int confirm(const char *word) {
  char line[32];
  puts_u("Type ");
  puts_u(word);
  puts_u(" to confirm: ");
  input("", line, sizeof(line));
  return ueq(word, line);
}
static int valid_name(const char *s) {
  unsigned n = ulen(s);
  if (!n || n > 31)
    return 0;
  for (unsigned i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return !ueq(s, ".") && !ueq(s, "..");
}
static void list_files(void) {
  unsigned count = 0;
  for (unsigned i = 0; i < 32; i++) {
    struct av_file_entry e;
    int r = call(AV_FILE_ENTRY, i, (uintptr_t)&e, 0);
    if (r < 0) {
      error(r);
      return;
    }
    if (!r)
      continue;
    puts_u("  ");
    puts_u(e.name);
    puts_u("  [");
    uint_u(e.size);
    puts_u(" bytes]\n");
    count++;
  }
  if (!count)
    puts_u("No files yet.\n");
}
static int filename_u(char *name) {
  input("Filename (or Enter to cancel): ", name, 32);
  if (!name[0])
    return 0;
  if (!valid_name(name)) {
    puts_u("Use 1-31 letters, digits, dots, underscores or dashes.\n");
    return 0;
  }
  return 1;
}
static int text_file(unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    unsigned c = (unsigned char)content[i];
    if (c != 9 && c != 10 && c != 13 && (c < 32 || c > 126))
      return 0;
  }
  return 1;
}
static void show_file(unsigned n) {
  if (text_file(n)) {
    content[n] = 0;
    puts_u(content);
    puts_u("\n");
    return;
  }
  puts_u("Binary data (first 256 bytes):\n");
  unsigned count = n > 256 ? 256 : n;
  for (unsigned i = 0; i < count; i++) {
    char hex[4] = {"0123456789ABCDEF"[((unsigned char)content[i]) >> 4],
                   "0123456789ABCDEF"[((unsigned char)content[i]) & 15], ' ',
                   0};
    puts_u(hex);
    if (i % 16 == 15)
      puts_u("\n");
  }
  puts_u("\n");
}
static void editor(const char *name, int editing) {
  unsigned length = 0;
  if (editing) {
    int n = read_u(name, content, LIMIT);
    if (n < 0) {
      error(n);
      return;
    }
    if (!text_file((unsigned)n)) {
      puts_u("The text editor cannot edit binary data or unsupported "
             "encodings.\n");
      return;
    }
    content[n] = 0;
    puts_u("\nExisting text:\n");
    puts_u(content);
    puts_u("\n\n[R] Replace  [A] Append  [Enter] Cancel\n");
    char mode[8];
    input("Edit mode: ", mode, sizeof(mode));
    if (ueq(mode, "A") || ueq(mode, "a")) {
      length = (unsigned)n;
      for (unsigned i = 0; i < length; i++)
        draft[i] = content[i];
    } else if (!ueq(mode, "R") && !ueq(mode, "r"))
      return;
  }
  puts_u("\nEnter text, one line at a time (511 characters per line).\n.save "
         "saves; .cancel discards; ..save inserts a literal .save line.\n");
  for (;;) {
    char line[512];
    int n = input("| ", line, sizeof(line));
    if (n < 0)
      return;
    if (ueq(line, ".cancel")) {
      puts_u("Changes discarded.\n");
      return;
    }
    if (ueq(line, ".save"))
      break;
    const char *text = line;
    if (line[0] == '.' && line[1] == '.') {
      text++;
      n--;
    }
    if ((unsigned)n + 1 > LIMIT - length) {
      puts_u("File limit reached (64 KiB). Save or cancel.\n");
      continue;
    }
    for (int i = 0; i < n; i++)
      draft[length++] = text[i];
    draft[length++] = '\n';
  }
  int n = write_u(name, draft, length);
  if (n < 0)
    error(n);
  else {
    puts_u("Saved ");
    uint_u((unsigned)n);
    puts_u(status.mounted_disk < 0 ? " bytes in RAM.\n" : " bytes to disk.\n");
  }
}
static void files(void) {
  for (;;) {
    header("Files");
    list_files();
    puts_u("\n1 Read  2 Create  3 Edit  4 Delete  0 Back\n");
    char choice[8], name[32];
    input("Files> ", choice, sizeof(choice));
    if (ueq(choice, "0"))
      return;
    if (ueq(choice, "1")) {
      if (filename_u(name)) {
        int n = read_u(name, content, LIMIT);
        if (n < 0)
          error(n);
        else {
          puts_u("\n");
          show_file((unsigned)n);
        }
      }
    } else if (ueq(choice, "2")) {
      if (filename_u(name)) {
        int n = call(AV_FILE_SIZE, (uintptr_t)name, 0, 0);
        if (n >= 0) {
          puts_u("A file with that name already exists. Use Edit.\n");
        } else
          editor(name, 0);
      }
    } else if (ueq(choice, "3")) {
      if (filename_u(name))
        editor(name, 1);
    } else if (ueq(choice, "4")) {
      if (filename_u(name)) {
        puts_u("Delete ");
        puts_u(name);
        puts_u("?\n");
        if (confirm("DELETE")) {
          int r = call(AV_FILE_REMOVE, (uintptr_t)name, 0, 0);
          if (r < 0)
            error(r);
          else
            puts_u("Deleted.\n");
        }
      }
    } else
      puts_u("Choose a listed option.\n");
    pause_u();
  }
}
static void launch(const char *name, const char *args, int foreground) {
  int pid =
      call(AV_SPAWN, (uintptr_t)name, (uintptr_t)args, (unsigned)foreground);
  if (pid < 0) {
    error(pid);
    return;
  }
  puts_u("Started PID ");
  uint_u((unsigned)pid);
  puts_u("\n");
  if (!foreground)
    return;
  uint32_t exit_status = 0;
  int result = call(AV_WAIT, (unsigned)pid, (uintptr_t)&exit_status, 0);
  if (result < 0)
    error(result);
  else if (exit_status & 127) {
    puts_u("Terminated by signal ");
    uint_u(exit_status & 127);
    puts_u(".\n");
  } else {
    puts_u("Program exit code: ");
    uint_u((exit_status >> 8) & 255);
    puts_u("\n");
  }
}
static void programs(void) {
  for (;;) {
    header("Programs");
    puts_u("Bundled: ush, hello, cat, udp, http, worker, linuxhello, "
           "linuxfork\nOther ELF programs stored on disk may also be "
           "launched.\n\n");
    list_files();
    puts_u("\n1 Run foreground  2 Run background  0 Back\n");
    char choice[8], name[32], args[256];
    input("Programs> ", choice, sizeof(choice));
    if (ueq(choice, "0"))
      return;
    if (!ueq(choice, "1") && !ueq(choice, "2")) {
      puts_u("Choose a listed option.\n");
      pause_u();
      continue;
    }
    if (filename_u(name)) {
      if (ueq(name, "magenta")) {
        puts_u("Magenta is already running.\n");
      } else {
        input("Arguments (optional): ", args, sizeof(args));
        launch(name, args, ueq(choice, "1"));
      }
    }
    pause_u();
  }
}
static void processes(void) {
  for (;;) {
    header("Processes");
    call(AV_REPORT, AV_REPORT_PROCESSES, 0, 0);
    puts_u("\n1 Refresh  2 Wait for a child  3 Terminate a process  0 Back\n");
    char choice[8], line[16];
    input("Processes> ", choice, sizeof(choice));
    if (ueq(choice, "0"))
      return;
    if (ueq(choice, "1"))
      continue;
    if (!ueq(choice, "2") && !ueq(choice, "3")) {
      puts_u("Choose a listed option.\n");
      pause_u();
      continue;
    }
    input("PID: ", line, sizeof(line));
    unsigned pid = decimal_u(line);
    if (!pid) {
      puts_u("Enter a positive process ID.\n");
      pause_u();
      continue;
    }
    if (ueq(choice, "2")) {
      uint32_t exit_status = 0;
      int r = call(AV_WAIT, pid, (uintptr_t)&exit_status, 0);
      if (r < 0)
        error(r);
      else {
        puts_u("Child completed; wait status ");
        uint_u(exit_status);
        puts_u(".\n");
      }
    } else if (pid == (unsigned)call(AV_GETPID, 0, 0, 0))
      puts_u("Use the power menu to leave Magenta.\n");
    else if (confirm("KILL")) {
      int r = call(AV_KILL, pid, 9, 0);
      if (r < 0)
        error(r);
      else
        puts_u("Termination requested.\n");
    }
    pause_u();
  }
}
static int read_address(const char *prompt, uint32_t *out) {
  char line[32];
  input(prompt, line, sizeof(line));
  if (ueq(line, "0.0.0.0")) {
    *out = 0;
    return 1;
  }
  *out = address_u(line);
  if (!*out) {
    puts_u("Invalid IPv4 address.\n");
    return 0;
  }
  return 1;
}
static void print_address(uint32_t address) {
  for (int i = 3; i >= 0; i--) {
    uint_u((address >> (i * 8)) & 255);
    if (i)
      puts_u(".");
  }
}
static void network(void) {
  for (;;) {
    header("Network");
    call(AV_REPORT, AV_REPORT_NETWORK, 0, 0);
    puts_u("\n1 DHCP  2 Static configuration  3 Ping  4 DNS lookup\n5 HTTP "
           "request  0 Back\n");
    char choice[8];
    input("Network> ", choice, sizeof(choice));
    if (ueq(choice, "0"))
      return;
    if (ueq(choice, "1")) {
      puts_u("Requesting a DHCP lease...\n");
      int r = call(AV_DHCP, 0, 0, 0);
      if (r < 0)
        error(r);
      else
        puts_u("Lease acquired.\n");
      call(AV_REPORT, AV_REPORT_NETWORK, 0, 0);
    } else if (ueq(choice, "2")) {
      uint32_t cfg[4];
      if (read_address("Address: ", &cfg[0]) &&
          read_address("Netmask: ", &cfg[1]) &&
          read_address("Gateway (0.0.0.0 for none): ", &cfg[2]) &&
          read_address("DNS server (0.0.0.0 for none): ", &cfg[3])) {
        uint32_t inv = ~cfg[1];
        if (!cfg[0] || (inv & (inv + 1))) {
          puts_u("Use a nonzero host address and a contiguous netmask.\n");
        } else {
          int r = call(AV_NET_CONFIG, (uintptr_t)cfg, 0, 0);
          if (r < 0)
            error(r);
          else
            puts_u("Configuration applied for this session.\n");
        }
      }
    } else if (ueq(choice, "3")) {
      uint32_t ip;
      if (read_address("Destination IPv4: ", &ip)) {
        int r = call(AV_PING, ip, 0, 0);
        if (r < 0)
          error(r);
        else {
          puts_u("Reply in ");
          uint_u((unsigned)r);
          puts_u(" ms.\n");
        }
      }
    } else if (ueq(choice, "4")) {
      char host[128];
      uint32_t ip;
      input("Hostname: ", host, sizeof(host));
      int r = call(AV_DNS, (uintptr_t)host, (uintptr_t)&ip, 0);
      if (r < 0)
        error(r);
      else {
        print_address(ip);
        puts_u("\n");
      }
    } else if (ueq(choice, "5")) {
      char host[128], path[96], args[256];
      input("Host (HTTP port 80): ", host, sizeof(host));
      input("Path [/]: ", path, sizeof(path));
      if (!path[0]) {
        path[0] = '/';
        path[1] = 0;
      }
      unsigned n = 0;
      for (unsigned i = 0; host[i] && n + 1 < sizeof(args); i++)
        args[n++] = host[i];
      args[n++] = ' ';
      for (unsigned i = 0; path[i] && n + 1 < sizeof(args); i++)
        args[n++] = path[i];
      args[n] = 0;
      launch("http", args, 1);
    } else
      puts_u("Choose a listed option.\n");
    pause_u();
  }
}
static void system_menu(void) {
  for (;;) {
    header("System");
    puts_u("Magenta 0.1 / Avalanche Kernel 0.4.0\n64-bit x86-64, BIOS and x86-64 "
           "UEFI\n\nUptime: ");
    uint_u(status.ticks / 100);
    puts_u(" seconds\nManaged memory: ");
    uint_u(status.managed_kib);
    puts_u(" KiB\nAllocated memory: ");
    uint_u(status.allocated_kib);
    puts_u(" KiB\nDetected storage devices: ");
    uint_u(status.disks);
    puts_u("\n\n");
    call(AV_REPORT, AV_REPORT_FILESYSTEM, 0, 0);
    puts_u("\n1 Detected devices  2 Firmware date/time  0 Back\n");
    char choice[8];
    input("System> ", choice, sizeof(choice));
    if (ueq(choice, "0"))
      return;
    if (ueq(choice, "1"))
      call(AV_REPORT, AV_REPORT_DEVICES, 0, 0);
    else if (ueq(choice, "2"))
      call(AV_REPORT, AV_REPORT_DATE, 0, 0);
    else
      puts_u("Choose a listed option.\n");
    pause_u();
  }
}
static int power_menu(void) {
  header("Power");
  puts_u("1 Restart  2 Halt  3 Rescue console  0 Back\n");
  char choice[8];
  input("Power> ", choice, sizeof(choice));
  if (ueq(choice, "1") && confirm("RESTART")) {
    int r = call(AV_POWER, AV_POWER_REBOOT, 0, 0);
    if (r < 0)
      error(r);
    pause_u();
  } else if (ueq(choice, "2") && confirm("HALT")) {
    call(AV_POWER, AV_POWER_HALT, 0, 0);
  } else if (ueq(choice, "3") && confirm("RESCUE"))
    return 1;
  return 0;
}
int main(void) {
  for (;;) {
    header("Home");
    puts_u("A small text-based operating system.\n\n1  Files       Read, "
           "create, edit and delete\n2  Programs    Run applications\n3  "
           "Processes   View and manage tasks\n4  Network     Configure and "
           "connect\n5  System      Devices, memory and uptime\n6  Shell       "
           "Open a command prompt\n0  Power       Restart, halt or rescue\n\n");
    char choice[8];
    input("Magenta> ", choice, sizeof(choice));
    if (ueq(choice, "1"))
      files();
    else if (ueq(choice, "2"))
      programs();
    else if (ueq(choice, "3"))
      processes();
    else if (ueq(choice, "4"))
      network();
    else if (ueq(choice, "5"))
      system_menu();
    else if (ueq(choice, "6")) {
      header("Shell");
      puts_u("Type exit to return to Magenta.\n\n");
      launch("ush", "", 1);
      pause_u();
    } else if (ueq(choice, "0")) {
      if (power_menu()) {
        color(0);
        return 0;
      }
    } else {
      puts_u("Choose a listed option.\n");
      pause_u();
    }
  }
}
