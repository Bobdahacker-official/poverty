#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ip_fragment.c"
static uint32_t tick;static unsigned delivered;static uint8_t source[3000];
uint32_t clock_ticks(void) {return tick;}
static void receive_packet(const uint8_t *p,uint32_t n) {
  assert(n==3034 && be16(p+16)==3020 && be16(p+20)==0);
  assert(!memcmp(p+34,source,3000));delivered++;
}
static void fragment(unsigned id,unsigned offset,unsigned size,bool more) {
  uint8_t frame[1514]={0};frame[12]=8;frame[14]=0x45;put16(frame+16,20+size);put16(frame+18,id);put16(frame+20,offset/8|(more?0x2000:0));frame[23]=17;
  frame[26]=10;frame[29]=1;frame[30]=10;frame[33]=2;
  memcpy(frame+34,source+offset,size);ip4_reassemble(frame,34+size,receive_packet);
}
int main(void) {
  for(unsigned i=0;i<3000;i++)source[i]=i*13;
  fragment(1,2960,40,false);fragment(1,1480,1480,true);fragment(1,0,1480,true);assert(delivered==1);
  fragment(2,0,1480,true);fragment(2,8,16,true);fragment(2,1480,1480,true);fragment(2,2960,40,false);assert(delivered==1);
  tick=3001;fragment(2,0,1480,true);fragment(2,1480,1480,true);fragment(2,2960,40,false);assert(delivered==2);
  puts("PASS reverse-order IPv4 reassembly, overlap rejection, tombstones and expiry");
  return 0;
}
