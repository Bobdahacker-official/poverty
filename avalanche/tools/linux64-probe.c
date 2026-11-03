#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static __thread unsigned tls_value = 0x12345678;
static void check(int condition, const char *name) {
  if (!condition) { perror(name); exit(1); }
  printf("PASS Linux64 %s\n", name);
  fflush(stdout);
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "exec-child")) {
    check(tls_value == 0x12345678, "exec restores TLS");
    return 23;
  }
  check(argc == 3 && !strcmp(argv[1], "alpha") && !strcmp(argv[2], "beta"),
        "glibc startup and argv");
  check(getenv("PATH") != NULL, "environment");
  struct utsname uts;
  check(!uname(&uts) && !strcmp(uts.machine, "x86_64"), "uname");
  struct rlimit limit;
  check(!getrlimit(RLIMIT_AS, &limit) && limit.rlim_cur >= 32*1024*1024,
        "resource limits");
  char *heap = malloc(1024*1024);
  check(heap != NULL, "malloc");
  memset(heap, 0x5a, 1024*1024);
  check(heap[12345] == 0x5a, "heap memory");
  free(heap);
  unsigned *mapped = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  check(mapped != MAP_FAILED && !*mapped, "anonymous demand paging");
  *mapped = 71;
  check(!mprotect(mapped,4096,PROT_READ), "mprotect");
  check(!munmap(mapped,4096), "munmap");
  int fd = open("/linux-data.txt", O_CREAT|O_TRUNC|O_RDWR, 0644);
  check(fd >= 0, "open");
  const char data[] = "Linux applications on Avalanche\n";
  check(write(fd,data,sizeof(data)) == sizeof(data) && lseek(fd,0,SEEK_SET)==0,
        "write and seek");
  char bytes[128] = {0};
  check(read(fd,bytes,sizeof(bytes)) == sizeof(data) && !memcmp(bytes,data,sizeof(data)),
        "read");
  struct stat st;
  check(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==sizeof(data) &&
        st.st_ino && st.st_nlink==1, "ext4 stat");
  check(!close(fd), "close");
  errno=0;
  check(syscall(SYS_write,1,(uintptr_t)bytes|UINT64_C(0xffff000000000000),1)==-1 && errno==EFAULT,
        "high pointers cannot alias low mappings");
  struct timespec before,after;
  check(!clock_gettime(CLOCK_MONOTONIC,&before), "monotonic clock");
  struct timespec sleep = {.tv_nsec=30000000};
  check(!nanosleep(&sleep,NULL) && !clock_gettime(CLOCK_MONOTONIC,&after) &&
        (after.tv_sec-before.tv_sec)*1000000000+after.tv_nsec-before.tv_nsec>=30000000,
        "nanosleep");
  check(time(NULL)>1700000000, "realtime clock");
  unsigned char random[32];
  check(getrandom(random,sizeof(random),0)==sizeof(random), "entropy");
  pid_t child=fork();
  check(child>=0, "fork");
  if(!child) {
    if(tls_value!=0x12345678) _exit(2);
    tls_value=17;
    char *args[]={argv[0],"exec-child",NULL};
    execv(argv[0],args);
    _exit(3);
  }
  int status=0;
  check(waitpid(child,&status,0)==child && WIFEXITED(status) &&
        WEXITSTATUS(status)==23 && tls_value==0x12345678, "fork exec wait and TLS isolation");
  check(!unlink("/linux-data.txt"), "unlink");
  puts("LINUX64 COMPLETE");
  return 0;
}
