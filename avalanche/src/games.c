#include "games.h"
#include <string.h>

static uint32_t next_random(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}
static unsigned bounded(uint32_t *state, unsigned count) {
  uint32_t n, threshold = (uint32_t)(-count) % count;
  do {
    n = next_random(state);
  } while (n < threshold);
  return n % count;
}
void mines_new(struct mines_game *g, unsigned difficulty, uint32_t seed) {
  static const unsigned widths[] = {9, 16, 24}, heights[] = {9, 16, 16},
                        bombs[] = {10, 40, 90};
  if (difficulty > 2)
    difficulty = 2;
  memset(g, 0, sizeof(*g));
  g->width = widths[difficulty];
  g->height = heights[difficulty];
  g->mines = bombs[difficulty];
  g->random = seed ? seed : 0x9e3779b9;
  g->exploded = -1;
}
static void place_mines(struct mines_game *g, unsigned first_x,
                        unsigned first_y) {
  uint16_t pool[384];
  unsigned available = 0;
  for (unsigned y = 0; y < g->height; y++)
    for (unsigned x = 0; x < g->width; x++)
      if ((int)x < (int)first_x - 1 || x > first_x + 1 ||
          (int)y < (int)first_y - 1 || y > first_y + 1)
        pool[available++] = y * g->width + x;
  for (unsigned i = 0; i < g->mines; i++) {
    unsigned pick = bounded(&g->random, available);
    unsigned at = pool[pick];
    pool[pick] = pool[--available];
    g->cells[at] = 9;
  }
  for (unsigned y = 0; y < g->height; y++)
    for (unsigned x = 0; x < g->width; x++) {
      unsigned at = y * g->width + x;
      if (g->cells[at] == 9)
        continue;
      unsigned count = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = (int)x + dx, ny = (int)y + dy;
          if (nx >= 0 && ny >= 0 && nx < (int)g->width && ny < (int)g->height)
            count += g->cells[ny * g->width + nx] == 9;
        }
      g->cells[at] = count;
    }
  g->state = MINE_PLAYING;
}
bool mines_reveal(struct mines_game *g, unsigned x, unsigned y) {
  if (x >= g->width || y >= g->height || g->state >= MINE_WON)
    return false;
  unsigned at = y * g->width + x;
  if (g->marked[at] || g->opened[at])
    return false;
  if (g->state == MINE_READY)
    place_mines(g, x, y);
  if (g->cells[at] == 9) {
    g->opened[at] = 1;
    g->exploded = at;
    g->state = MINE_LOST;
    return true;
  }
  uint16_t queue[384];
  unsigned read = 0, used = 1;
  queue[0] = at;
  g->opened[at] = 1;
  g->revealed++;
  while (read < used) {
    unsigned current = queue[read++];
    if (g->cells[current])
      continue;
    int cx = current % g->width, cy = current / g->width;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        int nx = cx + dx, ny = cy + dy;
        if (nx < 0 || ny < 0 || nx >= (int)g->width || ny >= (int)g->height)
          continue;
        unsigned n = ny * g->width + nx;
        if (g->opened[n] || g->marked[n] || g->cells[n] == 9)
          continue;
        g->opened[n] = 1;
        g->revealed++;
        queue[used++] = n;
      }
  }
  if (g->revealed == g->width * g->height - g->mines) {
    g->state = MINE_WON;
    for (unsigned i = 0; i < g->width * g->height; i++)
      if (g->cells[i] == 9)
        g->marked[i] = 1;
    g->flags = g->mines;
  }
  return true;
}
bool mines_flag(struct mines_game *g, unsigned x, unsigned y) {
  if (x >= g->width || y >= g->height || g->state >= MINE_WON)
    return false;
  unsigned at = y * g->width + x;
  if (g->opened[at])
    return false;
  if (!g->marked[at] && g->flags == g->mines)
    return false;
  g->marked[at] ^= 1;
  if (g->marked[at])
    g->flags++;
  else
    g->flags--;
  return true;
}
bool mines_chord(struct mines_game *g, unsigned x, unsigned y) {
  if (x >= g->width || y >= g->height || g->state != MINE_PLAYING)
    return false;
  unsigned at = y * g->width + x, marked = 0;
  if (!g->opened[at] || !g->cells[at])
    return false;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      int nx = (int)x + dx, ny = (int)y + dy;
      if (nx >= 0 && ny >= 0 && nx < (int)g->width && ny < (int)g->height)
        marked += g->marked[ny * g->width + nx];
    }
  if (marked != g->cells[at])
    return false;
  bool changed = false;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      changed = mines_reveal(g, x + dx, y + dy) || changed;
  return changed;
}
static void tiles_spawn(struct tiles_game *g) {
  unsigned free[16], count = 0;
  for (unsigned i = 0; i < 16; i++)
    if (!g->cells[i])
      free[count++] = i;
  if (count) {
    unsigned at = free[bounded(&g->random, count)];
    g->cells[at] = bounded(&g->random, 10) ? 2 : 4;
  }
}
void tiles_new(struct tiles_game *g, uint32_t seed) {
  memset(g, 0, sizeof(*g));
  g->random = seed ? seed : 0x85ebca6b;
  tiles_spawn(g);
  tiles_spawn(g);
}
static unsigned tile_index(enum tile_direction d, unsigned line, unsigned col) {
  switch (d) {
  case TILE_LEFT:
    return line * 4 + col;
  case TILE_RIGHT:
    return line * 4 + 3 - col;
  case TILE_UP:
    return col * 4 + line;
  case TILE_DOWN:
    return (3 - col) * 4 + line;
  }
  return 0;
}
bool tiles_move(struct tiles_game *g, enum tile_direction direction) {
  if (direction > TILE_DOWN)
    return false;
  uint32_t before[16], score = g->score;
  memcpy(before, g->cells, sizeof(before));
  bool changed = false;
  for (unsigned row = 0; row < 4; row++) {
    uint32_t input[4], output[4] = {0};
    unsigned count = 0, used = 0;
    for (unsigned col = 0; col < 4; col++) {
      uint32_t v = g->cells[tile_index(direction, row, col)];
      if (v)
        input[count++] = v;
    }
    for (unsigned col = 0; col < count; col++) {
      uint32_t v = input[col];
      if (col + 1 < count && input[col + 1] == v && v <= UINT32_MAX / 2) {
        v *= 2;
        g->score += v;
        col++;
      }
      output[used++] = v;
    }
    for (unsigned col = 0; col < 4; col++) {
      unsigned at = tile_index(direction, row, col);
      changed |= g->cells[at] != output[col];
      g->cells[at] = output[col];
    }
  }
  if (!changed)
    return false;
  memcpy(g->undo_cells, before, sizeof(before));
  g->undo_score = score;
  g->undo_random = g->random;
  g->can_undo = true;
  tiles_spawn(g);
  return true;
}
bool tiles_undo(struct tiles_game *g) {
  if (!g->can_undo)
    return false;
  memcpy(g->cells, g->undo_cells, sizeof(g->cells));
  g->score = g->undo_score;
  g->random = g->undo_random;
  g->can_undo = false;
  return true;
}
bool tiles_available(const struct tiles_game *g) {
  for (unsigned y = 0; y < 4; y++)
    for (unsigned x = 0; x < 4; x++) {
      uint32_t v = g->cells[y * 4 + x];
      if (!v || (x < 3 && v == g->cells[y * 4 + x + 1]) ||
          (y < 3 && v == g->cells[(y + 1) * 4 + x]))
        return true;
    }
  return false;
}
uint32_t tiles_largest(const struct tiles_game *g) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 16; i++)
    if (g->cells[i] > value)
      value = g->cells[i];
  return value;
}
