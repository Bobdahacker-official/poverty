#ifndef AV_UNIX_TOOLS_H
#define AV_UNIX_TOOLS_H
static int unix_command(const char *cmd, char *args, char *data) {
  if (ueq(cmd, "ps"))
    av_call(AV_REPORT, AV_REPORT_PROCESSES, 0, 0, 0);
  else if (ueq(cmd, "uname"))
    puts_u("Avalanche x86_64\n");
  else if (ueq(cmd, "pwd"))
    puts_u("/\n");
  else if (ueq(cmd, "clear"))
    av_call(AV_CLEAR, 0, 0, 0, 0);
  else if (ueq(cmd, "date"))
    av_call(AV_REPORT, AV_REPORT_DATE, 0, 0, 0);
  else if (ueq(cmd, "free") || ueq(cmd, "uptime")) {
    struct av_status status;
    if (av_call(AV_STATUS, (uintptr_t)&status, 0, 0, 0) < 0)
      puts_u("Status unavailable.\n");
    else if (ueq(cmd, "uptime")) {
      uint_u(status.ticks / 100);
      puts_u(" seconds\n");
    } else {
      puts_u("Total KiB: ");
      uint_u(status.managed_kib);
      puts_u("\nUsed KiB: ");
      uint_u(status.allocated_kib);
      puts_u("\nFree KiB: ");
      uint_u(status.managed_kib - status.allocated_kib);
      puts_u("\n");
    }
  } else if (ueq(cmd, "mkdir")) {
    puts_u(av_call(AV_FILE_MKDIR, (uintptr_t)args, 0, 0, 0) < 0
               ? "mkdir failed.\n"
               : "Directory created.\n");
  } else if (ueq(cmd, "mv") || ueq(cmd, "cp")) {
    char *source = word_u(&args), *destination = word_u(&args);
    puts_u(av_call(ueq(cmd, "mv") ? AV_FILE_RENAME : AV_FILE_COPY,
                   (uintptr_t)source, (uintptr_t)destination, 0, 0) < 0
               ? "File operation failed.\n"
               : "Done.\n");
  } else if (ueq(cmd, "wc") || ueq(cmd, "head") || ueq(cmd, "grep")) {
    char *pattern = ueq(cmd, "grep") ? word_u(&args) : 0;
    int n = read_u(args, data, 65536);
    if (n < 0) {
      puts_u("Read failed.\n");
      return 1;
    }
    if (ueq(cmd, "wc")) {
      unsigned lines = 0, words = 0, inword = 0;
      for (int i = 0; i < n; i++) {
        lines += data[i] == '\n';
        unsigned text = data[i] != ' ' && data[i] != '\t' && data[i] != '\r' &&
                        data[i] != '\n';
        if (text && !inword)
          words++;
        inword = text;
      }
      uint_u(lines);
      puts_u(" lines ");
      uint_u(words);
      puts_u(" words ");
      uint_u(n);
      puts_u(" bytes\n");
    } else {
      unsigned printed = 0, start = 0;
      while (start < (unsigned)n) {
        unsigned end = start;
        while (end < (unsigned)n && data[end] != '\n')
          end++;
        unsigned match = !pattern || !*pattern;
        if (pattern)
          for (unsigned at = start; at < end && !match; at++) {
            unsigned k = 0;
            while (at + k < end && pattern[k] && data[at + k] == pattern[k])
              k++;
            if (!pattern[k])
              match = 1;
          }
        if (match) {
          char saved = data[end];
          data[end] = 0;
          puts_u(data + start);
          puts_u("\n");
          data[end] = saved;
          printed++;
        }
        if (!pattern && printed == 10)
          break;
        start = end + 1;
      }
    }
  } else
    return 0;
  return 1;
}
#endif
