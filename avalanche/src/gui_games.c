#include "gui_games.h"
#include "games.h"
#include "gui_canvas.h"
#include "system.h"
static struct mines_game mines;
static struct tiles_game tiles;
static struct {
  char magic[8];
  uint32_t tiles_best, mine_times[3], checksum;
} scores;
static bool initialized, dirty, flags_mode;
static unsigned selected, difficulty;
static uint32_t mine_start, mine_elapsed, last_save;
static uint32_t seed(void) {
  uint32_t value = clock_ticks();
  if (!secure_random(&value, sizeof(value)))
    value ^= 0x6d2b79f5;
  return value ? value : 1;
}
void games_ui_save(void) {
  if (!dirty)
    return;
  memcpy(scores.magic, "MGGAMES1", 8);
  scores.checksum = crc32(&scores, offsetof(__typeof__(scores), checksum));
  if (fs_write("/games.scores", &scores, sizeof(scores)) == (int)sizeof(scores))
    dirty = false;
  last_save = clock_ticks();
}
static void initialize(void) {
  if (initialized)
    return;
  initialized = true;
  if (fs_read("/games.scores", &scores, sizeof(scores)) != sizeof(scores) ||
      __builtin_memcmp(scores.magic, "MGGAMES1", 8) ||
      crc32(&scores, offsetof(__typeof__(scores), checksum)) != scores.checksum)
    memset(&scores, 0, sizeof(scores));
  mines_new(&mines, 0, seed());
  tiles_new(&tiles, seed());
}
static void number(char out[32], const char *prefix, uint32_t value) {
  unsigned n = strlen(prefix);
  memcpy(out, prefix, n);
  char reverse[12];
  unsigned used = 0;
  do {
    reverse[used++] = '0' + value % 10;
    value /= 10;
  } while (value);
  while (used)
    out[n++] = reverse[--used];
  out[n] = 0;
}
static void score_label(struct nk_context *ui, const char *prefix,
                        uint32_t value) {
  char line[32];
  number(line, prefix, value);
  nk_label(ui, line, NK_TEXT_LEFT);
}
static void new_mines(void) {
  games_ui_save();
  mines_new(&mines, difficulty, seed());
  mine_elapsed = 0;
}
static void new_tiles(void) {
  games_ui_save();
  tiles_new(&tiles, seed());
}
bool games_ui_key(char c) {
  initialize();
  if (c == 'r') {
    if (selected)
      new_tiles();
    else
      new_mines();
    return true;
  }
  if (!selected)
    return false;
  int direction = c == 'a'   ? TILE_LEFT
                  : c == 'd' ? TILE_RIGHT
                  : c == 'w' ? TILE_UP
                  : c == 's' ? TILE_DOWN
                             : -1;
  if (direction >= 0) {
    tiles_move(&tiles, direction);
    return true;
  }
  if (c == 'u') {
    tiles_undo(&tiles);
    return true;
  }
  return false;
}
void games_ui_direction(unsigned direction) {
  initialize();
  if (selected && direction <= TILE_DOWN)
    tiles_move(&tiles, direction);
}
static void centered(struct nk_context *ui, struct nk_command_buffer *canvas,
                     struct nk_rect rect, const char *text,
                     struct nk_color color) {
  static struct nk_user_font font;
  font = *ui->style.font;
  float size = rect.h * 0.42f;
  if (size > 32)
    size = 32;
  if (size < 12)
    size = 12;
  font.height = size;
  float w = font.width(font.userdata, size, text, strlen(text));
  struct nk_rect line = nk_rect(rect.x + (rect.w - w) / 2,
                                rect.y + (rect.h - size) / 2, w + 1, size + 2);
  nk_draw_text(canvas, line, text, strlen(text), &font, nk_rgba(0, 0, 0, 0),
               color);
}
static void mines_board(struct nk_context *ui, struct nk_color text,
                        struct nk_color muted, struct nk_color surface,
                        struct nk_color accent) {
  const char *levels[] = {"Beginner", "Intermediate", "Expert"};
  nk_layout_row_dynamic(ui, 32, 3);
  unsigned next = nk_combo(ui, levels, 3, difficulty, 28, nk_vec2(190, 130));
  if (next != difficulty) {
    difficulty = next;
    new_mines();
  }
  if (nk_button_label(ui, "New game"))
    new_mines();
  if (nk_button_label(ui, flags_mode ? "Flag mode: on" : "Flag mode: off"))
    flags_mode = !flags_mode;
  nk_layout_row_dynamic(ui, 26, 3);
  score_label(ui, "Mines: ", mines.mines - mines.flags);
  score_label(ui, "Time: ", mine_elapsed / 100);
  if (scores.mine_times[difficulty])
    score_label(ui, "Best: ", scores.mine_times[difficulty] / 100);
  else
    nk_label_colored(ui, "Best: --", NK_TEXT_LEFT, muted);
  nk_layout_row_dynamic(ui, 28, 1);
  const char *status =
      mines.state == MINE_READY ? "Your first square is always safe."
      : mines.state == MINE_WON
          ? "Field cleared! Start a new game to play again."
      : mines.state == MINE_LOST ? "A mine! Start a new game to try again."
                                 : "Reveal the field. Right-click to flag; "
                                   "click a number to clear its neighbors.";
  nk_label_wrap(ui, status);
  struct nk_rect region = nk_window_get_content_region(ui);
  float available = region.h - 154;
  if (available < 140)
    available = 140;
  float size = (region.w - 16) / mines.width;
  if (size > available / mines.height)
    size = available / mines.height;
  if (size > 40)
    size = 40;
  nk_layout_row_dynamic(ui, size * mines.height + 8, 1);
  struct nk_rect area;
  if (nk_widget(&area, ui)) {
    struct nk_command_buffer *canvas = nk_window_get_canvas(ui);
    float left = area.x + (area.w - size * mines.width) / 2;
    static const uint32_t colors[] = {0,        0x79a6ff, 0x56d4a0,
                                      0xff7d97, 0xb6a0ff, 0xffb86c,
                                      0x66d9ef, 0xf4c2ff, 0xe6e8f5};
    for (unsigned y = 0; y < mines.height; y++)
      for (unsigned x = 0; x < mines.width; x++) {
        unsigned at = y * mines.width + x;
        struct nk_rect r = nk_rect(left + x * size + 1, area.y + y * size + 1,
                                   size - 2, size - 2);
        bool opened = mines.opened[at], bomb = mines.cells[at] == 9;
        struct nk_color fill =
            opened ? nk_rgba(surface.r, surface.g, surface.b, 100) : surface;
        if ((int)at == mines.exploded)
          fill = nk_rgb(180, 43, 74);
        bool hover = nk_input_is_mouse_hovering_rect(&ui->input, r) &&
                     nk_window_is_active(ui, "Games");
        if (hover && !opened)
          fill = accent;
        nk_fill_rect(canvas, r, 3, fill);
        if (!opened)
          gui_canvas_gloss(canvas, r, 24);
        if (mines.marked[at]) {
          float cx = r.x + r.w * .4f, cy = r.y + r.h * .25f;
          nk_stroke_line(canvas, cx, cy, cx, r.y + r.h * .8f, 2, text);
          nk_fill_triangle(canvas, cx, cy, cx + r.w * .4f, cy + r.h * .15f, cx,
                           cy + r.h * .3f, accent);
        } else if (bomb && (opened || mines.state == MINE_LOST)) {
          nk_fill_circle(canvas,
                         nk_rect(r.x + r.w * .28f, r.y + r.h * .28f, r.w * .44f,
                                 r.h * .44f),
                         text);
        } else if (opened && mines.cells[at]) {
          char n[2] = {'0' + mines.cells[at], 0};
          uint32_t c = colors[mines.cells[at]];
          centered(ui, canvas, r, n, nk_rgb(c >> 16, c >> 8, c));
        }
        if (hover && nk_input_is_mouse_pressed(&ui->input, NK_BUTTON_RIGHT))
          mines_flag(&mines, x, y);
        if (hover && nk_input_is_mouse_pressed(&ui->input, NK_BUTTON_LEFT)) {
          enum mine_state old = mines.state;
          if (flags_mode)
            mines_flag(&mines, x, y);
          else if (opened)
            mines_chord(&mines, x, y);
          else
            mines_reveal(&mines, x, y);
          if (old == MINE_READY && mines.state != MINE_READY)
            mine_start = clock_ticks();
          if (old == MINE_PLAYING && mines.state >= MINE_WON)
            mine_elapsed = clock_ticks() - mine_start;
        }
      }
  }
  if (mines.state == MINE_PLAYING)
    mine_elapsed = clock_ticks() - mine_start;
  if (mines.state == MINE_WON) {
    uint32_t elapsed = mine_elapsed ? mine_elapsed : 1;
    if (!scores.mine_times[difficulty] ||
        elapsed < scores.mine_times[difficulty]) {
      scores.mine_times[difficulty] = elapsed;
      dirty = true;
      games_ui_save();
    }
  }
}
static void tiles_board(struct nk_context *ui, struct nk_color text,
                        struct nk_color muted, struct nk_color surface,
                        struct nk_color accent) {
  nk_layout_row_dynamic(ui, 32, 4);
  score_label(ui, "Score: ", tiles.score);
  score_label(ui, "Best: ", scores.tiles_best);
  if (nk_button_label(ui, "New game"))
    new_tiles();
  if (nk_button_label(ui, "Undo"))
    tiles_undo(&tiles);
  if (nk_window_is_active(ui, "Games")) {
    const enum nk_keys keys[] = {NK_KEY_LEFT, NK_KEY_RIGHT, NK_KEY_UP,
                                 NK_KEY_DOWN};
    for (unsigned i = 0; i < 4; i++)
      if (nk_input_is_key_pressed(&ui->input, keys[i]))
        tiles_move(&tiles, i);
  }
  nk_layout_row_dynamic(ui, 28, 1);
  nk_label_colored(
      ui,
      !tiles_available(&tiles) ? "No moves left. Undo or start a new game."
      : tiles_largest(&tiles) >= 2048
          ? "2048! Keep going for a higher score."
          : "Merge matching tiles. Arrow keys or W A S D to move. U to undo.",
      NK_TEXT_LEFT, muted);
  struct nk_rect region = nk_window_get_content_region(ui);
  float size = region.h - 174;
  if (size > region.w - 16)
    size = region.w - 16;
  if (size > 400)
    size = 400;
  if (size < 160)
    size = 160;
  nk_layout_row_dynamic(ui, size + 8, 1);
  struct nk_rect area;
  if (nk_widget(&area, ui)) {
    struct nk_command_buffer *canvas = nk_window_get_canvas(ui);
    float left = area.x + (area.w - size) / 2, cell = size / 4;
    nk_fill_rect(canvas, nk_rect(left, area.y, size, size), 12,
                 nk_rgba(surface.r, surface.g, surface.b, 120));
    for (unsigned y = 0; y < 4; y++)
      for (unsigned x = 0; x < 4; x++) {
        uint32_t value = tiles.cells[y * 4 + x];
        unsigned power = 0;
        for (uint32_t n = value; n > 1; n >>= 1)
          power++;
        struct nk_rect r = nk_rect(left + x * cell + 4, area.y + y * cell + 4,
                                   cell - 8, cell - 8);
        struct nk_color fill = surface;
        if (value) {
          unsigned mix = power * 22;
          if (mix > 235)
            mix = 235;
          fill = nk_rgb((surface.r * (255 - mix) + accent.r * mix) / 255,
                        (surface.g * (255 - mix) + accent.g * mix) / 255,
                        (surface.b * (255 - mix) + accent.b * mix) / 255);
        }
        nk_fill_rect(canvas, r, 9, fill);
        if (value)
          gui_canvas_gloss(canvas, r, 35);
        if (value) {
          char n[32];
          number(n, "", value);
          centered(ui, canvas, r, n, text);
        }
      }
  }
  nk_layout_row_dynamic(ui, 34, 4);
  const char *directions[] = {"Left", "Right", "Up", "Down"};
  for (unsigned i = 0; i < 4; i++)
    if (nk_button_label(ui, directions[i]))
      tiles_move(&tiles, i);
  if (tiles.score > scores.tiles_best) {
    scores.tiles_best = tiles.score;
    dirty = true;
  }
}
void games_ui(struct nk_context *ui, struct nk_color text,
              struct nk_color muted, struct nk_color surface,
              struct nk_color accent) {
  initialize();
  nk_layout_row_dynamic(ui, 36, 2);
  if (nk_button_label(ui, "Minesweeper")) {
    selected = 0;
    games_ui_save();
  }
  if (nk_button_label(ui, "2048")) {
    selected = 1;
    games_ui_save();
  }
  if (selected)
    tiles_board(ui, text, muted, surface, accent);
  else
    mines_board(ui, text, muted, surface, accent);
  if (dirty && (uint32_t)(clock_ticks() - last_save) >= 500)
    games_ui_save();
}
