#ifndef AVALANCHE_ABI_H
#define AVALANCHE_ABI_H
/* i386 int 0x80: EAX number/result; EBX, ECX, EDX, ESI arguments.
 * Negative results are errors. Programs are static ELF32 ET_EXEC images. */
enum {
  AV_EXIT,
  AV_WRITE,
  AV_READLINE,
  AV_TICKS,
  AV_FILE_READ,
  AV_FILE_WRITE,
  AV_FILE_REMOVE,
  AV_FILE_LIST,
  AV_SLEEP,
  AV_UDP_SEND,
  AV_UDP_RECV,
  AV_ARGS,
  AV_FILE_SIZE,
  AV_YIELD,
  AV_TCP_CONNECT,
  AV_TCP_SEND,
  AV_TCP_RECV,
  AV_TCP_CLOSE,
  AV_DNS,
  AV_SPAWN,
  AV_WAIT,
  AV_GETPID,
  AV_KILL
};
#define AV_USER_BASE 0x40000000u
#define AV_USER_END 0x40400000u
#endif
