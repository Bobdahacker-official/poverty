#include "games.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  struct mines_game m;
  for(unsigned difficulty=0;difficulty<3;difficulty++) {
    for(unsigned seed=1;seed<=128;seed++) {
      mines_new(&m,difficulty,seed);
      unsigned x=seed%m.width,y=seed%m.height;
      assert(mines_reveal(&m,x,y));
      assert(m.state==MINE_PLAYING && m.cells[y*m.width+x]==0);
      unsigned bombs=0;
      for(unsigned i=0;i<m.width*m.height;i++) bombs+=m.cells[i]==9;
      assert(bombs==m.mines);
      for(unsigned i=0;i<m.width*m.height;i++)
        if(m.cells[i]!=9) mines_reveal(&m,i%m.width,i/m.width);
      assert(m.state==MINE_WON && m.flags==m.mines);
    }
  }
  puts("PASS Minesweeper first-click safety, exact mine counts and winning all difficulties");
  mines_new(&m,0,1);mines_reveal(&m,4,4);
  for(unsigned i=0;i<81;i++) if(m.cells[i]==9) {
    assert(mines_flag(&m,i%9,i/9));
    assert(!mines_reveal(&m,i%9,i/9));
    assert(mines_flag(&m,i%9,i/9));
    assert(mines_reveal(&m,i%9,i/9) && m.state==MINE_LOST && m.exploded==(int)i);
    break;
  }
  assert(!mines_flag(&m,0,0));
  puts("PASS Minesweeper flags, mine collision and stopped game");
  mines_new(&m,0,27);mines_reveal(&m,4,4);
  for(unsigned i=0;i<81;i++) if(m.cells[i]==9) mines_flag(&m,i%9,i/9);
  for(unsigned i=0;i<81;i++) if(m.opened[i]) mines_chord(&m,i%9,i/9);
  assert(m.state!=MINE_LOST);
  puts("PASS Minesweeper chording around correctly marked mines");
  struct tiles_game t;
  for(unsigned d=0;d<4;d++) {
    tiles_new(&t,42);memset(t.cells,0,sizeof(t.cells));
    if(d<2) {t.cells[0]=2;t.cells[1]=2;t.cells[2]=2;t.cells[3]=2;}
    else {t.cells[0]=2;t.cells[4]=2;t.cells[8]=2;t.cells[12]=2;}
    uint32_t original[16];memcpy(original,t.cells,sizeof(original));
    uint32_t random=t.random;
    assert(tiles_move(&t,d) && t.score==8 && tiles_largest(&t)==4);
    unsigned fours=0,free=0;
    for(unsigned i=0;i<16;i++) {fours+=t.cells[i]==4;free+=!t.cells[i];}
    assert(fours>=2 && free==13);
    uint32_t moved[16];memcpy(moved,t.cells,sizeof(moved));
    assert(tiles_undo(&t) && !t.can_undo && t.score==0 && t.random==random);
    assert(!memcmp(original,t.cells,sizeof(original)));
    assert(tiles_move(&t,d) && !memcmp(moved,t.cells,sizeof(moved)));
  }
  puts("PASS 2048 all four directions, one merge per turn, score, spawn and exact undo");
  tiles_new(&t,17);
  uint32_t blocked[16]={2,4,2,4,4,2,4,2,2,4,2,4,4,2,4,2};
  memcpy(t.cells,blocked,sizeof(blocked));
  assert(!tiles_available(&t));
  for(unsigned d=0;d<4;d++) assert(!tiles_move(&t,d));
  assert(!memcmp(t.cells,blocked,sizeof(blocked)));
  t.cells[15]=0;assert(tiles_available(&t));
  puts("PASS 2048 game over and unchanged turns never spawn tiles");
  return 0;
}
