#ifndef AVALANCHE_GAMES_H
#define AVALANCHE_GAMES_H
#include <stdbool.h>
#include <stdint.h>
enum mine_state { MINE_READY, MINE_PLAYING, MINE_WON, MINE_LOST };
struct mines_game {
  uint32_t random;
  unsigned width, height, mines, revealed, flags;
  enum mine_state state;
  int exploded;
  uint8_t cells[384], opened[384], marked[384];
};
void mines_new(struct mines_game *, unsigned difficulty, uint32_t seed);
bool mines_reveal(struct mines_game *, unsigned x, unsigned y);
bool mines_flag(struct mines_game *, unsigned x, unsigned y);
bool mines_chord(struct mines_game *, unsigned x, unsigned y);
struct tiles_game {
  uint32_t random, score, cells[16];
  uint32_t undo_random, undo_score, undo_cells[16];
  bool can_undo;
};
enum tile_direction { TILE_LEFT, TILE_RIGHT, TILE_UP, TILE_DOWN };
void tiles_new(struct tiles_game *, uint32_t seed);
bool tiles_move(struct tiles_game *, enum tile_direction);
bool tiles_undo(struct tiles_game *);
bool tiles_available(const struct tiles_game *);
uint32_t tiles_largest(const struct tiles_game *);
#endif
