#include <magenta/app.h>
int main(void) {
  struct av_status status;
  mg_write("Hello from a native Magenta application!\n");
  if(mg_status(&status)<0) return 1;
  mg_write("PID: ");uint_u(mg_pid());mg_write("\nUptime: ");
  uint_u(status.ticks/100);mg_write(" seconds\n");
  return 0;
}
