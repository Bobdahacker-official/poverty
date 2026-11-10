#ifndef AV_SANDBOX_H
#define AV_SANDBOX_H
#include "magenta.h"
#include <stdbool.h>
#include <stddef.h>
struct process;
struct process *process_current(void);
void sandbox_init(void);
uint32_t sandbox_image(const void *, unsigned, uint8_t[32]);
int sandbox_profile(const uint8_t[32], uint32_t);
bool sandbox_native(struct process *, unsigned);
bool sandbox_linux(struct process *, unsigned, bool);
bool sandbox_has(const struct process *, uint32_t);
bool sandbox_write_allowed(const char *);
void sandbox_root(const uint8_t[32], char[80]);
void sandbox_command(char *);
int fs_appdata_prepare(const char *);
void user_sandbox_report(void);
int user_sandbox_limit(int, uint32_t);
int user_spawn_sandbox(const char *, const char *, bool, uint32_t);
#endif
