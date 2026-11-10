#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/gui_canvas.c"
static unsigned updates;
static uint32_t screen[97*65];
void graphics_present_region(const uint32_t *source, unsigned w, unsigned h,
                             unsigned x, unsigned y, unsigned cw, unsigned ch) {
  assert(w==width && h==height && x+cw<=w && y+ch<=h);
  updates++;
  for(unsigned row=y;row<y+ch;row++)
    memcpy(screen+row*w+x,source+row*w+x,cw*sizeof(uint32_t));
}
int main(void) {
  static uint32_t current[97*65], previous[97*65];
  pixels=current; presented_pixels=previous; width=97; height=65;
  scene_repaint=first_present=true; gui_canvas_present();
  assert(updates==3 && presented_pixel_count==97*65);
  updates=0; presented_pixel_count=0;
  current[0]=1; current[96]=2; current[64*97+96]=3;
  scene_repaint=true; gui_canvas_present();
  assert(updates==3 && presented_pixel_count==32*32+32+1);
  assert(!memcmp(screen,current,sizeof(screen)));
  assert(!memcmp(previous,current,sizeof(previous)));
  updates=0; scene_repaint=true; gui_canvas_present(); assert(updates==0);
  for(unsigned i=0;i<97;i++) current[i]=4;
  updates=0; scene_repaint=true; gui_canvas_present(); assert(updates==1);
  assert(!memcmp(screen,current,sizeof(screen)));
  puts("PASS first frame, disjoint damage, edge tiles, idle frames and coalescing");
  uint32_t random=37;
  for(unsigned frame=0;frame<256;frame++) {
    for(unsigned i=0;i<17;i++) {
      random=random*1664525u+1013904223u;
      current[random/(uint32_t)97 % (97*65)]=random;
    }
    scene_repaint=true; gui_canvas_present();
    assert(!memcmp(screen,current,sizeof(screen)));
    assert(!memcmp(previous,current,sizeof(previous)));
  }
  puts("PASS 256 sparse damage frames match the complete framebuffer");
  return 0;
}
