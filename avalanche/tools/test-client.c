#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/client.c"
static unsigned invalidations, copy_fail;
static uint8_t memory[65536];
static uint32_t surface[160*100];
void gui_canvas_invalidate(void) { invalidations++; }
void *pages_alloc(unsigned pages) { assert(pages<=sizeof(surface)/4096+1); return surface; }
bool page_free(void *page) { (void)page; return true; }
bool process_range(struct process *p, uint32_t a, uint32_t n, bool write) {
  (void)p; (void)write; return a<=sizeof(memory) && n<=sizeof(memory)-a;
}
bool process_copyin(struct process *p, void *out, uint32_t a, uint32_t n) {
  if (a==copy_fail || !process_range(p,a,n,false)) return false;
  memcpy(out,memory+a,n); return true;
}
bool process_copyout(struct process *p, uint32_t a, const void *in, uint32_t n) {
  if(!process_range(p,a,n,true)) return false;
  memcpy(memory+a,in,n); return true;
}
static void draw_request(struct av_gui_draw *draw) { memcpy(memory+64,draw,sizeof(*draw)); }
int main(void) {
  static struct process p; p.pid=23;
  struct av_gui_open open={.width=160,.height=100,.title="Test"};
  memcpy(memory+64,&open,sizeof(open));
  assert(client_call(&p,0,64)==-19); client_enable(true);
  assert(client_call(&p,0,64)==0);
  struct gui_client *c=&gui_clients[0];
  for(int i=0;i<500;i++) client_event(0,AV_GUI_MOUSE,i,i+1,0);
  assert(c->event_count==2 && c->events[0].a==0 && c->events[1].a==499);
  client_event(0,AV_GUI_MOUSE,500,501,1);
  client_event(0,AV_GUI_MOUSE,501,502,1);
  client_event(0,AV_GUI_MOUSE,502,503,0);
  client_event(0,AV_GUI_KEY,17,0,0);
  client_event(0,AV_GUI_MOUSE,503,504,0);
  assert(c->event_count==7 && c->events[2].c==1 && c->events[2].a==500);
  for(unsigned i=0;i<7;i++) assert(client_call(&p,2,128)==1);
  assert(client_call(&p,2,128)==0);
  for(unsigned i=0;i<128;i++) client_event(0,AV_GUI_KEY,i,0,0);
  client_event(0,AV_GUI_QUIT,0,0,0);
  assert(c->event_count==128 && c->closing);
  assert(c->events[(c->event_head+127)%128].type==AV_GUI_QUIT);
  puts("PASS pointer coalescing preserves button transitions and key/quit ordering");
  struct av_gui_draw draw={.x=0,.y=0,.width=160,.height=2,.stride=160,.pixels=1024};
  uint32_t *source=(void *)(memory+1024);
  for(unsigned i=0;i<320;i++) source[i]=0x20202e;
  draw_request(&draw); invalidations=0;
  assert(client_call(&p,1,64)==0 && !invalidations && !c->sequence);
  source[9]=0x123456; assert(client_call(&p,1,64)==0);
  assert(invalidations==1 && c->sequence==1 && surface[9]==0xff123456);
  source[9]=0xaa123456; assert(client_call(&p,1,64)==0 && invalidations==1);
  draw.width=0;draw.height=0;draw_request(&draw);
  assert(client_call(&p,1,64)==0 && invalidations==1);
  draw.width=160;draw.height=2;draw_request(&draw);
  source[2]=123; copy_fail=1024+160*4;
  assert(client_call(&p,1,64)==-14 && invalidations==2 && c->sequence==2);
  assert(surface[2]==0xff00007b);copy_fail=0;
  draw.width=161;draw_request(&draw);assert(client_call(&p,1,64)==-22);
  puts("PASS identical and empty draws skip redraw; changed and partially failed draws invalidate");
  return 0;
}
