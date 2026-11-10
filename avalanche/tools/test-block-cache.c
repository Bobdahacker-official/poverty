#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/block_cache.c"
static struct block_device disks[2] = {{.sectors=8192}, {.sectors=8192}};
static unsigned reads, writes, flushes;
static bool fail_read, fail_write, fail_flush;
static uint8_t backing[2][8192][512];
const struct block_device *disk_get(unsigned d) { return d < 2 ? &disks[d] : NULL; }
bool disk_raw_read(unsigned d, uint32_t s, void *out) {
  reads++;
  if (fail_read) return false;
  memcpy(out, backing[d][s], 512); return true;
}
bool disk_raw_write(unsigned d, uint32_t s, const void *in) {
  writes++;
  if (fail_write) return false;
  memcpy(backing[d][s], in, 512); return true;
}
bool disk_raw_flush(unsigned d) { (void)d; flushes++; return !fail_flush; }
int main(void) {
  uint8_t data[512], out[512]; memset(data, 73, sizeof(data));
  assert(!disk_read(2,0,out) && !disk_read(0,8192,out));
  disks[1].read_only=1; assert(!disk_write(1,0,data)); disks[1].read_only=0;
  assert(disk_write(0,7,data) && reads==0 && writes==0);
  assert(disk_read(0,7,out) && !memcmp(data,out,512) && reads==0);
  assert(disk_read(1,7,out) && out[0]==0 && reads==1);
  fail_write=true; assert(!disk_flush(0)); fail_write=false;
  assert(disk_flush(0) && backing[0][7][0]==73);
  fail_flush=true; assert(!disk_flush(0)); fail_flush=false; assert(disk_flush(0));
  puts("PASS writeback, disk isolation, bounds, read-only and flush retries");
  memset(cache,0,sizeof(cache)); memset(cursors,0,sizeof(cursors)); reads=0;
  for(unsigned s=0;s<1024;s++) assert(disk_read(0,s,out));
  unsigned initial=reads;
  for(unsigned s=0;s<1024;s++) assert(disk_read(0,s,out));
  assert(reads==initial);
  puts("PASS 512 KiB working set stays cached on repeated reads");
  memset(cache,0,sizeof(cache)); memset(cursors,0,sizeof(cursors));
  unsigned sectors[6], count=0;
  for(unsigned s=0;s<8192 && count<6;s++)
    if(cache_set(0,s)==0) sectors[count++]=s;
  assert(count==6);
  for(unsigned i=0;i<4;i++) assert(disk_write(0,sectors[i],data));
  fail_write=true; assert(!disk_read(0,sectors[4],out));
  assert(disk_read(0,sectors[0],out) && out[0]==73);
  fail_write=false; fail_read=true; assert(!disk_read(0,sectors[4],out));
  fail_read=false; assert(disk_read(0,sectors[4],out));
  assert(disk_flush(0));
  for(unsigned i=0;i<4;i++) assert(backing[0][sectors[i]][0]==73);
  puts("PASS colliding-set eviction preserves dirty data through I/O failures");
  return 0;
}
