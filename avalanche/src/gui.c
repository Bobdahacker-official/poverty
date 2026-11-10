#include "client.h"
#include "calculator.h"
#include "filetypes.h"
#include "nvme_health.h"
#include "package.h"
#include "font.h"
#include "gui_canvas.h"
#include "gui_games.h"
#include "gui_style.h"
#include "http.h"
#include "image.h"
#include "process.h"
#include "system.h"
#include "tls.h"
#include "usb.h"
#include "web.h"

enum {
  TERMINAL,
  FILES,
  EDITOR,
  TASKS,
  NETWORK,
  PACKAGES,
  SETTINGS,
  BROWSER,
  IMAGES,
  MUSIC,
  GAMES,
  CALCULATOR,
  APP_COUNT
};
static struct nk_context ui;
static uint8_t ui_memory[4 * 1024 * 1024];
static struct nk_user_font body_font, heading_font;
static unsigned width, height, display_width, display_height;
static float native_scale = 1;
static const char *app_names[APP_COUNT] = {
    "Terminal", "Files",    "Text editor", "System Monitor",
    "Network",  "Packages", "Settings",    "Browser",
    "Images",   "Music",    "Games", "Calculator"};
static const char *app_summaries[APP_COUNT] = {
    "Run programs and shell commands",
    "Browse your disks and documents",
    "Write, edit and save text",
    "Inspect and manage processes",
    "Configure IPv4 and watch traffic",
    "Install and update applications",
    "Make Magenta your own",
    "Read web pages and local documents",
    "View PNG, JPEG and BMP pictures",
    "Listen to music and WAV recordings",
    "Play Minesweeper and 2048",
    "Calculate expressions, recall answers and use memory"};
static char music_path[256];
static bool music_focus, music_play_requested;
static int music_result;
static struct image_document picture;
static char picture_path[256];
static bool picture_focus;
static bool picture_load_requested;
static struct app {
  bool visible, maximized;
  struct nk_rect bounds, restore;
} apps[APP_COUNT];
struct preferences {
  uint32_t magic, version, theme, accent, wallpaper, alpha, blur, pointer_speed,
      checksum;
};
static struct preferences preferences;
static bool preferences_dirty, theme_dirty, launcher, power_dialog, running,
    save_requested, escape_requested;
static bool preferences_saved, close_editor_after_discard, exit_after_discard;
static void shell_start(void);
static int active_app = SETTINGS, focus_request = -1, editor_focus,
           terminal_focus, settings_page;
static int held_control_app = -1, held_control = -1;
static unsigned launcher_category;
static const uint32_t accents[] = {0xe6007e, 0x7287fd, 0x179299, 0xfe640b,
                                   0x8839ef};
static const struct palette {
  const char *name;
  uint32_t base, mantle, surface, text, muted;
} palettes[] = {{"Latte", 0xeff1f5, 0xe6e9ef, 0xccd0da, 0x4c4f69, 0x6c6f85},
                {"Frappe", 0x303446, 0x292c3c, 0x414559, 0xc6d0f5, 0xa5adce},
                {"Macchiato", 0x24273a, 0x1e2030, 0x363a4f, 0xcad3f5, 0xa5adcb},
                {"Mocha", 0x1e1e2e, 0x181825, 0x313244, 0xcdd6f4, 0xa6adc8}};
static char notification[256], clock_text[48], clipboard[65536];
static bool clipboard_dirty, clipboard_saved;
static uint32_t clipboard_last_attempt;
static struct {
  uint32_t magic, version, handlers[FILE_TYPE_COUNT], checksum;
} file_defaults;
static bool file_defaults_saved;
static void browser_new_tab(const char *);
static bool resume_desktop, resume_editor;
static void desktop_session_save(void);
static unsigned default_handler(enum file_type type) {
  if (type == FILE_TEXT || type == FILE_HTML)
    return file_defaults.handlers[type] == BROWSER ? BROWSER : EDITOR;
  if (type == FILE_PNG || type == FILE_JPEG || type == FILE_BMP) return IMAGES;
  if (type == FILE_WAV) return MUSIC;
  return EDITOR;
}
static void defaults_reset(void) {
  file_defaults_saved = false;
  memset(&file_defaults, 0, sizeof(file_defaults));
  file_defaults.magic = 0x4d464150;
  file_defaults.version = 1;
  file_defaults.handlers[FILE_TEXT] = EDITOR;
  file_defaults.handlers[FILE_HTML] = BROWSER;
}
static void defaults_load(void) {
  defaults_reset();
  __typeof__(file_defaults) saved;
  if (fs_read("magenta-filetypes", &saved, sizeof(saved)) == sizeof(saved) &&
      saved.magic == file_defaults.magic && saved.version == 1 &&
      saved.checksum == crc32(&saved, offsetof(__typeof__(saved), checksum)) &&
      (saved.handlers[FILE_TEXT] == EDITOR || saved.handlers[FILE_TEXT] == BROWSER) &&
      (saved.handlers[FILE_HTML] == EDITOR || saved.handlers[FILE_HTML] == BROWSER)) {
    file_defaults = saved;
    file_defaults_saved = true;
  }
}
static void clipboard_save(void) {
  if (!clipboard_dirty) return;
  clipboard_last_attempt = clock_ticks();
  int size = strlen(clipboard);
  int result = fs_write("magenta-clipboard", clipboard, size);
  if (result >= 0) {
    clipboard_dirty = false;
    clipboard_saved = true;
  }
}
static void clipboard_load(void) {
  int size = fs_read("magenta-clipboard", clipboard, sizeof(clipboard) - 1);
  clipboard_saved = size >= 0;
  if (size < 0) size = 0;
  clipboard[size] = 0;
  clipboard_dirty = false;
}
static uint32_t notification_until, last_status, last_save;
static struct av_status system_status;
static struct network_info network_status;
static unsigned task_count;
static char terminal_output[16384], terminal_line[256];
static unsigned terminal_used;
static int terminal_pid;
static char history[16][256];
static unsigned history_count, history_cursor;
static char editor[FS_MAX_FILE + 1], editor_path[256] = "/notes";
static char editor_copy_path[256];
static bool editor_save_as;
static char editor_find[128], editor_last_find[128];
static int editor_line = 1, editor_match_end;
static uint32_t editor_draft_tick, editor_draft_crc;
static bool editor_draft_available, editor_draft_written;
#define EDITOR_DRAFT_PATH "/.magenta-editor-recovery"
static struct {
  char path[256];
  char text[FS_MAX_FILE + 1];
} editor_draft;
static bool editor_dirty, editor_discard;
static char browser_path[256] = "/", browser_address[256] = "/",
            file_filter[64], new_name[256];
static struct file_item {
  char name[256], path[256];
  uint64_t size;
  bool directory;
} file_items[64];
static unsigned file_count, file_page;
static int selected_file = -1;
static bool file_more, creating_file, deleting_file;
static bool emptying_trash;
static bool showing_drives;
static bool showing_downloads, download_attention;
static char local_manifest[256];
static char restore_name[256], restore_entry[256];
static struct task_info task_items[256];
static unsigned task_slots;
static int focused_client = -1;
static int client_focus_request = -1;
static int client_seen[GUI_CLIENTS];
static int client_return_app[GUI_CLIENTS];
static struct nk_rect client_bounds[GUI_CLIENTS];
static struct nk_rect client_restore[GUI_CLIENTS];
static bool client_maximized[GUI_CLIENTS];
static void client_name(unsigned i, char name[96]);
static bool pointer_targets_window(const char *name);
static int client_mouse_x = -1, client_mouse_y = -1, client_mouse_buttons = -1;
static int selected_pid, task_priority = 16;
static bool ending_task;
static int ending_pid;
static char deleting_path[256];
static char ping_target[64] = "10.0.2.2", network_result[2048];
static char ipv4_fields[4][32];
static bool static_address;
static struct package_item {
  char name[32], version[32];
  bool installed;
} package_items[256];
static unsigned package_count;
static int selected_package;
static char package_filter[64], package_details[2048], package_result[4096];
static nk_bool installed_only;
static bool removing_package;
static char download_host[128], download_path[128];
static char upload_host[128] = "http://10.0.2.2:6082";
static char launcher_search[64];
struct desktop_registry {
  uint32_t magic, version, shortcuts, top_bar, dock_bar, desktop_icons;
  uint32_t icons_x, icons_y, dock_bottom, checksum;
};
static struct desktop_registry desktop = {
    0x4d475247, 1, (1u << FILES) | (1u << TERMINAL), 1, 1, 1, 6, 58, 16, 0};
static bool desktop_dirty, context_open;
static int context_app = -1;
static struct nk_rect context_bounds;
static char registry_key[48], registry_value[32];
static char registry_status[96] = "Select a key to edit its current value";
static bool creating_folder, renaming_file;
static char rename_name[256];
static char file_clipboard[256], file_clipboard_name[256];
static bool file_cut;
static nk_bool search_subfolders;
static unsigned file_matches, file_scanned;
static bool search_limited;
static char file_history[32][256];
static unsigned file_history_count, file_history_at;
static int files_address_focus, files_search_focus;
static bool files_editing;
static char task_filter[64];
static unsigned task_cpu[256], task_previous_cpu[256], task_cpu_pid[256];
static unsigned cpu_samples[60], memory_samples[60], sample_count, sample_at;
static unsigned userspace_cpu;
static uint32_t task_sample_time;
static uint64_t previous_user_ticks;
static int task_page;
static nk_bool tasks_paused;
static bool task_sample_valid;
static int file_last_click = -1;
static uint32_t file_last_click_time;

static void copy(char *out, const char *in, unsigned capacity) {
  unsigned n = 0;
  if (!capacity)
    return;
  while (in[n] && n + 1 < capacity) {
    out[n] = in[n];
    n++;
  }
  out[n] = 0;
}
static void append(char *out, const char *in, unsigned capacity) {
  unsigned used = strlen(out);
  if (used < capacity)
    copy(out + used, in, capacity - used);
}
static void number(char out[32], uint64_t value) {
  char reverse[32];
  unsigned n = 0;
  do {
    reverse[n++] = '0' + value % 10;
    value /= 10;
  } while (value);
  for (unsigned i = 0; i < n; i++)
    out[i] = reverse[n - 1 - i];
  out[n] = 0;
}
static bool contains(const char *text, const char *needle) {
  if (!*needle)
    return true;
  for (; *text; text++) {
    unsigned n = 0;
    while (needle[n] && text[n]) {
      char a = text[n], b = needle[n];
      if (a >= 'A' && a <= 'Z')
        a += 32;
      if (b >= 'A' && b <= 'Z')
        b += 32;
      if (a != b)
        break;
      n++;
    }
    if (!needle[n])
      return true;
  }
  return false;
}
static bool starts_with(const char *text, const char *prefix) {
  while (*prefix)
    if (*text++ != *prefix++)
      return false;
  return true;
}
static struct nk_color rgb(uint32_t value) {
  return nk_rgb((value >> 16) & 255, (value >> 8) & 255, value & 255);
}
static struct nk_color alpha_color(uint32_t value, unsigned alpha) {
  struct nk_color c = rgb(value);
  c.a = alpha;
  return c;
}
static uint32_t accent(void) { return accents[preferences.accent]; }
static const struct palette *palette(void) {
  return &palettes[preferences.theme];
}
static void capture(const char *command, char *out, unsigned capacity) {
  char line[512];
  copy(line, command, sizeof(line));
  kernel_capture(line, out, capacity);
}
#define WORKSPACE_MAX 16
static struct workspace_settings {
  uint32_t magic, version, count, current, homes[APP_COUNT];
  char names[WORKSPACE_MAX][32];
  uint32_t checksum;
} workspaces;
static unsigned client_workspace[GUI_CLIENTS];
static bool workspace_hidden[APP_COUNT], client_workspace_hidden[GUI_CLIENTS];
static bool app_close_requested[APP_COUNT];
static int workspace_focus[WORKSPACE_MAX], workspace_pending = -1;
static bool workspace_dirty;
static uint32_t workspace_last_save;
static void workspace_view(void);
static void workspace_command(char *);
static void workspace_request(unsigned target) {
  if (target < workspaces.count) workspace_pending = target;
}
static void workspace_save(void) {
  if (workspace_pending >= 0) return;
  workspace_last_save = clock_ticks();
  workspaces.checksum = crc32(&workspaces, offsetof(struct workspace_settings, checksum));
  if (fs_write("magenta-workspaces", &workspaces, sizeof(workspaces)) >= 0)
    workspace_dirty = false;
}
static void workspace_load(void) {
  memset(&workspaces, 0, sizeof(workspaces));
  workspaces.magic = 0x4d475753; workspaces.version = 1; workspaces.count = 4;
  for (unsigned i = 0; i < WORKSPACE_MAX; i++) {
    char n[32]; number(n, i + 1);
    copy(workspaces.names[i], "Workspace ", 32); append(workspaces.names[i], n, 32);
    workspace_focus[i] = -1;
  }
  struct workspace_settings saved;
  if (fs_read("magenta-workspaces", &saved, sizeof(saved)) == sizeof(saved) &&
      saved.magic == workspaces.magic && saved.version == 1 &&
      saved.count && saved.count <= WORKSPACE_MAX && saved.current < saved.count &&
      saved.checksum == crc32(&saved, offsetof(struct workspace_settings, checksum))) {
    bool valid = true;
    for (unsigned i = 0; i < APP_COUNT; i++) if (saved.homes[i] >= saved.count) valid = false;
    for (unsigned i = 0; i < WORKSPACE_MAX; i++)
      if (!saved.names[i][0] || !__builtin_memchr(saved.names[i], 0, 32)) valid = false;
    if (valid) workspaces = saved;
  }
  memset(workspace_hidden, 0, sizeof(workspace_hidden));
  memset(app_close_requested, 0, sizeof(app_close_requested));
  memset(client_workspace_hidden, 0, sizeof(client_workspace_hidden));
  memset(client_workspace, 0, sizeof(client_workspace));
  workspace_pending = -1; workspace_dirty = false;
}
static void workspace_count(unsigned count) {
  if (!count || count > WORKSPACE_MAX) return;
  for (unsigned i = 0; i < APP_COUNT; i++)
    if (workspaces.homes[i] >= count) workspaces.homes[i] = count - 1;
  for (unsigned i = 0; i < GUI_CLIENTS; i++)
    if (client_workspace[i] >= count) client_workspace[i] = count - 1;
  workspaces.count = count;
  if (workspaces.current >= count) workspace_pending = count - 1;
  else if (workspace_pending >= (int)count) workspace_pending = -1;
  workspace_dirty = true; gui_canvas_invalidate();
}
static void workspace_move(unsigned window, unsigned target) {
  if (target >= workspaces.count) return;
  if (window < APP_COUNT) workspaces.homes[window] = target;
  else if (window - APP_COUNT < GUI_CLIENTS) client_workspace[window - APP_COUNT] = target;
  else return;
  if ((active_app >= 0 && (unsigned)active_app == window) ||
      (focused_client >= 0 && window == APP_COUNT + (unsigned)focused_client)) {
    if (focused_client >= 0 && client_mouse_buttons > 0)
      client_event(focused_client, AV_GUI_MOUSE, client_mouse_x, client_mouse_y, 0);
    client_mouse_buttons = -1;
    active_app = -1; focused_client = -1;
    nk_edit_unfocus(&ui);
  }
  workspace_dirty = true; gui_canvas_invalidate();
}
static bool workspace_window(const char *name, struct nk_rect bounds,
                             unsigned home, bool *hidden) {
  if (home != workspaces.current) {
    struct nk_window *window = nk_window_find(&ui, name);
    if (window) {
      if (!(window->flags & (NK_WINDOW_CLOSED | NK_WINDOW_HIDDEN))) {
        nk_window_show(&ui, name, NK_HIDDEN); *hidden = true;
      }
      nk_begin(&ui, name, bounds, NK_WINDOW_HIDDEN); nk_end(&ui);
      for (struct nk_table *table = window->tables; table; table = table->next)
        table->seq = ui.seq;
    }
    return false;
  }
  if (*hidden) { nk_window_show(&ui, name, NK_SHOWN); *hidden = false; }
  return true;
}
static void workspace_apply(void) {
  if (workspace_pending >= 0 && (unsigned)workspace_pending < workspaces.count &&
      (unsigned)workspace_pending != workspaces.current) {
    if (workspaces.current < WORKSPACE_MAX) {
      if (focused_client >= 0 && client_workspace[focused_client] == workspaces.current)
        workspace_focus[workspaces.current] = APP_COUNT + focused_client;
      else if (active_app >= 0 && workspaces.homes[active_app] == workspaces.current)
        workspace_focus[workspaces.current] = active_app;
    }
    if (focused_client >= 0 && client_mouse_buttons > 0)
      client_event(focused_client, AV_GUI_MOUSE, client_mouse_x, client_mouse_y, 0);
    client_mouse_buttons = -1;
    workspaces.current = workspace_pending;
    active_app = focused_client = -1; focus_request = client_focus_request = -1;
    held_control_app = held_control = -1;
    context_open = launcher = false;
    nk_edit_unfocus(&ui);
    int focus = workspace_focus[workspaces.current];
    if (focus >= 0 && focus < APP_COUNT && apps[focus].visible && workspaces.homes[focus] == workspaces.current)
      focus_request = focus;
    else if (focus >= APP_COUNT && focus < APP_COUNT + GUI_CLIENTS &&
             gui_clients[focus - APP_COUNT].pixels && client_workspace[focus - APP_COUNT] == workspaces.current)
      client_focus_request = focus - APP_COUNT;
    workspace_dirty = true; gui_canvas_invalidate();
  }
  workspace_pending = -1;
}
static void notify(const char *message) {
  copy(notification, message, sizeof(notification));
  notification_until = clock_ticks() + 400;
  console_write("[Magenta] ");
  console_write(notification);
  console_putc('\n');
}
static void terminal_character(char c) {
  if (c == '\r')
    return;
  if (c == '\b') {
    if (terminal_used)
      terminal_output[--terminal_used] = 0;
    return;
  }
  if ((unsigned char)c < 32 && c != '\n' && c != '\t')
    return;
  if (terminal_used + 1 >= sizeof(terminal_output)) {
    unsigned remove = 1024;
    while (remove < terminal_used && terminal_output[remove] != '\n')
      remove++;
    if (remove < terminal_used)
      remove++;
    memmove(terminal_output, terminal_output + remove, terminal_used - remove);
    terminal_used -= remove;
  }
  terminal_output[terminal_used++] = c;
  terminal_output[terminal_used] = 0;
}
static void terminal_append(const char *text) {
  while (*text)
    terminal_character(*text++);
}
static void desktop_save(void) {
  desktop.checksum =
      crc32(&desktop, offsetof(struct desktop_registry, checksum));
  if (fs_write("magenta-registry", &desktop, sizeof(desktop)) >= 0)
    desktop_dirty = false;
}
static void desktop_load(void) {
  struct desktop_registry saved;
  if (fs_read("magenta-registry", &saved, sizeof(saved)) ==
          (int)sizeof(saved) &&
      saved.magic == desktop.magic && saved.version == 1 &&
      saved.top_bar <= 1 && saved.dock_bar <= 1 && saved.desktop_icons <= 1 &&
      saved.checksum ==
          crc32(&saved, offsetof(struct desktop_registry, checksum)))
    desktop = saved;
}
static uint32_t *registry_slot(const char *key) {
  if (str_eq(key, "workspace.count")) return &workspaces.count;
  if (str_eq(key, "workspace.current")) return &workspaces.current;
  if (str_eq(key, "desktop.shortcuts"))
    return &desktop.shortcuts;
  if (str_eq(key, "desktop.top_bar"))
    return &desktop.top_bar;
  if (str_eq(key, "desktop.dock"))
    return &desktop.dock_bar;
  if (str_eq(key, "desktop.icons"))
    return &desktop.desktop_icons;
  if (str_eq(key, "desktop.icons_x"))
    return &desktop.icons_x;
  if (str_eq(key, "desktop.icons_y"))
    return &desktop.icons_y;
  if (str_eq(key, "desktop.dock_bottom"))
    return &desktop.dock_bottom;
  return NULL;
}
static bool registry_set(const char *key, const char *value) {
  uint32_t *slot = registry_slot(key), number = 0;
  if (!slot || !*value)
    return false;
  for (const char *p = value; *p; p++) {
    if (*p < '0' || *p > '9' || number > 100000)
      return false;
    number = number * 10 + *p - '0';
  }
  if (slot == &workspaces.count) {
    if (!number || number > WORKSPACE_MAX) return false;
    workspace_count(number); return true;
  }
  if (slot == &workspaces.current) {
    if (number >= workspaces.count) return false;
    workspace_request(number); return true;
  }
  if ((slot == &desktop.top_bar || slot == &desktop.dock_bar ||
       slot == &desktop.desktop_icons) &&
      number > 1)
    return false;
  if (slot == &desktop.icons_x && number >= width - 100)
    return false;
  if (slot == &desktop.icons_y && number >= height - 120)
    return false;
  if (slot == &desktop.dock_bottom && number > height - 80)
    return false;
  uint32_t old = *slot;
  *slot = number;
  desktop_dirty = true;
  desktop_save();
  if (desktop_dirty) {
    *slot = old;
    return false;
  }
  return true;
}
static void preference_save(bool announce) {
  last_save = clock_ticks();
  preferences.checksum =
      crc32(&preferences, offsetof(struct preferences, checksum));
  int r = fs_write("gui-style", &preferences, sizeof(preferences));
  if (r >= 0) {
    preferences_dirty = false;
    preferences_saved = true;
    last_save = clock_ticks();
    if (announce)
      notify("Appearance saved");
  } else if (announce)
    notify("Could not save appearance to this disk");
}
static void preference_load(void) {
  preferences_saved = false;
  preferences = (struct preferences){0x4d475549, 4, 3, 0, 2, 248, 1, 100, 0};
  struct preferences saved;
  if (fs_read("gui-style", &saved, sizeof(saved)) == (int)sizeof(saved) &&
      saved.magic == preferences.magic &&
      (saved.version >= 1 && saved.version <= 4) && saved.theme < 4 &&
      saved.accent < 5 && saved.wallpaper < 3 && saved.alpha >= 195 &&
      saved.alpha <= 255 && saved.blur <= 1 && saved.pointer_speed >= 25 &&
      saved.pointer_speed <= 250 &&
      saved.checksum == crc32(&saved, offsetof(struct preferences, checksum))) {
    preferences = saved;
    if (saved.version < 4) {
      preferences.version = 4;
      preferences.theme = 3;
      preferences.wallpaper = 2;
    }
    preferences_saved = true;
  }
}
static void style(void) {
  set_style(&ui, THEME_CATPPUCCIN_LATTE + preferences.theme);
  const struct palette *p = palette();
  struct nk_color panel = alpha_color(p->base, preferences.alpha),
                  text = rgb(p->text), muted = rgb(p->muted),
                  color = rgb(accent());
  ui.style.window.fixed_background = nk_style_item_color(panel);
  ui.style.window.background = panel;
  ui.style.window.rounding = 12;
  ui.style.window.border = 1;
  ui.style.window.border_color = alpha_color(p->surface, 210);
  ui.style.window.padding = nk_vec2(16, 14);
  ui.style.window.spacing = nk_vec2(8, 7);
  ui.style.window.group_padding = nk_vec2(10, 10);
  ui.style.window.header.normal = ui.style.window.header.hover =
      ui.style.window.header.active =
          nk_style_item_color(alpha_color(p->mantle, 250));
  ui.style.window.header.padding = nk_vec2(14, 12);
  ui.style.window.scrollbar_size = nk_vec2(6, 6);
  ui.style.scrollv.rounding = ui.style.scrollh.rounding = 3;
  ui.style.window.header.label_padding = nk_vec2(6, 2);
  ui.style.window.header.label_normal = ui.style.window.header.label_hover =
      ui.style.window.header.label_active = text;
  ui.style.window.header.close_button.rounding = 8;
  ui.style.window.header.minimize_button.rounding = 8;
  ui.style.button.rounding = 8;
  ui.style.button.padding = nk_vec2(8, 6);
  ui.style.button.border = 0;
  ui.style.button.normal = nk_style_item_color(alpha_color(p->mantle, 245));
  ui.style.button.hover = nk_style_item_color(rgb(p->surface));
  ui.style.button.active = nk_style_item_color(color);
  ui.style.button.text_normal = ui.style.button.text_hover = text;
  ui.style.button.text_active = nk_rgb(255, 255, 255);
  ui.style.button.border_color = alpha_color(p->surface, 170);
  ui.style.selectable.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  ui.style.selectable.hover = nk_style_item_color(alpha_color(p->surface, 150));
  ui.style.selectable.normal_active = ui.style.selectable.hover_active =
      ui.style.selectable.pressed_active =
          nk_style_item_color(alpha_color(accent(), 32));
  ui.style.selectable.text_normal = ui.style.selectable.text_hover = text;
  ui.style.selectable.text_normal_active =
      ui.style.selectable.text_hover_active = color;
  ui.style.selectable.rounding = 8;
  ui.style.selectable.padding = nk_vec2(8, 6);
  ui.style.contextual_button.rounding = 6;
  ui.style.contextual_button.padding = nk_vec2(10, 6);
  ui.style.contextual_button.normal = nk_style_item_color(rgb(p->mantle));
  ui.style.contextual_button.hover = nk_style_item_color(rgb(p->surface));
  ui.style.contextual_button.active = nk_style_item_color(color);
  ui.style.contextual_button.text_normal =
      ui.style.contextual_button.text_hover = text;
  ui.style.contextual_button.text_active = nk_rgb(255, 255, 255);
  ui.style.edit.rounding = 8;
  ui.style.edit.border = 1;
  ui.style.edit.border_color = alpha_color(p->surface, 200);
  ui.style.edit.normal = ui.style.edit.hover = ui.style.edit.active =
      nk_style_item_color(alpha_color(p->mantle, 235));
  ui.style.edit.text_normal = ui.style.edit.text_hover =
      ui.style.edit.text_active = text;
  ui.style.edit.cursor_normal = ui.style.edit.cursor_hover = color;
  ui.style.edit.selected_normal = ui.style.edit.selected_hover = color;
  ui.style.edit.selected_text_normal = ui.style.edit.selected_text_hover =
      nk_rgb(255, 255, 255);
  ui.style.edit.padding = nk_vec2(10, 8);
  ui.style.edit.row_padding = 5;
  ui.style.text.color = text;
  ui.style.text.padding = nk_vec2(0, 2);
  ui.style.checkbox.cursor_normal = ui.style.checkbox.cursor_hover =
      nk_style_item_color(color);
  ui.style.slider.cursor_normal = ui.style.slider.cursor_hover =
      ui.style.slider.cursor_active = nk_style_item_color(color);
  ui.style.slider.bar_filled = color;
  ui.style.progress.cursor_normal = ui.style.progress.cursor_hover =
      ui.style.progress.cursor_active = nk_style_item_color(color);
  ui.style.window.scaler = nk_style_item_color(muted);
  gui_canvas_wallpaper(accent(), preferences.wallpaper);
  mouse_configure(display_width, display_height, preferences.pointer_speed);
  theme_dirty = false;
}
static void caption(const char *label) {
  nk_label_colored(&ui, label, NK_TEXT_LEFT, rgb(palette()->muted));
}
static void heading(const char *label) {
  nk_layout_row_dynamic(&ui, 34, 1);
  nk_style_push_font(&ui, &heading_font);
  nk_label(&ui, label, NK_TEXT_LEFT);
  nk_style_pop_font(&ui);
}
static void label_value(const char *name, uint64_t value, const char *unit) {
  char buffer[128], digits[32];
  number(digits, value);
  copy(buffer, name, sizeof(buffer));
  append(buffer, digits, sizeof(buffer));
  append(buffer, unit, sizeof(buffer));
  nk_label(&ui, buffer, NK_TEXT_LEFT);
}
static bool accent_button(const char *label) {
  struct nk_color hover = rgb(accent());
  hover.r += (255 - hover.r) / 6;
  hover.g += (255 - hover.g) / 6;
  hover.b += (255 - hover.b) / 6;
  nk_style_push_style_item(&ui, &ui.style.button.normal,
                           nk_style_item_color(rgb(accent())));
  nk_style_push_style_item(&ui, &ui.style.button.hover,
                           nk_style_item_color(hover));
  nk_style_push_color(&ui, &ui.style.button.text_normal, nk_rgb(255, 255, 255));
  nk_style_push_color(&ui, &ui.style.button.text_hover, nk_rgb(255, 255, 255));
  nk_bool clicked = nk_button_label(&ui, label);
  nk_style_pop_color(&ui);
  nk_style_pop_color(&ui);
  nk_style_pop_style_item(&ui);
  nk_style_pop_style_item(&ui);
  return clicked;
}
int gui_clipboard_call(struct process *process, bool write, uint32_t address,
                       uint32_t size) {
  if (size > sizeof(clipboard)) return -22;
  if (write) {
    static char incoming[sizeof(clipboard)];
    if (size >= sizeof(clipboard)) return -22;
    if (size && !process_copyin(process, incoming, address, size)) return -14;
    for (unsigned i = 0; i < size; i++)
      if (!incoming[i]) return -22;
    memcpy(clipboard, incoming, size);
    clipboard[size] = 0;
    clipboard_dirty = true;
    return size;
  }
  unsigned length = strlen(clipboard);
  if (!size) return length;
  unsigned count = length < size - 1 ? length : size - 1;
  if (!process_range(process, address, count + 1, true)) return -14;
  if (count && !process_copyout(process, address, clipboard, count)) return -14;
  const char zero = 0;
  if (!process_copyout(process, address + count, &zero, 1)) return -14;
  return count;
}
static void clipboard_copy(nk_handle handle, const char *text, int length) {
  (void)handle;
  if (length < 0)
    return;
  unsigned n = length;
  if (n >= sizeof(clipboard))
    n = sizeof(clipboard) - 1;
  memcpy(clipboard, text, n);
  clipboard[n] = 0;
  clipboard_dirty = true;
}
static void clipboard_paste(nk_handle handle, struct nk_text_edit *edit) {
  (void)handle;
  nk_textedit_paste(edit, clipboard, strlen(clipboard));
}
static void open_app(unsigned id) {
  if (id >= APP_COUNT)
    return;
  if (apps[id].visible) {
    workspace_focus[workspaces.homes[id]] = id;
    workspace_request(workspaces.homes[id]);
  }
  else { workspaces.homes[id] = workspaces.current; workspace_dirty = true; }
  focused_client = -1;
  if (id == IMAGES)
    picture_focus = true;
  if (id == MUSIC)
    music_focus = true;
  bool was_visible =
      apps[id].visible && !nk_window_is_collapsed(&ui, app_names[id]);
  apps[id].visible = true;
  if (!was_visible)
    gui_canvas_open(id + 1);
  launcher = false;
  active_app = id;
  focus_request = id;
  struct nk_window *window = nk_window_find(&ui, app_names[id]);
  if (window) {
    window->flags &= ~(NK_WINDOW_CLOSED | NK_WINDOW_HIDDEN |
                       NK_WINDOW_MINIMIZED | NK_WINDOW_ROM);
    nk_window_set_focus(&ui, app_names[id]);
  }
  if (id == EDITOR)
    editor_focus = 1;
  if (id == TERMINAL)
    terminal_focus = 1;
  if (id == TERMINAL && !terminal_pid)
    shell_start();
  notify(app_names[id]);
}
static void update_status(void) {
  kernel_status(&system_status);
  net_snapshot(&network_status);
  if (!tasks_paused) {
    uint32_t now = clock_ticks(), elapsed = now - task_sample_time;
    bool sample = !task_sample_valid || elapsed >= 100;
    uint64_t cpu_total = user_cpu_ticks();
    task_count = 0;
    task_slots = 0;
    for (unsigned i = 0; i < 256; i++) {
      struct task_info info;
      if (user_task_info(i, &info) && info.state != TASK_ZOMBIE) {
        unsigned usage = task_cpu[i];
        if (sample) {
          usage = task_sample_valid && elapsed &&
                          task_cpu_pid[i] == (unsigned)info.pid
                      ? (unsigned)((uint64_t)(info.cpu - task_previous_cpu[i]) *
                                   100 / elapsed)
                      : 0;
          if (usage > 100)
            usage = 100;
          task_previous_cpu[i] = info.cpu;
          task_cpu_pid[i] = info.pid;
          task_cpu[i] = usage;
        }
        task_items[task_slots] = info;
        /* Keep the sample with its task when process slots have holes. */
        task_slots++;
        task_count++;
      } else if (sample) {
        task_cpu_pid[i] = task_previous_cpu[i] = task_cpu[i] = 0;
      }
    }
    if (sample) {
      uint64_t percentage =
          task_sample_valid && elapsed
              ? (cpu_total - previous_user_ticks) * 100 / elapsed
              : 0;
      userspace_cpu = percentage > 100 ? 100 : (unsigned)percentage;
      previous_user_ticks = cpu_total;
      cpu_samples[sample_at] = userspace_cpu;
      memory_samples[sample_at] =
          system_status.managed_kib
              ? (unsigned)((uint64_t)system_status.allocated_kib * 100 /
                           system_status.managed_kib)
              : 0;
      sample_at = (sample_at + 1) % 60;
      if (sample_count < 60)
        sample_count++;
      task_sample_time = now;
      task_sample_valid = true;
    }
    bool selected_exists = selected_pid == 0;
    for (unsigned i = 0; i < task_slots; i++)
      if (task_items[i].pid == selected_pid)
        selected_exists = true;
    if (!selected_exists) {
      selected_pid = 0;
      ending_task = false;
    }
  }
  char date[128];
  capture("date", date, sizeof(date));
  copy(clock_text, date, sizeof(clock_text));
  for (unsigned i = 0; clock_text[i]; i++)
    if (clock_text[i] == '\n' || clock_text[i] == '\r' ||
        clock_text[i] == '(') {
      clock_text[i] = 0;
      break;
    }
}
static bool join_path(char out[256], const char *parent, const char *name) {
  unsigned a = strlen(parent), b = strlen(name);
  unsigned separator = !a || parent[a - 1] != '/';
  if (a + b + separator + 1 > 256)
    return false;
  memcpy(out, parent, a);
  if (!a || out[a - 1] != '/')
    out[a++] = '/';
  memcpy(out + a, name, b + 1);
  return true;
}
static void scan_files(const char *folder, unsigned depth) {
  for (unsigned index = 0; index < 65536; index++) {
    if (file_scanned++ >= 32768) {
      search_limited = true;
      return;
    }
    struct file_item item = {0};
    int r = fs_browse(folder, index, item.name, &item.size, &item.directory);
    if (r == -2)
      break;
    if (r) {
      notify("Unable to read this folder");
      break;
    }
    if (str_eq(item.name, ".") || str_eq(item.name, ".."))
      continue;
    if (!join_path(item.path, folder, item.name)) {
      search_limited = true;
      continue;
    }
    if (contains(item.name, file_filter)) {
      if (file_matches++ >= file_page) {
        if (file_count < 64)
          file_items[file_count++] = item;
        else
          file_more = true;
      }
    }
    if (search_subfolders && *file_filter && item.directory) {
      if (depth < 16)
        scan_files(item.path, depth + 1);
      else
        search_limited = true;
    }
    if (search_limited && file_scanned >= 32768)
      return;
  }
}
static int file_compare(const struct file_item *a, const struct file_item *b) {
  if (a->directory != b->directory)
    return a->directory ? -1 : 1;
  for (unsigned i = 0;; i++) {
    unsigned char x = a->name[i], y = b->name[i];
    if (x >= 'A' && x <= 'Z')
      x += 32;
    if (y >= 'A' && y <= 'Z')
      y += 32;
    if (x != y || !x || !y)
      return (int)x - y;
  }
}
static void refresh_files(void) {
  file_count = file_matches = file_scanned = 0;
  file_more = search_limited = false;
  selected_file = file_last_click = -1;
  deleting_file = renaming_file = false;
  emptying_trash = false;
  scan_files(browser_path, 0);
  for (unsigned i = 1; i < file_count; i++) {
    struct file_item item = file_items[i];
    unsigned j = i;
    while (j && file_compare(&item, &file_items[j - 1]) < 0) {
      file_items[j] = file_items[j - 1];
      j--;
    }
    file_items[j] = item;
  }
}
static bool canonical_path(char out[256], const char *path) {
  char full[256];
  if (*path == '/')
    copy(full, path, sizeof(full));
  else if (!join_path(full, browser_path, path))
    return false;
  if (strlen(path) >= 256)
    return false;
  unsigned used = 1;
  out[0] = '/';
  out[1] = 0;
  for (unsigned at = 0; full[at];) {
    while (full[at] == '/')
      at++;
    unsigned start = at;
    while (full[at] && full[at] != '/')
      at++;
    unsigned n = at - start;
    if (!n || (n == 1 && full[start] == '.'))
      continue;
    if (n == 2 && full[start] == '.' && full[start + 1] == '.') {
      while (used > 1 && out[used - 1] != '/')
        used--;
      if (used > 1)
        used--;
      out[used] = 0;
      continue;
    }
    if (used > 1)
      out[used++] = '/';
    if (used + n >= 256)
      return false;
    memcpy(out + used, full + start, n);
    used += n;
    out[used] = 0;
  }
  return true;
}
static void browse(const char *path) {
  char normalized[256];
  if (!canonical_path(normalized, path)) {
    notify("The path is too long");
    return;
  }
  int r = fs_directory(normalized);
  if (r) {
    notify("That folder is unavailable");
    return;
  }
  if (!file_history_count ||
      !str_eq(file_history[file_history_at], normalized)) {
    file_history_count = file_history_count ? file_history_at + 1 : 0;
    if (file_history_count == 32) {
      memmove(file_history, file_history + 1, sizeof(file_history[0]) * 31);
      file_history_count--;
    }
    file_history_at = file_history_count++;
    copy(file_history[file_history_at], normalized, 256);
  }
  copy(browser_path, normalized, sizeof(browser_path));
  copy(browser_address, normalized, sizeof(browser_address));
  file_filter[0] = 0;
  file_page = 0;
  refresh_files();
}
static void open_file(unsigned index) {
  if (index >= file_count)
    return;
  char path[256];
  copy(path, file_items[index].path, sizeof(path));
  if (file_items[index].directory) {
    browse(path);
    return;
  }
  unsigned length = strlen(path);
  bool manifest = length >= 15 && str_eq(path + length - 15, ".mgnta.manifest");
  bool bundle = length >= 10 && str_eq(path + length - 10, ".mgnta.zip");
  bool application = length >= 6 && str_eq(path + length - 6, ".mgnta");
  if (bundle || manifest || (application && length + 9 < sizeof(path))) {
    if (application) append(path, ".manifest", sizeof(path));
    if (bundle || manifest || fs_size(path) >= 0) {
      copy(local_manifest, path, sizeof(local_manifest));
      open_app(PACKAGES);
      return;
    }
    path[length] = 0;
  }
  unsigned char ident[64];
  int ident_size = fs_read_at(path, 0, ident, sizeof(ident));
  if (ident_size < 0) { notify("Could not read this file"); return; }
  enum file_type type = file_type_detect(path, ident, ident_size);
  if (type == FILE_EXECUTABLE) {
    if (user_spawn(path, "", false) < 0) notify("Program launch failed");
    return;
  }
  unsigned handler = default_handler(type);
  if (handler == MUSIC) {
    copy(music_path, path, sizeof(music_path));
    music_play_requested = true;
    open_app(MUSIC);
    return;
  }
  if (handler == IMAGES) {
    copy(picture_path, path, sizeof(picture_path));
    picture_load_requested = true;
    open_app(IMAGES);
    return;
  }
  if (handler == BROWSER) {
    char address[264] = "file:";
    append(address, path, sizeof(address));
    browser_new_tab(address);
    open_app(BROWSER);
    return;
  }
  if (editor_dirty) {
    notify("Save or discard the current document first");
    open_app(EDITOR);
    return;
  }
  if (file_items[index].size > FS_MAX_FILE) {
    notify("This editor accepts documents up to 64 KiB");
    return;
  }
  static char contents[FS_MAX_FILE + 1];
  int n = fs_read(path, contents, FS_MAX_FILE);
  if (n < 0) {
    notify("Could not open this file");
    return;
  }
  for (int i = 0; i < n; i++)
    if (((unsigned char)contents[i] < 32 && contents[i] != '\n' &&
         contents[i] != '\t') ||
        (unsigned char)contents[i] > 126) {
      notify("Select a text document to edit");
      return;
    }
  contents[n] = 0;
  memcpy(editor, contents, n + 1);
  copy(editor_path, path, sizeof(editor_path));
  editor_save_as = false;
  editor_dirty = false;
  open_app(EDITOR);
}
static void save_editor(void) {
  int n = fs_write(editor_path, editor, strlen(editor));
  if (n >= 0) {
    editor_dirty = false;
    if (editor_draft_written && !editor_draft_available &&
        fs_remove(EDITOR_DRAFT_PATH) == 0)
      editor_draft_written = false;
    notify("File saved");
    refresh_files();
  } else
    notify("File save failed");
}
static void refresh_packages(void) {
  static char list[16384];
  capture("pkg list", list, sizeof(list));
  package_count = 0;
  char *p = list;
  while (*p && package_count < 256) {
    char *line = p;
    while (*p && *p != '\n')
      p++;
    if (*p)
      *p++ = 0;
    struct package_item item = {0};
    char *space = line;
    while (*space && *space != ' ')
      space++;
    if (!*space)
      continue;
    *space++ = 0;
    copy(item.name, line, sizeof(item.name));
    char *end = space;
    while (*end && *end != ' ')
      end++;
    if (*end) {
      *end++ = 0;
      copy(item.version, space, sizeof(item.version));
      item.installed = contains(end, "installed");
    } else {
      item.installed = contains(space, "installed");
      copy(item.version, "HTTP", sizeof(item.version));
    }
    package_items[package_count++] = item;
  }
  if (selected_package >= (int)package_count)
    selected_package = 0;
  if (package_count) {
    char cmd[96] = "pkg query ";
    append(cmd, package_items[selected_package].name, sizeof(cmd));
    capture(cmd, package_details, sizeof(package_details));
  }
}
static void package_action(const char *verb) {
  if (!package_count)
    return;
  char command[128] = "pkg ";
  append(command, verb, sizeof(command));
  append(command, " ", sizeof(command));
  append(command, package_items[selected_package].name, sizeof(command));
  capture(command, package_result, sizeof(package_result));
  refresh_packages();
  refresh_files();
  if (*package_result)
    notify(package_result);
  else
    notify("Package database updated");
}
static void shell_start(void) {
  if (terminal_pid)
    return;
  terminal_pid = user_spawn("ush", "", true);
  if (terminal_pid < 0) {
    terminal_pid = 0;
    terminal_append("Could not start the shell.\n");
  }
}
static char *terminal_trim(char *text) {
  while (*text == ' ' || *text == '\t') text++;
  unsigned length = strlen(text);
  while (length && (text[length - 1] == ' ' || text[length - 1] == '\t'))
    text[--length] = 0;
  return text;
}
static int terminal_text_stage(char *stage, char *input, unsigned length,
                               bool piped, char *out) {
  char *args = stage;
  while (*args && *args != ' ' && *args != '\t') args++;
  if (*args) *args++ = 0;
  args = terminal_trim(args);
  if (!piped && str_eq(stage, "ps") && !*args) {
    capture("ps", out, FS_MAX_FILE + 1);
    return strlen(out);
  }
  if (!piped && str_eq(stage, "echo")) {
    unsigned n = strlen(args);
    memcpy(out, args, n);
    out[n++] = '\n';
    return n;
  }
  bool cat = str_eq(stage, "cat"), head = str_eq(stage, "head"),
       grep = str_eq(stage, "grep"), wc = str_eq(stage, "wc");
  if (!cat && !head && !grep && !wc) return -95;
  char *pattern = NULL;
  if (grep) {
    pattern = args;
    while (*args && *args != ' ' && *args != '\t') args++;
    if (*args) *args++ = 0;
    args = terminal_trim(args);
    if (!*pattern) return -22;
  }
  if (*args) {
    int bytes = fs_read(args, input, FS_MAX_FILE);
    if (bytes < 0) return bytes;
    length = bytes;
  } else if (!piped)
    return -22;
  if (cat) {
    memcpy(out, input, length);
    return length;
  }
  if (wc) {
    unsigned lines = 0, words = 0;
    bool in_word = false;
    for (unsigned i = 0; i < length; i++) {
      char c = input[i];
      lines += c == '\n';
      bool word = c != ' ' && c != '\t' && c != '\r' && c != '\n';
      words += word && !in_word;
      in_word = word;
    }
    char value[32];
    number(out, lines);
    append(out, " lines ", FS_MAX_FILE + 1);
    number(value, words);
    append(out, value, FS_MAX_FILE + 1);
    append(out, " words ", FS_MAX_FILE + 1);
    number(value, length);
    append(out, value, FS_MAX_FILE + 1);
    append(out, " bytes\n", FS_MAX_FILE + 1);
    return strlen(out);
  }
  unsigned used = 0, lines = 0, pattern_length = pattern ? strlen(pattern) : 0;
  for (unsigned start = 0; start < length;) {
    unsigned end = start;
    while (end < length && input[end] != '\n') end++;
    bool match = head;
    for (unsigned i = start; !match && pattern_length <= end - i; i++)
      match = !__builtin_memcmp(input + i, pattern, pattern_length);
    if (end < length) end++;
    if (match) {
      memcpy(out + used, input + start, end - start);
      used += end - start;
      if (head && ++lines == 10) break;
    }
    start = end;
  }
  return used;
}
static bool terminal_pipeline(const char *command) {
  bool operators = false;
  for (const char *p = command; *p; p++)
    operators |= *p == '|' || *p == '>';
  if (!operators) return false;
  char line[sizeof(terminal_line)];
  copy(line, command, sizeof(line));
  static char buffers[2][FS_MAX_FILE + 1];
  unsigned side = 0, length = 0;
  bool piped = false;
  for (char *stage = line;;) {
    char *end = stage;
    while (*end && *end != '|' && *end != '>') end++;
    char op = *end;
    *end = 0;
    int n = terminal_text_stage(terminal_trim(stage), buffers[side], length,
                                piped, buffers[side ^ 1]);
    if (n < 0) {
      terminal_append(n == -95
          ? "Pipes support ps, echo, cat, head, grep and wc.\n"
          : "Pipeline command failed.\n");
      return true;
    }
    side ^= 1;
    length = n;
    buffers[side][length] = 0;
    if (op == '|') {
      stage = end + 1;
      piped = true;
      continue;
    }
    if (!op) {
      terminal_append(buffers[side]);
      return true;
    }
    bool append_output = end[1] == '>';
    char *target = terminal_trim(end + (append_output ? 2 : 1));
    for (const char *p = target; *p; p++)
      if (*p == '|' || *p == '>') {
        terminal_append("Redirection must come last.\n");
        return true;
      }
    if (!*target) {
      terminal_append("Enter an output filename.\n");
      return true;
    }
    char *output = buffers[side];
    if (append_output) {
      struct file_metadata metadata;
      int stat = fs_metadata(target, &metadata);
      int previous = stat == -2 ? 0 :
          fs_read(target, buffers[side ^ 1], FS_MAX_FILE - length);
      if (previous < 0) {
        terminal_append("Could not read the append destination.\n");
        return true;
      }
      output = buffers[side ^ 1];
      memcpy(output + previous, buffers[side], length);
      length += previous;
    }
    if (fs_write(target, output, length) < 0)
      terminal_append("Could not write redirected output.\n");
    else
      refresh_files();
    return true;
  }
}
static void shell_submit(void) {
  if (terminal_pipeline(terminal_line)) {
    terminal_line[0] = 0;
    terminal_focus = 1;
    return;
  }
  if (str_eq(terminal_line,"netdiag") || str_eq(terminal_line,"netprofile") ||
      str_eq(terminal_line,"route") ||
      (strlen(terminal_line)>=11 && !__builtin_memcmp(terminal_line,"netprofile ",11)) ||
      (strlen(terminal_line)>=6 && !__builtin_memcmp(terminal_line,"route ",6))) {
    static char output[8192];capture(terminal_line,output,sizeof(output));
    terminal_append(output);terminal_line[0]=0;terminal_focus=1;return;
  }
  if (str_eq(terminal_line, "sandbox") ||
      (strlen(terminal_line) >= 8 && !__builtin_memcmp(terminal_line, "sandbox ", 8))) {
    static char output[8192]; capture(terminal_line, output, sizeof(output));
    terminal_append(output); terminal_line[0] = 0; terminal_focus = 1; return;
  }
  if (str_eq(terminal_line, "nvmehealth") ||
      (strlen(terminal_line) >= 11 && !__builtin_memcmp(terminal_line, "nvmehealth ", 11))) {
    static char output[8192];
    capture(terminal_line, output, sizeof(output));
    terminal_append(output); terminal_line[0] = 0; terminal_focus = 1; return;
  }
  if (str_eq(terminal_line, "workspace") ||
      (strlen(terminal_line) >= 10 && !__builtin_memcmp(terminal_line, "workspace ", 10))) {
    workspace_command(terminal_line + (strlen(terminal_line) >= 10 ? 10 : 9));
    terminal_line[0] = 0; terminal_focus = 1; return;
  }
  if (str_eq(terminal_line, "firewall") ||
      (strlen(terminal_line) >= 9 && !__builtin_memcmp(terminal_line, "firewall ", 9))) {
    static char output[8192];
    capture(terminal_line, output, sizeof(output));
    terminal_append(output);
    terminal_line[0] = 0;
    terminal_focus = 1;
    return;
  }
  if (str_eq(terminal_line, "reg list") ||
      str_eq(terminal_line, "registry list")) {
    static const char *keys[] = {"desktop.shortcuts",  "desktop.top_bar",
                                 "desktop.dock",       "desktop.icons",
                                 "desktop.icons_x",    "desktop.icons_y",
                                 "desktop.dock_bottom", "workspace.count", "workspace.current"};
    for (unsigned i = 0; i < 9; i++) {
      char value[32];
      number(value, *registry_slot(keys[i]));
      terminal_append(keys[i]);
      terminal_append(" = ");
      terminal_append(value);
      terminal_append("\n");
    }
    terminal_line[0] = 0;
    return;
  }
  if (terminal_line[0] == 'r' && terminal_line[1] == 'e' &&
      terminal_line[2] == 'g' && terminal_line[3] == ' ') {
    char *key = terminal_line + 4, *value = key;
    while (*value && *value != ' ')
      value++;
    if (*value)
      *value++ = 0;
    if (!*value) {
      uint32_t *slot = registry_slot(key);
      if (slot) {
        char output[32];
        number(output, *slot);
        terminal_append(output);
        terminal_append("\n");
      } else
        terminal_append("Unknown registry key\n");
    } else
      terminal_append(registry_set(key, value) ? "Registry saved\n"
                                               : "Invalid registry value\n");
    terminal_line[0] = 0;
    return;
  }
  if (!terminal_pid)
    shell_start();
  if (!terminal_pid)
    return;
  unsigned n = strlen(terminal_line);
  if (n) {
    copy(history[history_count % 16], terminal_line, sizeof(history[0]));
    history_count++;
    history_cursor = history_count;
  }
  for (unsigned i = 0; i < n; i++)
    user_tty_input(terminal_line[i]);
  user_tty_input('\n');
  terminal_line[0] = 0;
  terminal_focus = 1;
}
static void terminal_view(void) {
  nk_layout_row_dynamic(&ui, 28, 4);
  caption("Native shell / ring 3");
  if (nk_button_label(&ui, "Copy output"))
    clipboard_copy(nk_handle_ptr(NULL), terminal_output, terminal_used);
  if (nk_button_label(&ui, "Clear")) {
    terminal_used = 0;
    terminal_output[0] = 0;
  }
  if (nk_button_label(&ui, terminal_pid ? "Interrupt" : "Start shell")) {
    if (terminal_pid) {
      user_tty_input(3);
      terminal_line[0] = 0;
    } else
      shell_start();
  }
  float available = nk_window_get_content_region(&ui).h - 94;
  if (available < 80)
    available = 80;
  nk_layout_row_dynamic(&ui, available, 1);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                                 terminal_output, sizeof(terminal_output),
                                 nk_filter_default);
  float columns[] = {0.82f, 0.18f};
  nk_layout_row(&ui, NK_DYNAMIC, 36, 2, columns);
  if (terminal_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    terminal_focus = 0;
  }
  nk_flags flags = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, terminal_line,
      sizeof(terminal_line), nk_filter_default);
  bool submit = nk_button_label(&ui, "Run");
  if ((flags & NK_EDIT_COMMITTED) || submit)
    shell_submit();
}
static void format_size(char out[64], uint64_t bytes) {
  char digits[32];
  const char *unit = " B";
  uint64_t divisor = 1;
  if (bytes >= 1024ull * 1024 * 1024) {
    divisor = 1024ull * 1024 * 1024;
    unit = " GiB";
  } else if (bytes >= 1024 * 1024) {
    divisor = 1024 * 1024;
    unit = " MiB";
  } else if (bytes >= 1024) {
    divisor = 1024;
    unit = " KiB";
  }
  number(digits, bytes / divisor);
  copy(out, digits, 64);
  if (divisor > 1) {
    append(out, ".", 64);
    number(digits, (bytes % divisor) * 10 / divisor);
    append(out, digits, 64);
  }
  append(out, unit, 64);
}
static bool valid_item_name(const char *name) {
  if (!*name || str_eq(name, ".") || str_eq(name, "..") || contains(name, "/"))
    return false;
  for (; *name; name++)
    if ((unsigned char)*name < 32)
      return false;
  return true;
}
static void file_error(const char *action, int error) {
  char message[256];
  copy(message, action, sizeof(message));
  append(message,
         error == -17   ? ": an item with that name already exists"
         : error == -28 ? ": the disk or directory is full"
         : error == -30 ? ": the volume is read-only or lacks folder support"
         : error == -18 ? ": use Copy to transfer files between volumes"
         : error == -27 ? ": the filesystem's size limit was reached"
         : error == -39 ? ": the folder is not empty"
         : error == -95 ? ": unsupported file type or filesystem feature"
         : error == -22 ? ": the destination is inside the source"
         : error == -40 ? ": the folder nesting limit was reached"
                        : ": the filesystem could not complete the operation",
         sizeof(message));
  notify(message);
}
static void file_begin_create(bool folder) {
  creating_file = true;
  creating_folder = folder;
  renaming_file = false;
  new_name[0] = 0;
}
static void file_begin_rename(void) {
  if (selected_file < 0 || (unsigned)selected_file >= file_count)
    return;
  copy(rename_name, file_items[selected_file].name, sizeof(rename_name));
  renaming_file = true;
  creating_file = false;
}
static void file_begin_delete(void) {
  if (selected_file < 0 || (unsigned)selected_file >= file_count)
    return;
  copy(deleting_path, file_items[selected_file].path, sizeof(deleting_path));
  unsigned disk;
  const char *relative = deleting_path;
  (void)fs_secondary_path(deleting_path, &disk, &relative);
  if (str_eq(relative, "/") || str_eq(relative, "/.magenta-trash") ||
      starts_with(relative, "/.magenta-trash/")) {
    notify("Select an item outside Trash to move it to Trash.");
    return;
  }
  emptying_trash = false;
  deleting_file = true;
}
static void file_copy_selection(bool cut) {
  if (selected_file < 0 || (unsigned)selected_file >= file_count)
    return;
  copy(file_clipboard, file_items[selected_file].path, sizeof(file_clipboard));
  copy(file_clipboard_name, file_items[selected_file].name,
       sizeof(file_clipboard_name));
  file_cut = cut;
  notify(cut ? "Ready to move. Open a destination folder and Paste."
             : "Copied. Open a destination folder and Paste.");
}
static int remove_copy_tree(const char *path, unsigned depth) {
  if (depth > 16)
    return -40;
  if (fs_directory(path) == 0) {
    for (;;) {
      char name[256], child[256];
      uint64_t size;
      bool directory;
      int r = fs_browse(path, 0, name, &size, &directory);
      if (r == -2)
        break;
      if (r)
        return r;
      if (!join_path(child, path, name))
        return -36;
      r = remove_copy_tree(child, depth + 1);
      if (r)
        return r;
    }
  }
  return fs_remove(path);
}
#define TRASH_PATH "/.magenta-trash"
struct trash_record {
  char original[256];
  uint32_t days, seconds;
};
static void drive_path(char out[256], unsigned disk) {
  char id[32];
  number(id, disk);
  copy(out, "/.magenta-drive-", 256);
  append(out, id, 256);
}
static void trash_path(char out[256], const char *path) {
  unsigned disk;
  if (fs_secondary_path(path, &disk, NULL)) {
    drive_path(out, disk);
    append(out, TRASH_PATH, 256);
  } else
    copy(out, TRASH_PATH, 256);
}
static int move_to_trash(const char *path) {
  char trash[256];
  trash_path(trash, path);
  int r = fs_directory(trash);
  if (r && (r = fs_mkdir(trash)) < 0)
    return r;
  struct trash_record record = {0};
  unsigned disk;
  const char *relative = path;
  (void)fs_secondary_path(path, &disk, &relative);
  copy(record.original, relative, sizeof(record.original));
  (void)rtc_time(&record.days, &record.seconds);
  char entry[256], metadata[256], payload[256];
  const char *name = path;
  for (const char *p = path; *p; p++)
    if (*p == '/') name = p + 1;
  for (unsigned i = 0; i < 1024; i++) {
    char id[32], label[210];
    number(id, (uint64_t)record.days * 86400 + record.seconds + i);
    copy(label, id, sizeof(label));
    append(label, "-", sizeof(label));
    append(label, name, sizeof(label));
    if (!join_path(entry, trash, label) ||
        !join_path(metadata, entry, "original") ||
        !join_path(payload, entry, "item"))
      return -36;
    r = fs_mkdir(entry);
    if (r == -17) continue;
    if (r < 0) return r;
    r = fs_write(metadata, (const char *)&record, sizeof(record));
    if (r >= 0) r = fs_rename(path, payload);
    if (r < 0) {
      (void)fs_remove(metadata);
      (void)fs_remove(entry);
    }
    return r;
  }
  return -17;
}
static int restore_trash(const char *entry, const char *name) {
  char metadata[256], payload[256];
  if (!join_path(metadata, entry, "original") ||
      !join_path(payload, entry, "item"))
    return -36;
  struct trash_record record;
  if (fs_read(metadata, (char *)&record, sizeof(record)) != (int)sizeof(record) ||
      record.original[0] != '/' || record.original[255] ||
      starts_with(record.original, TRASH_PATH))
    return -22;
  if (name && *name) {
    if (!valid_item_name(name)) return -22;
    char parent[256];
    copy(parent, record.original, sizeof(parent));
    unsigned split = strlen(parent);
    while (split && parent[split - 1] != '/') split--;
    if (!split) return -22;
    parent[split - 1] = 0;
    if (!join_path(record.original, parent, name)) return -36;
  }
  char destination[256] = "";
  unsigned disk;
  if (fs_secondary_path(entry, &disk, NULL))
    drive_path(destination, disk);
  if (strlen(destination) + strlen(record.original) >= sizeof(destination))
    return -36;
  append(destination, record.original, sizeof(destination));
  int r = fs_rename(payload, destination);
  if (r < 0) return r;
  (void)fs_remove(metadata);
  (void)fs_remove(entry);
  return 0;
}
static int empty_trash(const char *trash) {
  for (;;) {
    char name[256], entry[256], metadata[256], payload[256];
    struct trash_record record;
    uint64_t size;
    bool directory;
    int r = fs_browse(trash, 0, name, &size, &directory);
    if (r == -2) return 0;
    if (r < 0) return r;
    if (!directory || !join_path(entry, trash, name) ||
        !join_path(metadata, entry, "original") ||
        !join_path(payload, entry, "item") ||
        fs_read(metadata, &record, sizeof(record)) != (int)sizeof(record) ||
        record.original[0] != '/' || record.original[255])
      return -22;
    r = remove_copy_tree(payload, 0);
    if (r < 0 && r != -2) return r;
    r = fs_remove(metadata);
    if (r < 0) return r;
    r = fs_remove(entry);
    if (r < 0) return r;
  }
}
static int copy_tree(const char *source, const char *target, unsigned depth) {
  if (depth > 16)
    return -40;
  if (fs_size(target) >= 0)
    return -17;
  if (fs_directory(source) != 0)
    return fs_copy(source, target);
  int r = fs_mkdir(target);
  if (r)
    return r;
  for (unsigned index = 0;; index++) {
    char name[256], from[256], to[256];
    uint64_t size;
    bool directory;
    r = fs_browse(source, index, name, &size, &directory);
    if (r == -2)
      return 0;
    if (r)
      break;
    if (!join_path(from, source, name) || !join_path(to, target, name)) {
      r = -36;
      break;
    }
    r = copy_tree(from, to, depth + 1);
    if (r)
      break;
    devices_poll();
    net_poll();
  }
  /* Only remove the destination tree we created; never the original. */
  if (remove_copy_tree(target, depth) < 0)
    notify("Copy failed; an incomplete copy remains in the destination.");
  return r;
}
static void file_paste(void) {
  if (!*file_clipboard)
    return;
  char destination[256];
  if (!join_path(destination, browser_path, file_clipboard_name)) {
    notify("The destination path is too long");
    return;
  }
  unsigned n = strlen(file_clipboard);
  if (strlen(destination) > n &&
      !__builtin_memcmp(destination, file_clipboard, n) &&
      destination[n] == '/') {
    notify("Choose a destination outside the original item.");
    return;
  }
  if (fs_size(destination) >= 0) {
    if (file_cut) {
      notify("An item with that name already exists. Rename it before moving.");
      return;
    }
    bool available = false;
    for (unsigned suffix = 1; suffix <= 999; suffix++) {
      char name[256], digits[32];
      number(digits, suffix);
      unsigned reserve = strlen(digits) + 7;
      copy(name, file_clipboard_name, sizeof(name) - reserve);
      append(name, "-copy-", sizeof(name));
      append(name, digits, sizeof(name));
      if (!join_path(destination, browser_path, name))
        break;
      if (fs_size(destination) < 0) {
        available = true;
        break;
      }
    }
    if (!available) {
      notify("No available name for the copy in this folder.");
      return;
    }
  }
  int r = file_cut ? fs_rename(file_clipboard, destination)
                   : copy_tree(file_clipboard, destination, 0);
  if (r < 0)
    file_error(file_cut ? "Move failed" : "Copy failed", r);
  else {
    if (file_cut)
      file_clipboard[0] = file_clipboard_name[0] = 0;
    refresh_files();
    notify(file_cut ? "Item moved" : "Item copied");
  }
}
static void file_history_go(int direction) {
  if ((direction < 0 && !file_history_at) ||
      (direction > 0 && file_history_at + 1 >= file_history_count))
    return;
  unsigned next = direction < 0 ? file_history_at - 1 : file_history_at + 1;
  if (fs_directory(file_history[next]) < 0) {
    notify("That location is no longer available");
    return;
  }
  file_history_at = next;
  copy(browser_path, file_history[next], sizeof(browser_path));
  copy(browser_address, browser_path, sizeof(browser_address));
  file_filter[0] = 0;
  file_page = 0;
  refresh_files();
}
static bool quiet_button(const char *label, const char *tip, bool enabled) {
  if (!enabled)
    nk_widget_disable_begin(&ui);
  bool hovered = nk_widget_is_hovered(&ui);
  bool clicked = nk_button_label(&ui, label);
  if (hovered && tip)
    nk_tooltip(&ui, tip);
  if (!enabled)
    nk_widget_disable_end(&ui);
  return clicked && enabled;
}
static void file_context_menu(struct nk_rect bounds) {
  if (nk_contextual_begin(&ui, 0, nk_vec2(248, 360), bounds)) {
    if (escape_requested) {
      nk_contextual_close(&ui);
      escape_requested = false;
    }
    nk_layout_row_dynamic(&ui, 30, 1);
    bool selected = selected_file >= 0 && (unsigned)selected_file < file_count;
    if (selected) {
      if (nk_contextual_item_label(&ui, "Open", NK_TEXT_LEFT))
        open_file(selected_file);
      if (nk_contextual_item_label(&ui, "Copy    Ctrl+C", NK_TEXT_LEFT))
        file_copy_selection(false);
      if (nk_contextual_item_label(&ui, "Cut       Ctrl+X", NK_TEXT_LEFT))
        file_copy_selection(true);
      if (nk_contextual_item_label(&ui, "Rename", NK_TEXT_LEFT))
        file_begin_rename();
      if (nk_contextual_item_label(&ui, "Delete", NK_TEXT_LEFT))
        file_begin_delete();
    }
    if (*file_clipboard &&
        nk_contextual_item_label(&ui, "Paste    Ctrl+V", NK_TEXT_LEFT))
      file_paste();
    if (nk_contextual_item_label(&ui, "New Folder", NK_TEXT_LEFT))
      file_begin_create(true);
    if (nk_contextual_item_label(&ui, "New File", NK_TEXT_LEFT))
      file_begin_create(false);
    if (nk_contextual_item_label(&ui, "Refresh", NK_TEXT_LEFT))
      refresh_files();
    nk_contextual_end(&ui);
  }
}
static bool desktop_drive_busy(unsigned disk) {
  unsigned volume;
  if (editor_dirty && fs_secondary_path(editor_path, &volume, NULL) &&
      volume == disk)
    return true;
  struct av_audio_status audio;
  audio_snapshot(&audio);
  return (audio.playing || audio_paused()) &&
         fs_secondary_path(audio.path, &volume, NULL) && volume == disk;
}
static void files_drives_view(void) {
  heading("Drives");
  nk_layout_row_dynamic(&ui, 32, 1);
  if (nk_button_label(&ui, "Back to Files")) {
    showing_drives = false;
    return;
  }
  for (unsigned disk = 0; disk < disk_count(); disk++) {
    int state = fs_secondary_state(disk);
    const struct block_device *device = disk_get(disk);
    if (!device || state == -19) continue;
    nk_layout_row_dynamic(&ui, 30, 1);
    nk_label(&ui, device->model, NK_TEXT_LEFT);
    if (state < 0) {
      caption("System volume");
      continue;
    }
    nk_layout_row_dynamic(&ui, 32, 3);
    if (nk_button_label(&ui, state ? "Open" : "Open read-only")) {
      int r = state ? 0 : fs_secondary_mount(disk, false);
      if (r < 0)
        notify("Could not open a clean ext4 volume on this drive");
      else {
        char path[256];
        drive_path(path, disk);
        file_filter[0] = 0;
        browse(path);
        showing_drives = false;
        return;
      }
    }
    if (quiet_button("Enable writes", "Allow changes on this ext4 volume",
                     state == 1 && !device->read_only)) {
      int r = fs_secondary_mount(disk, true);
      notify(r < 0 ? "Could not enable writes" : "Drive is writable");
    }
    if (quiet_button("Eject", "Unmount and flush the drive", state > 0)) {
      int r = desktop_drive_busy(disk) ? -16 : fs_secondary_eject(disk);
      if (r < 0)
        notify(r == -16 ? "Drive is busy" : "Could not safely eject the drive");
      else {
        unsigned current;
        if (fs_secondary_path(browser_path, &current, NULL) && current == disk)
          browse("/");
        notify(device->backend == 4 ? "Drive unmounted. Safe to remove."
                                    : "Drive unmounted.");
      }
    }
  }
}
#define DOWNLOAD_SLOTS 4
#define DOWNLOAD_LIMIT (1024u * 1024u)
enum download_state { DL_EMPTY, DL_RECEIVING, DL_READY, DL_FAILED, DL_CANCELLED, DL_SAVED };
static struct desktop_download {
  unsigned id, received, total;
  int owner;
  enum download_state state;
  char filename[128], destination[256];
  uint8_t data[DOWNLOAD_LIMIT];
} downloads[DOWNLOAD_SLOTS];
static unsigned download_sequence;
static struct desktop_download *download_begin(int owner, const char *filename,
                                                uint64_t total) {
  if (total > DOWNLOAD_LIMIT) return NULL;
  for (unsigned i = 0; i < DOWNLOAD_SLOTS; i++) {
    struct desktop_download *item = &downloads[i];
    if (item->state != DL_EMPTY) continue;
    item->id = download_sequence = download_sequence % INT32_MAX + 1;
    item->received = 0;
    item->total = total;
    item->owner = owner;
    item->state = DL_RECEIVING;
    copy(item->filename, valid_item_name(filename) ? filename : "download",
         sizeof(item->filename));
    if (!join_path(item->destination, browser_path, item->filename))
      join_path(item->destination, "/", item->filename);
    download_attention = true;
    return item;
  }
  return NULL;
}
void gui_download_disconnect(int pid) {
  for (unsigned i = 0; i < DOWNLOAD_SLOTS; i++)
    if (downloads[i].owner == pid && downloads[i].state == DL_RECEIVING)
      downloads[i].state = DL_FAILED;
}
int gui_download_call(struct process *process, uint32_t address) {
  struct av_download_request request;
  if (!running || !process_copyin(process, &request, address, sizeof(request)))
    return -14;
  if (request.operation == AV_DOWNLOAD_BEGIN) {
    if (!__builtin_memchr(request.filename, 0, sizeof(request.filename)))
      return -22;
    struct desktop_download *item =
        download_begin(process->pid, request.filename, request.total);
    return item ? (int)item->id : request.total > DOWNLOAD_LIMIT ? -27 : -28;
  }
  for (unsigned i = 0; i < DOWNLOAD_SLOTS; i++) {
    struct desktop_download *item = &downloads[i];
    if (item->state == DL_EMPTY || item->id != request.id ||
        item->owner != process->pid)
      continue;
    if (item->state != DL_RECEIVING) return -125;
    switch (request.operation) {
    case AV_DOWNLOAD_STATUS:
      return 0;
    case AV_DOWNLOAD_DATA:
      if (request.size > DOWNLOAD_LIMIT - item->received) {
        item->state = DL_FAILED;
        return -27;
      }
      if (request.data > UINT32_MAX ||
          !process_copyin(process, item->data + item->received, request.data,
                          request.size)) {
        item->state = DL_FAILED;
        return -14;
      }
      item->received += request.size;
      return 0;
    case AV_DOWNLOAD_DONE:
      item->state = !item->total || item->total == item->received
                        ? DL_READY : DL_FAILED;
      return item->state == DL_READY ? 0 : -5;
    case AV_DOWNLOAD_FAILED:
      item->state = DL_FAILED;
      return 0;
    default:
      return -22;
    }
  }
  return -2;
}
static int download_save(struct desktop_download *item) {
  char partial[256];
  if (item->destination[0] != '/' ||
      strlen(item->destination) + sizeof(".part") > sizeof(partial))
    return -22;
  copy(partial, item->destination, sizeof(partial));
  append(partial, ".part", sizeof(partial));
  struct file_metadata info;
  if (fs_metadata(item->destination, &info) != -2 ||
      fs_metadata(partial, &info) != -2)
    return -17;
  int result = fs_write_new_stream(partial, item->data, item->received);
  if (result < 0) return result;
  if ((unsigned)result != item->received) return -5;
  result = fs_rename(partial, item->destination);
  if (!result) item->state = DL_SAVED;
  return result;
}
static void downloads_view(void) {
  heading("Downloads");
  nk_layout_row_dynamic(&ui, 32, 1);
  if (nk_button_label(&ui, "Back to Files")) {
    showing_downloads = false;
    return;
  }
  float remaining = nk_window_get_content_region(&ui).h - 90;
  nk_layout_row_dynamic(&ui, remaining > 80 ? remaining : 80, 1);
  if (!nk_group_begin(&ui, "Browser downloads", 0)) {
    nk_group_end(&ui);
    return;
  }
  unsigned count = 0;
  for (unsigned i = 0; i < DOWNLOAD_SLOTS; i++) {
    struct desktop_download *item = &downloads[i];
    if (item->state == DL_EMPTY) continue;
    count++;
    heading(item->filename);
    nk_layout_row_dynamic(&ui, 26, 2);
    static const char *states[] = {
        "", "Receiving", "Ready to save", "Incomplete", "Cancelled", "Saved"};
    caption(states[item->state]);
    char size[64];
    format_size(size, item->received);
    caption(size);
    if (item->state == DL_RECEIVING && item->total) {
      nk_size progress = item->received < item->total ? item->received : item->total;
      nk_layout_row_dynamic(&ui, 16, 1);
      nk_progress(&ui, &progress, item->total, NK_FIXED);
    }
    if (item->state == DL_READY) {
      nk_layout_row_dynamic(&ui, 34, 2);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, item->destination,
                                     sizeof(item->destination), nk_filter_default);
      if (accent_button("Save to this path")) {
        int r = download_save(item);
        if (r < 0) file_error("Download save failed", r);
      }
    }
    nk_layout_row_dynamic(&ui, 34, 2);
    if (item->state == DL_RECEIVING || item->state == DL_READY) {
      if (nk_button_label(&ui, "Cancel")) item->state = DL_CANCELLED;
    } else if (nk_button_label(&ui, "Dismiss")) item->state = DL_EMPTY;
    if (item->state == DL_SAVED && nk_button_label(&ui, "Open containing folder")) {
      char folder[256];
      copy(folder, item->destination, sizeof(folder));
      unsigned slash = strlen(folder);
      while (slash > 1 && folder[slash - 1] != '/') slash--;
      folder[slash > 1 ? slash - 1 : 1] = 0;
      browse(folder);
      showing_downloads = false;
      break;
    }
  }
  if (!count) {
    nk_layout_row_dynamic(&ui, 36, 1);
    caption("No browser downloads");
  }
  nk_group_end(&ui);
}
static void file_contents_view(void) {
  if (showing_downloads) {
    downloads_view();
    return;
  }
  if (showing_drives) {
    files_drives_view();
    return;
  }
  files_editing = false;
  const float toolbar[] = {0.09f, 0.09f, 0.09f, 0.60f, 0.13f};
  nk_layout_row(&ui, NK_DYNAMIC, 36, 5, toolbar);
  if (quiet_button("<", "Back", file_history_count && file_history_at > 0))
    file_history_go(-1);
  if (quiet_button(">", "Forward", file_history_at + 1 < file_history_count))
    file_history_go(1);
  if (quiet_button("^", "Parent folder", !str_eq(browser_path, "/")))
    browse("..");
  if (files_address_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    files_address_focus = 0;
  }
  nk_flags entered = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_AUTO_SELECT,
      browser_address, sizeof(browser_address), nk_filter_default);
  files_editing |= (entered & NK_EDIT_ACTIVE) != 0;
  if (quiet_button("Go", "Open location (Ctrl+L)", true) ||
      (entered & NK_EDIT_COMMITTED))
    browse(browser_address);

  const float search[] = {0.12f, 0.54f, 0.34f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 3, search);
  caption("Search");
  char previous[64];
  copy(previous, file_filter, sizeof(previous));
  if (files_search_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    files_search_focus = 0;
  }
  nk_flags search_flags = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_AUTO_SELECT, file_filter,
      sizeof(file_filter), nk_filter_default);
  files_editing |= (search_flags & NK_EDIT_ACTIVE) != 0;
  nk_bool recursive_before = search_subfolders;
  nk_checkbox_label(&ui, "Subfolders", &search_subfolders);
  if (!str_eq(previous, file_filter) || recursive_before != search_subfolders) {
    file_page = 0;
    refresh_files();
  }

  nk_layout_row_dynamic(&ui, 32, 4);
  if (quiet_button("New File", "Create an empty document", true))
    file_begin_create(false);
  if (quiet_button("New Folder", "Create a folder", true))
    file_begin_create(true);
  if (quiet_button("Paste", "Paste the copied or cut item",
                   *file_clipboard != 0))
    file_paste();
  if (quiet_button("Refresh", "Read this folder again", true))
    refresh_files();

  bool selected = selected_file >= 0 && (unsigned)selected_file < file_count;
  char trash[256];
  trash_path(trash, browser_path);
  bool in_trash = str_eq(browser_path, trash);
  nk_layout_row_dynamic(&ui, 32, 5);
  if (nk_button_label(&ui, "Drives")) {
    showing_drives = true;
    return;
  }
  if (nk_button_label(&ui, "Downloads")) {
    showing_downloads = true;
    return;
  }
  if (quiet_button("Trash", "Show items moved to Trash", !in_trash)) {
    if (fs_directory(trash) == 0) {
      file_filter[0] = 0;
      browse(trash);
      return;
    }
    notify("Trash is empty");
  }
  if (quiet_button("Restore", "Restore the selected item", in_trash && selected)) {
    int r = restore_trash(file_items[selected_file].path, NULL);
    if (r == -17) {
      copy(restore_entry, file_items[selected_file].path, sizeof(restore_entry));
      restore_name[0] = 0;
      notify("The original name is in use. Choose a new name.");
    } else if (r < 0) file_error("Restore failed", r);
    else notify("Item restored");
    refresh_files();
    return;
  }
  if (quiet_button("Empty Trash", "Permanently remove items in Trash", in_trash)) {
    emptying_trash = true;
    deleting_file = true;
  }
  if (in_trash && *restore_entry) {
    nk_layout_row_dynamic(&ui, 32, 3);
    nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, restore_name,
                                   sizeof(restore_name), nk_filter_default);
    if (quiet_button("Restore as", "Restore under a new name",
                     valid_item_name(restore_name))) {
      int r = restore_trash(restore_entry, restore_name);
      if (r < 0) file_error("Restore failed", r);
      else {
        restore_entry[0] = 0;
        refresh_files();
        notify("Item restored");
        return;
      }
    }
    if (nk_button_label(&ui, "Cancel")) restore_entry[0] = 0;
  }
  nk_layout_row_dynamic(&ui, 32, 5);
  if (quiet_button("Open", "Open the selected item", selected))
    open_file(selected_file);
  if (quiet_button("Copy", "Copy the selected item", selected))
    file_copy_selection(false);
  if (quiet_button("Cut", "Move the selected item on Paste", selected))
    file_copy_selection(true);
  if (quiet_button("Rename", "Rename the selected item", selected))
    file_begin_rename();
  if (quiet_button("Delete", "Delete the selected item", selected))
    file_begin_delete();

  if (creating_file || (renaming_file && selected_file >= 0)) {
    nk_layout_row_dynamic(&ui, 24, 1);
    caption(renaming_file     ? "Rename selected item"
            : creating_folder ? "New folder name"
                              : "New file name");
    const float fields[] = {0.56f, 0.25f, 0.19f};
    nk_layout_row(&ui, NK_DYNAMIC, 34, 3, fields);
    char *name = renaming_file ? rename_name : new_name;
    unsigned capacity = renaming_file ? sizeof(rename_name) : sizeof(new_name);
    nk_flags result =
        nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
                                       name, capacity, nk_filter_default);
    files_editing |= (result & NK_EDIT_ACTIVE) != 0;
    bool apply = accent_button(renaming_file ? "Rename" : "Create");
    bool cancel = nk_button_label(&ui, "Cancel");
    if (apply || (result & NK_EDIT_COMMITTED)) {
      char target[256], parent[256];
      copy(parent, browser_path, sizeof(parent));
      if (renaming_file) {
        copy(parent, file_items[selected_file].path, sizeof(parent));
        unsigned end = strlen(parent);
        while (end > 1 && parent[end - 1] != '/')
          end--;
        if (end > 1)
          end--;
        parent[end] = 0;
      }
      if (!valid_item_name(name) || !join_path(target, parent, name))
        notify("Enter a valid name without slashes, . or ..");
      else if (fs_size(target) >= 0)
        notify("An item with that name already exists");
      else {
        bool was_folder = creating_folder, was_rename = renaming_file;
        int r = was_rename   ? fs_rename(file_items[selected_file].path, target)
                : was_folder ? fs_mkdir(target)
                             : fs_write(target, "", 0);
        if (r < 0)
          file_error(was_rename ? "Rename failed" : "Creation failed", r);
        else {
          creating_file = renaming_file = false;
          refresh_files();
          notify(was_rename   ? "Item renamed"
                 : was_folder ? "Folder created"
                              : "File created");
        }
      }
    }
    if (cancel)
      creating_file = renaming_file = false;
  }

  float area = nk_window_get_content_region(&ui).h -
               (creating_file || renaming_file ? 330 : 254);
  if (area < 80)
    area = 80;
  nk_layout_row_dynamic(&ui, area, 1);
  struct nk_rect list_bounds = nk_widget_bounds(&ui);
  if (nk_group_begin(&ui, "Folder contents", 0)) {
    const float columns[] = {0.09f, 0.65f, 0.26f};
    nk_layout_row(&ui, NK_DYNAMIC, 24, 3, columns);
    caption("");
    caption("NAME");
    caption("SIZE / TYPE");
    int open_index = -1;
    bool right_on_item = false;
    for (unsigned i = 0; i < file_count; i++) {
      nk_layout_row(&ui, NK_DYNAMIC, 38, 3, columns);
      struct nk_rect icon = nk_widget_bounds(&ui);
      nk_spacing(&ui, 1);
      gui_canvas_icon(
          nk_window_get_canvas(&ui), nk_rect(icon.x + 2, icon.y + 7, 24, 24),
          file_items[i].directory ? FILES : EDITOR,
          rgb(file_items[i].directory ? accent() : palette()->muted));
      struct nk_rect row_bounds = nk_widget_bounds(&ui);
      row_bounds.x = icon.x;
      row_bounds.w = nk_window_get_content_region(&ui).w;
      nk_bool row_selected = selected_file == (int)i;
      if (nk_selectable_label(&ui, file_items[i].name, NK_TEXT_LEFT,
                              &row_selected)) {
        selected_file = i;
        uint32_t now = clock_ticks();
        if (file_last_click == (int)i &&
            (uint32_t)(now - file_last_click_time) < 40)
          open_index = i;
        file_last_click = i;
        file_last_click_time = now;
      }
      if (nk_input_is_mouse_hovering_rect(&ui.input, row_bounds) &&
          nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_RIGHT)) {
        selected_file = i;
        right_on_item = true;
      }
      if (file_items[i].directory)
        caption("Folder");
      else {
        char size[64];
        format_size(size, file_items[i].size);
        caption(size);
      }
    }
    if (!file_count) {
      nk_layout_row_dynamic(&ui, 44, 1);
      nk_label(&ui,
               *file_filter ? "No files match your search"
                            : "This folder is empty",
               NK_TEXT_CENTERED);
      nk_layout_row_dynamic(&ui, 50, 1);
      nk_label_wrap(
          &ui, *file_filter
                   ? "Try a shorter name or enable Subfolders to search deeper."
                   : "Create a file or folder, or paste an item here.");
    }
    if (!right_on_item &&
        nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_RIGHT) &&
        nk_input_is_mouse_hovering_rect(&ui.input, list_bounds))
      selected_file = -1;
    nk_group_end(&ui);
    file_context_menu(list_bounds);
    if (open_index >= 0)
      open_file(open_index);
  }
  const float footer[] = {0.20f, 0.60f, 0.20f};
  nk_layout_row(&ui, NK_DYNAMIC, 30, 3, footer);
  if (quiet_button("< Previous", "Previous page", file_page >= 64)) {
    file_page -= 64;
    refresh_files();
  }
  char digits[32], summary[128];
  number(digits, file_matches);
  copy(summary, digits, sizeof(summary));
  append(summary, *file_filter ? " matches" : " items", sizeof(summary));
  if (search_limited)
    append(summary, " (search limit reached)", sizeof(summary));
  caption(summary);
  if (quiet_button("Next >", "Next page", file_more)) {
    file_page += 64;
    refresh_files();
  }
}
static void files_view(void) {
  bool sidebar = nk_window_get_content_region(&ui).w >= 650;
  const float columns[] = {0.23f, 0.77f};
  if (sidebar)
    nk_layout_row(&ui, NK_DYNAMIC, nk_window_get_content_region(&ui).h - 8, 2,
                  columns);
  else
    nk_layout_row_dynamic(&ui, nk_window_get_content_region(&ui).h - 8, 1);
  if (sidebar &&
      nk_group_begin(&ui, "File locations", NK_WINDOW_NO_SCROLLBAR)) {
    heading("Files");
    nk_layout_row_dynamic(&ui, 24, 1);
    caption("PLACES");
    nk_layout_row_dynamic(&ui, 38, 1);
    if (nk_button_label(&ui, "Home")) {
      file_filter[0] = 0;
      browse("/");
    }
    nk_layout_row_dynamic(&ui, 38, 1);
    if (nk_button_label(&ui, "Applications"))
      launcher = true;
    nk_layout_row_dynamic(&ui, 38, 1);
    if (nk_button_label(&ui, "Storage")) {
      settings_page = 2;
      open_app(SETTINGS);
    }
    nk_layout_row_dynamic(&ui, 24, 1);
    caption("CLIPBOARD");
    nk_layout_row_dynamic(&ui, 56, 1);
    nk_label_wrap(&ui,
                  *file_clipboard
                      ? file_clipboard_name
                      : "Copy or cut an item to paste it into another folder.");
    if (*file_clipboard) {
      nk_layout_row_dynamic(&ui, 24, 1);
      caption(file_cut ? "Ready to move" : "Ready to copy");
      nk_layout_row_dynamic(&ui, 32, 1);
      if (nk_button_label(&ui, "Clear Clipboard"))
        file_clipboard[0] = 0;
    }
    if (selected_file >= 0 && (unsigned)selected_file < file_count) {
      nk_layout_row_dynamic(&ui, 24, 1);
      caption("SELECTED ITEM");
      nk_layout_row_dynamic(&ui, 56, 1);
      nk_label_wrap(&ui, file_items[selected_file].name);
      nk_layout_row_dynamic(&ui, 24, 1);
      if (file_items[selected_file].directory)
        caption("Folder");
      else {
        char size[64];
        format_size(size, file_items[selected_file].size);
        caption(size);
      }
      nk_layout_row_dynamic(&ui, 80, 1);
      nk_label_wrap(&ui, file_items[selected_file].path);
    }
    nk_layout_row_dynamic(&ui, 40, 1);
    caption(fs_mounted_disk() < 0 ? "Session storage" : "Persistent disk");
    nk_group_end(&ui);
  }
  if (nk_group_begin(&ui, "File browser", NK_WINDOW_NO_SCROLLBAR)) {
    file_contents_view();
    nk_group_end(&ui);
  }
}
static void editor_recovery_poll(void) {
  if (!editor_dirty || editor_draft_available || !fs_writable())
    return;
  uint32_t now = clock_ticks();
  if ((uint32_t)(now - editor_draft_tick) < 200)
    return;
  editor_draft_tick = now;
  unsigned length = strlen(editor);
  uint32_t checksum = crc32(editor, length) ^ crc32(editor_path, strlen(editor_path));
  if (editor_draft_written && editor_draft_crc == checksum)
    return;
  copy(editor_draft.path, editor_path, sizeof(editor_draft.path));
  memcpy(editor_draft.text, editor, length + 1);
  if (fs_write(EDITOR_DRAFT_PATH, &editor_draft, sizeof(editor_draft.path) +
                                                    length + 1) >= 0) {
    editor_draft_written = true;
    editor_draft_crc = checksum;
  }
}
static void editor_view(void) {
  float row[] = {0.49f, 0.17f, 0.17f, 0.17f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 4, row);
  nk_label(&ui, editor_path, NK_TEXT_LEFT);
  if (accent_button("Save"))
    save_requested = true;
  if (nk_button_label(&ui, "Save As")) {
    editor_save_as = !editor_save_as;
    copy(editor_copy_path, editor_path, sizeof(editor_copy_path));
  }
  if (nk_button_label(&ui, "New")) {
    editor_save_as = false;
    if (editor_dirty)
      editor_discard = true;
    else {
      editor[0] = 0;
      copy(editor_path, "/notes", sizeof(editor_path));
      editor_focus = 1;
    }
  }
  if (editor_save_as) {
    nk_layout_row_dynamic(&ui, 34, 2);
    nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, editor_copy_path,
                                   sizeof(editor_copy_path), nk_filter_default);
    if (nk_button_label(&ui, "Save copy")) {
      if (editor_copy_path[0] != '/')
        notify("Enter an absolute path for the copy");
      else if (fs_metadata(editor_copy_path, &(struct file_metadata){0}) != -2)
        notify("Choose an unused path for the copy");
      else if (fs_write(editor_copy_path, editor, strlen(editor)) < 0)
        notify("Could not save the copy");
      else {
        copy(editor_path, editor_copy_path, sizeof(editor_path));
        editor_dirty = editor_save_as = false;
        if (editor_draft_written && !editor_draft_available &&
            fs_remove(EDITOR_DRAFT_PATH) == 0)
          editor_draft_written = false;
        refresh_files();
        notify("Copy saved");
      }
    }
  }
  if (editor_draft_available) {
    nk_layout_row_dynamic(&ui, 30, 2);
    if (quiet_button("Recover draft", "Open the recovered document", !editor_dirty)) {
      copy(editor_path, editor_draft.path, sizeof(editor_path));
      copy(editor, editor_draft.text, sizeof(editor));
      editor_dirty = true;
      editor_draft_available = false;
    }
    if (nk_button_label(&ui, "Discard draft") &&
        fs_remove(EDITOR_DRAFT_PATH) == 0)
      editor_draft_available = editor_draft_written = false;
  }
  int selection = -1, selection_end = -1;
  nk_layout_row_dynamic(&ui, 30, 4);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, editor_find,
                                 sizeof(editor_find), nk_filter_default);
  if (quiet_button("Find next", "Select the next matching text", *editor_find)) {
    unsigned length = strlen(editor), query_length = strlen(editor_find);
    unsigned start = str_eq(editor_last_find, editor_find) &&
                             (unsigned)editor_match_end < length
                         ? (unsigned)editor_match_end : 0;
    for (unsigned i = 0; i < length; i++) {
      unsigned at = (start + i) % length;
      if (query_length <= length - at &&
          !__builtin_memcmp(editor + at, editor_find, query_length)) {
        selection = at;
        selection_end = editor_match_end = at + query_length;
        break;
      }
    }
    copy(editor_last_find, editor_find, sizeof(editor_last_find));
    if (selection < 0) notify("Text not found");
  }
  nk_property_int(&ui, "Line", 1, &editor_line, FS_MAX_FILE + 1, 1, 1);
  if (nk_button_label(&ui, "Go to line")) {
    int line = 1;
    unsigned at = 0;
    while (editor[at] && line < editor_line)
      if (editor[at++] == '\n') line++;
    if (line == editor_line) selection = selection_end = at;
    else notify("Line is outside the document");
  }
  float area = nk_window_get_content_region(&ui).h - 112
               - (editor_save_as ? 34 : 0) - (editor_draft_available ? 30 : 0);
  if (area < 70)
    area = 70;
  nk_layout_row_dynamic(&ui, area, 1);
  if (editor_focus || selection >= 0) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    editor_focus = 0;
  }
  if (selection >= 0) {
    struct nk_window *window = ui.current;
    window->edit.cursor = nk_utf_len(editor, selection_end);
    window->edit.sel_start = nk_utf_len(editor, selection);
    window->edit.sel_end = window->edit.cursor;
    unsigned row = 0;
    for (int i = 0; i < selection; i++)
      row += editor[i] == '\n';
    window->edit.scrollbar.y =
        (nk_uint)(row * (ui.style.font->height + ui.style.edit.row_padding));
  }
  uint32_t before = crc32(editor, strlen(editor));
  nk_edit_string_zero_terminated(&ui, NK_EDIT_BOX, editor, sizeof(editor),
                                 nk_filter_default);
  if (before != crc32(editor, strlen(editor)))
    editor_dirty = true;
  nk_layout_row_dynamic(&ui, 24, 2);
  caption(editor_dirty ? "Unsaved changes" : "All changes saved");
  label_value("", strlen(editor), " / 65536 bytes");
}
static void metric_card(const char *title, uint64_t value, const char *unit,
                        const char *detail, uint32_t color) {
  struct nk_rect bounds;
  if (!nk_widget(&bounds, &ui))
    return;
  struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
  struct nk_color background = rgb(palette()->mantle);
  nk_fill_rect(canvas, bounds, 10, background);
  nk_fill_rect(canvas, nk_rect(bounds.x, bounds.y + 12, 3, bounds.h - 24), 1,
               rgb(color));
  char text[128], digits[32];
  number(digits, value);
  copy(text, digits, sizeof(text));
  append(text, unit, sizeof(text));
  struct nk_rect line = nk_rect(bounds.x + 16, bounds.y + 8, bounds.w - 24, 20);
  nk_draw_text(canvas, line, title, strlen(title), &body_font, background,
               rgb(palette()->muted));
  line.y += 25;
  line.h = 28;
  nk_draw_text(canvas, line, text, strlen(text), &heading_font, background,
               rgb(palette()->text));
  line.y += 31;
  line.h = 20;
  nk_draw_text(canvas, line, detail, strlen(detail), &body_font, background,
               rgb(palette()->muted));
}
static void history_graph(const char *title, const unsigned samples[60],
                          uint32_t color) {
  nk_layout_row_dynamic(&ui, 24, 1);
  caption(title);
  nk_layout_row_dynamic(&ui, 100, 1);
  struct nk_rect bounds;
  if (!nk_widget(&bounds, &ui))
    return;
  struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
  nk_fill_rect(canvas, bounds, 8, rgb(palette()->mantle));
  for (unsigned i = 1; i < 4; i++)
    nk_stroke_line(canvas, bounds.x + 8, bounds.y + bounds.h * i / 4,
                   bounds.x + bounds.w - 8, bounds.y + bounds.h * i / 4, 1,
                   alpha_color(palette()->surface, 130));
  for (unsigned i = 1; i < sample_count; i++) {
    unsigned start = (sample_at + 60 - sample_count) % 60;
    unsigned a = samples[(start + i - 1) % 60], b = samples[(start + i) % 60];
    if (a > 100)
      a = 100;
    if (b > 100)
      b = 100;
    float x = bounds.x + 8 + (bounds.w - 16) * (60 - sample_count + i - 1) / 59;
    float next = bounds.x + 8 + (bounds.w - 16) * (60 - sample_count + i) / 59;
    nk_stroke_line(
        canvas, x, bounds.y + bounds.h - 8 - (bounds.h - 16) * a / 100, next,
        bounds.y + bounds.h - 8 - (bounds.h - 16) * b / 100, 2, rgb(color));
  }
}
static unsigned task_usage(int pid) {
  for (unsigned i = 0; i < 256; i++)
    if (task_cpu_pid[i] == (unsigned)pid)
      return task_cpu[i];
  return 0;
}
static void tasks_view(void) {
  heading("System Monitor");
  nk_layout_row_dynamic(&ui, 24, 1);
  caption(tasks_paused ? "Snapshot paused"
                       : "Live activity - updates every second");
  nk_layout_row_dynamic(&ui, 94, 3);
  metric_card("USERSPACE CPU", userspace_cpu, "%",
              "Single CPU / 100 Hz sampling", accent());
  metric_card("MEMORY", system_status.allocated_kib / 1024, " MiB",
              "Kernel + applications", 0x23ad95);
  metric_card("PROCESSES", task_count + 1, "", "Includes the desktop kernel",
              0x7287fd);

  const float tools[] = {0.22f, 0.18f, 0.20f, 0.25f, 0.15f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 5, tools);
  if (task_page == 2 ? accent_button("Applications")
                     : nk_button_label(&ui, "Applications"))
    task_page = 2;
  if (task_page == 0 ? accent_button("Processes")
                     : nk_button_label(&ui, "Processes"))
    task_page = 0;
  if (task_page == 1 ? accent_button("Performance")
                     : nk_button_label(&ui, "Performance"))
    task_page = 1;
  nk_checkbox_label(&ui, "Pause updates", &tasks_paused);
  if (nk_button_label(&ui, "Refresh")) {
    tasks_paused = false;
    update_status();
  }
  if (task_page == 2) {
    nk_layout_row_dynamic(&ui, 28, 1);
    caption("Open desktop windows, including minimized applications");
    float available = nk_window_get_content_region(&ui).h - 235;
    if (available < 100)
      available = 100;
    nk_layout_row_dynamic(&ui, available, 1);
    if (nk_group_begin(&ui, "Running applications", 0)) {
      for (unsigned i = 0; i < APP_COUNT; i++)
        if (apps[i].visible) {
          nk_layout_row_dynamic(&ui, 36, 4);
          nk_label(&ui, app_names[i], NK_TEXT_LEFT);
          caption(nk_window_is_collapsed(&ui, app_names[i]) ? "Minimized"
                                                            : "Open");
          if (nk_button_label(&ui, "Focus"))
            open_app(i);
          if (nk_button_label(&ui, "Close"))
            app_close_requested[i] = true;
        }
      for (unsigned i = 0; i < GUI_CLIENTS; i++) {
        struct gui_client *c = &gui_clients[i];
        if (!c->pixels || c->closing)
          continue;
        char name[96];
        client_name(i, name);
        nk_layout_row_dynamic(&ui, 36, 4);
        nk_label(&ui, c->title, NK_TEXT_LEFT);
        caption(nk_window_is_collapsed(&ui, name) ? "Minimized" : "Open");
        if (nk_button_label(&ui, "Focus")) {
          workspace_focus[client_workspace[i]] = APP_COUNT + i;
          workspace_request(client_workspace[i]);
          client_focus_request = i;
          nk_window_collapse(&ui, name, NK_MAXIMIZED);
          nk_window_set_focus(&ui, name);
          focused_client = i;
        }
        if (nk_button_label(&ui, "Close"))
          client_event(i, AV_GUI_QUIT, 0, 0, 0);
      }
      nk_group_end(&ui);
    }
    return;
  }
  if (task_page == 1) {
    float available = nk_window_get_content_region(&ui).h - 230;
    if (available < 100)
      available = 100;
    nk_layout_row_dynamic(&ui, available, 1);
    if (nk_group_begin(&ui, "Performance history", 0)) {
      history_graph("USERSPACE CPU - LAST 60 SAMPLES (0-100%)", cpu_samples,
                    accent());
      history_graph("PHYSICAL MEMORY - LAST 60 SAMPLES (0-100%)",
                    memory_samples, 0x23ad95);
      nk_layout_row_dynamic(&ui, 28, 2);
      label_value("Allocated: ", system_status.allocated_kib, " KiB");
      label_value("Managed: ", system_status.managed_kib, " KiB");
      nk_layout_row_dynamic(&ui, 28, 2);
      label_value("Uptime: ", system_status.ticks / 100, " seconds");
      label_value("Storage devices: ", system_status.disks, "");
      nk_layout_row_dynamic(&ui, 56, 1);
      nk_label_wrap(
          &ui, "CPU shows time spent running userspace programs. The remaining "
               "time includes kernel work and idle time. Built-in desktop apps "
               "share the kernel and are not separate processes.");
      nk_group_end(&ui);
    }
    return;
  }
  const float search[] = {0.15f, 0.85f};
  nk_layout_row(&ui, NK_DYNAMIC, 32, 2, search);
  caption("Filter");
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, task_filter,
                                 sizeof(task_filter), nk_filter_default);
  float area = nk_window_get_content_region(&ui).h - 354;
  if (area < 80)
    area = 80;
  nk_layout_row_dynamic(&ui, area, 1);
  if (nk_group_begin(&ui, "Process list", 0)) {
    const float columns[] = {0.34f, 0.09f, 0.18f, 0.11f, 0.17f, 0.11f};
    nk_layout_row(&ui, NK_DYNAMIC, 24, 6, columns);
    caption("APPLICATION");
    caption("PID");
    caption("STATE");
    caption("CPU");
    caption("MEMORY");
    caption("PRIO");
    unsigned shown = 0;
    if (contains("Magenta desktop / Avalanche kernel", task_filter)) {
      nk_layout_row(&ui, NK_DYNAMIC, 36, 6, columns);
      nk_bool selected = selected_pid == 0;
      if (nk_selectable_label(&ui, "Magenta desktop / kernel", NK_TEXT_LEFT,
                              &selected))
        selected_pid = 0;
      caption("0");
      caption("Kernel");
      caption("--");
      caption("Shared");
      caption("--");
      shown++;
    }
    const char *states[] = {"Empty",   "Ready",  "Running", "Sleeping", "Input",
                            "Waiting", "Exited", "Polling", "Futex"};
    for (unsigned i = 0; i < task_slots; i++) {
      struct task_info *t = &task_items[i];
      char pid[32];
      number(pid, t->pid);
      if (!contains(t->name, task_filter) && !contains(pid, task_filter))
        continue;
      shown++;
      nk_layout_row(&ui, NK_DYNAMIC, 36, 6, columns);
      nk_bool selected = selected_pid == t->pid;
      if (nk_selectable_label(&ui, t->name, NK_TEXT_LEFT, &selected)) {
        selected_pid = t->pid;
        task_priority = t->priority;
      }
      label_value("", t->pid, "");
      caption(t->state < sizeof(states) / sizeof(states[0]) ? states[t->state]
                                                            : "Unknown");
      label_value("", task_usage(t->pid), "%");
      char memory[64];
      format_size(memory, (uint64_t)t->pages * 4096);
      caption(memory);
      label_value("", t->priority, "");
    }
    if (!shown) {
      nk_layout_row_dynamic(&ui, 48, 1);
      nk_label(&ui, "No processes match this filter", NK_TEXT_CENTERED);
    }
    nk_group_end(&ui);
  }
  nk_layout_row_dynamic(&ui, 34, 3);
  if (selected_pid <= 0)
    nk_widget_disable_begin(&ui);
  nk_property_int(&ui, "Priority", 0, &task_priority, 31, 1, 0.3f);
  if (nk_button_label(&ui, "Apply Priority") && selected_pid > 0) {
    int r = user_priority(selected_pid, task_priority);
    notify(r < 0 ? "This process is no longer available" : "Priority updated");
    update_status();
  }
  if (nk_button_label(&ui, "End Process") && selected_pid > 0) {
    ending_pid = selected_pid;
    ending_task = true;
  }
  if (selected_pid <= 0)
    nk_widget_disable_end(&ui);
  nk_layout_row_dynamic(&ui, 30, 1);
  caption(selected_pid <= 0
              ? "The desktop kernel cannot be terminated here."
              : "Lower priority numbers receive earlier CPU service.");
}
static void ip_string(char out[32], uint32_t address) {
  out[0] = 0;
  for (unsigned i = 0; i < 4; i++) {
    char value[32];
    number(value, (address >> (24 - i * 8)) & 255);
    if (i)
      append(out, ".", 32);
    append(out, value, 32);
  }
}
static void bluetooth_view(void) {
  heading("Bluetooth LE");
  static const char *states[] = {
      "No compatible USB Bluetooth controller", "Ready", "Starting scan",
      "Scanning for advertisements", "Stopping scan", "Controller did not accept scanning"};
  unsigned state = usb_bluetooth_state();
  nk_layout_row_dynamic(&ui, 28, 1);
  caption(states[state < sizeof(states) / sizeof(states[0]) ? state : 0]);
  nk_layout_row_dynamic(&ui, 34, 2);
  if (quiet_button("Scan", "Listen for nearby Bluetooth LE advertisements",
                   state == 1 || state == 5))
    usb_bluetooth_scan(true);
  if (quiet_button("Stop scan", "Stop Bluetooth discovery", state == 2 || state == 3))
    usb_bluetooth_scan(false);
  struct bluetooth_peer peers[24];
  unsigned count = usb_bluetooth_peers(peers, 24);
  for (unsigned i = 0; i < count; i++) {
    const struct bluetooth_peer *peer = &peers[i];
    char address[18], value[32], signal[48] = "";
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned n = 0; n < 6; n++) {
      unsigned octet = peer->address[5 - n];
      address[n * 3] = hex[octet >> 4];
      address[n * 3 + 1] = hex[octet & 15];
      address[n * 3 + 2] = n == 5 ? 0 : ':';
    }
    nk_layout_row_dynamic(&ui, 26, 3);
    nk_label(&ui, peer->name[0] ? peer->name : "Unnamed device", NK_TEXT_LEFT);
    nk_label(&ui, address, NK_TEXT_LEFT);
    if (peer->rssi == 127)
      caption("Signal unavailable");
    else {
      if (peer->rssi < 0) copy(signal, "-", sizeof(signal));
      number(value, peer->rssi < 0 ? -peer->rssi : peer->rssi);
      append(signal, value, sizeof(signal));
      append(signal, " dBm", sizeof(signal));
      caption(signal);
    }
  }
}
static void network_view(void) {
  static bool showing_bluetooth;
  nk_layout_row_dynamic(&ui, 32, 2);
  if (nk_button_label(&ui, "Ethernet")) showing_bluetooth = false;
  if (nk_button_label(&ui, "Bluetooth LE")) showing_bluetooth = true;
  if (showing_bluetooth) {
    bluetooth_view();
    return;
  }
  uint8_t usb_mac[6];
  if (usb_ethernet_mac(usb_mac)) {
    nk_layout_row_dynamic(&ui, 32, 1);
    bool selected = net_usb_ethernet_selected();
    if (nk_button_label(&ui, selected ? "Use PCI Ethernet" : "Use USB Ethernet")) {
      net_use_usb_ethernet(!selected);
      update_status();
    }
  }
  nk_layout_row_dynamic(&ui, 28, 2);
  nk_label(&ui, network_status.adapter, NK_TEXT_LEFT);
  caption(network_status.ip ? "IPv4 configured" : "No IPv4 address");
  nk_layout_row_dynamic(&ui, 30, 3);
  label_value("Received: ", network_status.rx, "");
  label_value("Sent: ", network_status.tx, "");
  label_value("Dropped: ", network_status.drops, "");
  nk_layout_row_dynamic(&ui, 34, 3);
  if (accent_button("Connect with DHCP")) {
    int r = net_dhcp();
    notify(r < 0 ? "DHCP request failed" : "DHCP address assigned");
    update_status();
  }
  if (nk_button_label(&ui, "Static IPv4")) {
    static_address = !static_address;
    ip_string(ipv4_fields[0], network_status.ip);
    ip_string(ipv4_fields[1], network_status.mask);
    ip_string(ipv4_fields[2], network_status.gateway);
    ip_string(ipv4_fields[3], network_status.dns);
  }
  if (nk_button_label(&ui, "Refresh"))
    update_status();
  if (static_address) {
    const char *labels[] = {"Address", "Netmask", "Gateway", "DNS"};
    for (unsigned i = 0; i < 4; i++) {
      nk_layout_row_dynamic(&ui, 30, 2);
      caption(labels[i]);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, ipv4_fields[i], 32,
                                     nk_filter_default);
    }
    nk_layout_row_dynamic(&ui, 32, 1);
    if (accent_button("Apply IPv4 settings")) {
      uint32_t ip[4];
      bool valid = true;
      for (unsigned i = 0; i < 4; i++)
        if (!ip_parse(ipv4_fields[i], &ip[i]))
          valid = false;
      if (valid) {
        uint32_t inverse = ~ip[1];
        valid = ip[0] && ip[1] && !(inverse & (inverse + 1));
      }
      if (valid) {
        net_config(ip[0], ip[1], ip[2], ip[3]);
        notify("IPv4 settings applied");
        update_status();
      } else
        notify("Enter a valid address and contiguous netmask");
    }
  }
  nk_layout_row_dynamic(&ui, 26, 2);
  char value[32];
  ip_string(value, network_status.ip);
  caption("IPv4 address");
  nk_label(&ui, value, NK_TEXT_LEFT);
  nk_layout_row_dynamic(&ui, 34, 3);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, ping_target,
                                 sizeof(ping_target), nk_filter_default);
  if (nk_button_label(&ui, "Ping")) {
    uint32_t ip;
    int r = ip_parse(ping_target, &ip) ? 0 : net_dns(ping_target, &ip);
    if (r >= 0)
      r = net_ping(ip);
    if (r < 0)
      copy(network_result, "No reply received", sizeof(network_result));
    else {
      char time[32];
      number(time, r);
      copy(network_result, "Reply in ", sizeof(network_result));
      append(network_result, time, sizeof(network_result));
      append(network_result, " ms", sizeof(network_result));
    }
  }
  if (nk_button_label(&ui, "Resolve DNS")) {
    uint32_t ip;
    int r = net_dns(ping_target, &ip);
    if (r < 0)
      copy(network_result, "DNS lookup failed", sizeof(network_result));
    else
      ip_string(network_result, ip);
  }
  nk_layout_row_dynamic(&ui, 44, 1);
  nk_label_wrap(&ui, network_result);
}
static const char *package_summary(const char *name) {
  if (str_eq(name, "hello"))
    return "A small native application for inspecting arguments and uptime.";
  if (str_eq(name, "cat"))
    return "Read text files from persistent storage.";
  if (str_eq(name, "worker"))
    return "Run a CPU workload or a sleeping background process.";
  if (str_eq(name, "udp"))
    return "Send IPv4 datagrams using the kernel network stack.";
  if (str_eq(name, "http"))
    return "Fetch HTTP resources over a TCP connection.";
  return "An application installed from an HTTP package repository.";
}
static void packages_view(void) {
  if (*local_manifest) {
    heading("Install signed application");
    nk_layout_row_dynamic(&ui, 44, 1);
    nk_label_wrap(&ui, local_manifest);
    nk_layout_row_dynamic(&ui, 60, 1);
    nk_label_wrap(&ui, "Magenta verifies the manifest with the pinned signing "
                       "certificate, then verifies the executable hash.");
    nk_layout_row_dynamic(&ui, 36, 2);
    if (accent_button("Verify and install")) {
      int result = package_install_local(local_manifest);
      if (result < 0) {
        notify(result == -13 ? "Pin the publisher's certificate first"
               : result == -74 ? "Signature or executable hash did not match"
                               : "Could not install the signed application");
      } else {
        local_manifest[0] = 0;
        refresh_packages();
        refresh_files();
        notify("Application installed. Open it from the launcher.");
      }
    }
    if (nk_button_label(&ui, "Cancel")) local_manifest[0] = 0;
    return;
  }
  heading("Packages");
  nk_layout_row_dynamic(&ui, 30, 4);
  caption("UPLOADED APPLICATIONS");
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, upload_host,
                                 sizeof(upload_host), nk_filter_default);
  if (nk_button_label(&ui, "Trust uploaded signer")) {
    if (!network_status.ip)
      net_dhcp();
    char command[256] = "pkg trust-url ";
    append(command, upload_host, sizeof(command));
    append(command, " /latest/trust.der", sizeof(command));
    capture(command, package_result, sizeof(package_result));
  }
  if (nk_button_label(&ui, "Install uploaded app")) {
    if (!network_status.ip)
      net_dhcp();
    char command[256] = "pkg fetch ";
    append(command, upload_host, sizeof(command));
    append(command, " /latest/app.manifest", sizeof(command));
    capture(command, package_result, sizeof(package_result));
    refresh_packages();
    refresh_files();
  }
  unsigned installed_count = 0;
  for (unsigned i = 0; i < package_count; i++)
    if (package_items[i].installed)
      installed_count++;
  nk_layout_row_dynamic(&ui, 26, 2);
  label_value("In catalog: ", package_count, " packages");
  label_value("Installed: ", installed_count, "");
  const float toolbar[] = {0.12f, 0.43f, 0.28f, 0.17f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 4, toolbar);
  caption("Search");
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, package_filter,
                                 sizeof(package_filter), nk_filter_default);
  nk_checkbox_label(&ui, "Installed only", &installed_only);
  if (nk_button_label(&ui, "Refresh"))
    refresh_packages();
  float area =
      nk_window_get_content_region(&ui).h - (*package_result ? 300 : 258);
  if (area < 120)
    area = 120;
  const float panels[] = {0.44f, 0.56f};
  nk_layout_row(&ui, NK_DYNAMIC, area, 2, panels);
  if (nk_group_begin(&ui, "Package catalog", 0)) {
    unsigned shown = 0;
    for (unsigned i = 0; i < package_count; i++) {
      struct package_item *p = &package_items[i];
      if (!contains(p->name, package_filter) ||
          (installed_only && !p->installed))
        continue;
      shown++;
      const float row[] = {0.72f, 0.28f};
      nk_layout_row(&ui, NK_DYNAMIC, 38, 2, row);
      nk_bool selected = selected_package == (int)i;
      if (nk_selectable_label(&ui, p->name, NK_TEXT_LEFT, &selected)) {
        selected_package = i;
        char command[96] = "pkg query ";
        append(command, p->name, sizeof(command));
        capture(command, package_details, sizeof(package_details));
      }
      nk_label_colored(&ui, p->installed ? "Installed" : "Available",
                       NK_TEXT_RIGHT,
                       rgb(p->installed ? 0x23ad95 : palette()->muted));
      nk_layout_row_dynamic(&ui, 22, 1);
      caption(p->version);
    }
    if (!shown) {
      nk_layout_row_dynamic(&ui, 52, 1);
      nk_label_wrap(
          &ui, package_count
                   ? "No packages match your search."
                   : "The catalog is empty. Fetch a package manifest below.");
    }
    nk_group_end(&ui);
  }
  if (nk_group_begin(&ui, "Package information", 0)) {
    if (package_count && selected_package >= 0 &&
        selected_package < (int)package_count) {
      struct package_item *p = &package_items[selected_package];
      bool installed = p->installed;
      heading(p->name);
      nk_layout_row_dynamic(&ui, 24, 2);
      caption(p->version);
      nk_label_colored(&ui, installed ? "INSTALLED" : "AVAILABLE",
                       NK_TEXT_RIGHT, rgb(installed ? 0x23ad95 : accent()));
      nk_layout_row_dynamic(&ui, 64, 1);
      nk_label_wrap(&ui, package_summary(p->name));
      nk_layout_row_dynamic(&ui, 36, installed ? 3 : 1);
      if (installed && nk_button_label(&ui, "Launch") &&
          user_spawn(p->name, "", false) < 0)
        notify("Could not launch the application");
      if (accent_button(installed ? "Upgrade" : "Install"))
        package_action(installed ? "upgrade" : "install");
      if (installed && nk_button_label(&ui, "Remove"))
        removing_package = true;
      nk_layout_row_dynamic(&ui, 24, 1);
      caption("PACKAGE DETAILS");
      nk_layout_row_dynamic(&ui, 150, 1);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                                     package_details, sizeof(package_details),
                                     nk_filter_default);
    } else {
      heading("Your software, in one place.");
      nk_layout_row_dynamic(&ui, 88, 1);
      nk_label_wrap(&ui, "Fetch a manifest from an HTTP repository to add "
                         "packages to the catalog. Select a package to install "
                         "it or manage an existing installation.");
    }
    nk_group_end(&ui);
  }
  nk_layout_row_dynamic(&ui, 24, 1);
  caption("ADD FROM AN HTTP REPOSITORY");
  const float fields[] = {0.36f, 0.44f, 0.20f};
  nk_layout_row(&ui, NK_DYNAMIC, 22, 3, fields);
  caption("Host");
  caption("Manifest path");
  caption("");
  nk_layout_row(&ui, NK_DYNAMIC, 34, 3, fields);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, download_host,
                                 sizeof(download_host), nk_filter_default);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, download_path,
                                 sizeof(download_path), nk_filter_default);
  if (quiet_button("Fetch", "Fetch a package manifest from this repository",
                   true)) {
    if (!*download_host || *download_path != '/')
      notify("Enter a host and a manifest path beginning with /");
    else {
      char command[384] = "pkg fetch ";
      append(command, download_host, sizeof(command));
      append(command, " ", sizeof(command));
      append(command, download_path, sizeof(command));
      capture(command, package_result, sizeof(package_result));
      refresh_packages();
      refresh_files();
      notify(*package_result ? package_result : "Repository fetch completed");
    }
  }
  if (*package_result) {
    nk_layout_row_dynamic(&ui, 40, 1);
    nk_label_wrap(&ui, package_result);
  }
}
static void theme_card(unsigned index) {
  struct nk_rect r = nk_widget_bounds(&ui);
  bool selected = preferences.theme == index;
  if (nk_button_label(&ui, "")) {
    preferences.theme = index;
    preferences_dirty = theme_dirty = true;
  }
  struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
  struct nk_rect preview = nk_rect(r.x + 5, r.y + 5, r.w - 10, r.h - 30);
  nk_fill_rect(canvas, preview, 8, rgb(palettes[index].base));
  nk_fill_rect(canvas, nk_rect(preview.x + 8, preview.y + 8, preview.w - 16, 7),
               3, rgb(palettes[index].surface));
  nk_fill_rect(
      canvas,
      nk_rect(preview.x + 8, preview.y + 22, preview.w * 0.28f, preview.h - 30),
      4, rgb(palettes[index].mantle));
  nk_fill_rect(canvas,
               nk_rect(preview.x + preview.w * 0.40f, preview.y + 22,
                       preview.w * 0.45f, 7),
               3, rgb(index ? 0xb7bdf8 : accent()));
  nk_draw_text(canvas, nk_rect(r.x + 8, r.y + r.h - 23, r.w - 16, 20),
               palettes[index].name, strlen(palettes[index].name), &body_font,
               nk_rgba(0, 0, 0, 0), rgb(palette()->text));
  if (selected)
    nk_stroke_rect(canvas, nk_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 9, 2,
                   rgb(accent()));
}
static void appearance_view(void) {
  heading("Appearance");
  nk_layout_row_dynamic(&ui, 24, 1);
  caption("Choose a palette. Make it yours.");
  nk_layout_row_dynamic(&ui, 22, 1);
  nk_label(&ui, "Catppuccin themes", NK_TEXT_LEFT);
  nk_layout_row_dynamic(&ui, 100, 4);
  for (unsigned i = 0; i < 4; i++)
    theme_card(i);
  nk_layout_row_dynamic(&ui, 24, 1);
  nk_label(&ui, "Accent", NK_TEXT_LEFT);
  nk_layout_row_static(&ui, 34, 44, 5);
  for (unsigned i = 0; i < 5; i++) {
    struct nk_rect r = nk_widget_bounds(&ui);
    if (nk_button_color(&ui, rgb(accents[i]))) {
      preferences.accent = i;
      preferences_dirty = theme_dirty = true;
    }
    if (preferences.accent == i)
      nk_stroke_rect(nk_window_get_canvas(&ui),
                     nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 8, 2,
                     rgb(palette()->text));
  }
  nk_layout_row_dynamic(&ui, 24, 1);
  nk_label(&ui, "Desktop background", NK_TEXT_LEFT);
  const char *wallpapers[] = {"Ribbon", "Graphite", "Mountains"};
  nk_layout_row_dynamic(&ui, 32, 3);
  for (unsigned i = 0; i < 3; i++) {
    nk_bool chosen = preferences.wallpaper == i;
    if (nk_selectable_label(&ui, wallpapers[i], NK_TEXT_CENTERED, &chosen)) {
      preferences.wallpaper = i;
      preferences_dirty = theme_dirty = true;
    }
  }
  nk_layout_row_dynamic(&ui, 26, 2);
  caption("Glass opacity");
  int value = preferences.alpha;
  if (nk_slider_int(&ui, 195, &value, 255, 1)) {
    preferences.alpha = value;
    preferences_dirty = theme_dirty = true;
  }
  nk_layout_row_dynamic(&ui, 28, 1);
  nk_bool blurred = preferences.blur;
  if (nk_checkbox_label(&ui, "Blur translucent panels", &blurred)) {
    preferences.blur = blurred;
    preferences_dirty = true;
  }
  nk_layout_row_dynamic(&ui, 32, 2);
  if (accent_button("Save appearance"))
    preference_save(true);
  caption(preferences_dirty ? "Changes apply immediately"
          : preferences_saved
              ? (fs_mounted_disk() < 0 ? "Saved for this session"
                                       : "Saved on this disk")
              : "Default appearance");
}
static void workspace_view(void) {
  heading("Workspaces");
  nk_layout_row_dynamic(&ui, 32, 1);
  int count = workspaces.count;
  nk_property_int(&ui, "Workspaces", 1, &count, WORKSPACE_MAX, 1, 1);
  if ((unsigned)count != workspaces.count) workspace_count(count);
  for (unsigned i = 0; i < workspaces.count; i++) {
    nk_layout_row_dynamic(&ui, 32, 2);
    char old[32]; copy(old, workspaces.names[i], sizeof(old));
    nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, workspaces.names[i], 32, nk_filter_default);
    if (!str_eq(old, workspaces.names[i])) workspace_dirty = true;
    if (!workspaces.names[i][0]) copy(workspaces.names[i], old, 32);
    if (nk_button_label(&ui, workspaces.current == i ? "Current workspace" : "Switch")) workspace_request(i);
  }
  nk_layout_row_dynamic(&ui, 30, 1);
  caption("Ctrl+Alt+Left/Right switches; add Shift to move the focused window.");
  nk_layout_row_dynamic(&ui, 30, 1);
  caption("WINDOW ASSIGNMENTS");
  for (unsigned i = 0; i < APP_COUNT + GUI_CLIENTS; i++) {
    bool client = i >= APP_COUNT;
    unsigned slot = client ? i - APP_COUNT : i;
    if (client ? (!gui_clients[slot].pixels || gui_clients[slot].closing) : !apps[slot].visible) continue;
    unsigned home = client ? client_workspace[slot] : workspaces.homes[slot];
    nk_layout_row_dynamic(&ui, 32, 2);
    nk_label(&ui, client ? gui_clients[slot].title : app_names[slot], NK_TEXT_LEFT);
    if (nk_combo_begin_label(&ui, workspaces.names[home], nk_vec2(220, 280))) {
      nk_layout_row_dynamic(&ui, 28, 1);
      for (unsigned target = 0; target < workspaces.count; target++)
        if (nk_button_label(&ui, workspaces.names[target])) { workspace_move(i, target); nk_combo_close(&ui); break; }
      nk_combo_end(&ui);
    }
  }
  nk_layout_row_dynamic(&ui, 34, 1);
  if (accent_button("Save workspace settings")) workspace_save();
  nk_layout_row_dynamic(&ui, 30, 1);
  nk_bool restore = resume_desktop, documents = resume_editor;
  nk_checkbox_label(&ui, "Restore desktop after reboot", &restore);
  nk_checkbox_label(&ui, "Reopen the last saved text document", &documents);
  if (resume_desktop != (bool)restore || resume_editor != (bool)documents) {
    resume_desktop = restore;
    resume_editor = documents;
    desktop_session_save();
  }
  nk_layout_row_dynamic(&ui, 28, 1);
  caption(workspace_dirty ? "Changes waiting to save" : "Workspace settings saved");
}
static char *workspace_word(char **text) {
  while (**text == ' ') (*text)++;
  char *start = *text;
  while (**text && **text != ' ') (*text)++;
  if (**text) *(*text)++ = 0;
  return start;
}
static void workspace_command(char *line) {
  char *operation = workspace_word(&line);
  if (!*operation || str_eq(operation, "list")) {
    for (unsigned i = 0; i < workspaces.count; i++) {
      char digits[32]; number(digits, i + 1);
      terminal_append(digits); terminal_append(i == workspaces.current ? " * " : "   ");
      terminal_append(workspaces.names[i]); terminal_append("\n");
    }
    return;
  }
  if (str_eq(operation, "save")) { workspace_save(); return; }
  bool valid;
  unsigned index = parse_uint(workspace_word(&line), &valid);
  if (!valid) goto invalid;
  if (str_eq(operation, "count")) {
    if (!index || index > WORKSPACE_MAX) goto invalid;
    workspace_count(index); return;
  }
  if (str_eq(operation, "switch")) {
    if (!index || index > workspaces.count) goto invalid;
    workspace_request(index - 1); return;
  }
  if (str_eq(operation, "rename")) {
    while (*line == ' ') line++;
    if (!index || index > workspaces.count || !*line || strlen(line) >= 32) goto invalid;
    copy(workspaces.names[index - 1], line, 32); workspace_dirty = true; return;
  }
  if (str_eq(operation, "move") || str_eq(operation, "client")) {
    unsigned target = parse_uint(workspace_word(&line), &valid);
    if (!valid || !target || target > workspaces.count) goto invalid;
    if (str_eq(operation, "move")) {
      if (index >= APP_COUNT) goto invalid;
      workspace_move(index, target - 1); return;
    }
    for (unsigned i = 0; i < GUI_CLIENTS; i++)
      if (gui_clients[i].pixels && gui_clients[i].pid == (int)index) {
        workspace_move(APP_COUNT + i, target - 1); return;
      }
  }
invalid:
  terminal_append("workspace list|save|count N|switch N|rename N NAME\n"
                  "workspace move APP_INDEX N | workspace client PID N\n");
}
static void storage_health_view(void) {
  static int selected = -1, result;
  static struct nvme_health sample;
  static bool loaded;
  nk_layout_row_dynamic(&ui, 30, 1);
  caption("NVME HEALTH");
  bool found = false, refresh = false;
  for (unsigned i = 0; i < disk_count(); i++) {
    const struct block_device *disk = disk_get(i);
    if (!disk || disk->backend != 3) continue;
    found = true;
    if (selected < 0) { selected = i; refresh = true; }
    char label[96], index[32]; number(index, i);
    copy(label, "Disk ", sizeof(label)); append(label, index, sizeof(label));
    append(label, " - ", sizeof(label)); append(label, disk->model, sizeof(label));
    nk_layout_row_dynamic(&ui, 32, 1);
    nk_bool active = selected == (int)i;
    if (nk_selectable_label(&ui, label, NK_TEXT_LEFT, &active)) {
      selected = i; refresh = true;
    }
  }
  if (!found) { nk_label(&ui, "No NVMe namespace detected", NK_TEXT_LEFT); return; }
  nk_layout_row_dynamic(&ui, 34, 1);
  if (nk_button_label(&ui, "Refresh controller health")) refresh = true;
  const struct block_device *disk = disk_get(selected);
  if (!disk || disk->backend != 3) { selected = -1; loaded = false; return; }
  if (refresh || !loaded) {
    result = nvme_health_read(disk->unit, &sample, refresh);
    loaded = true;
  }
  if (result < 0) {
    nk_layout_row_dynamic(&ui, 30, 1);
    nk_label(&ui, result == -95 ? "Controller rejected the SMART/Health log request" :
                    result == -16 ? "Controller health query is busy; refresh to retry" :
                                    "Controller health query failed", NK_TEXT_LEFT);
    return;
  }
  nk_layout_row_dynamic(&ui, 28, 2);
  caption("Model"); nk_label(&ui, sample.model, NK_TEXT_LEFT);
  caption("Serial"); nk_label(&ui, sample.serial, NK_TEXT_LEFT);
  caption("Firmware"); nk_label(&ui, sample.firmware, NK_TEXT_LEFT);
  caption("Health"); nk_label(&ui, sample.critical_warning ? "Controller warning" : "No critical warnings", NK_TEXT_LEFT);
  static const char *warnings[] = {"Available spare is below threshold", "Temperature exceeds the controller threshold",
    "NVM reliability is degraded", "Media is read-only", "Volatile memory backup has failed", "Persistent memory region is read-only"};
  for (unsigned bit = 0; bit < 6; bit++) if (sample.critical_warning & (1u << bit)) {
    nk_layout_row_dynamic(&ui, 36, 1); nk_label_wrap(&ui, warnings[bit]);
  }
  nk_layout_row_dynamic(&ui, 28, 2);
  caption("Composite temperature");
  if (sample.temperature_kelvin) {
    char text[48], value[32]; int celsius = (int)sample.temperature_kelvin - 273;
    text[0] = 0; if (celsius < 0) copy(text, "-", sizeof(text));
    number(value, celsius < 0 ? -celsius : celsius); append(text, value, sizeof(text)); append(text, " C", sizeof(text));
    nk_label(&ui, text, NK_TEXT_LEFT);
  } else nk_label(&ui, "Not reported by controller", NK_TEXT_LEFT);
  caption("Available spare"); label_value("", sample.available_spare, "%");
  caption("Spare warning threshold"); label_value("", sample.spare_threshold, "%");
  caption("Endurance used"); label_value("", sample.percentage_used, "%");
  caption("Namespace ID"); label_value("", sample.namespace_id, "");
  caption("Logical block bytes"); label_value("", sample.logical_block_size, "");
  uint8_t blocks[16] = {0}; char value[40];
  for (unsigned i = 0; i < 8; i++) blocks[i] = sample.namespace_blocks >> (8 * i);
  nvme_counter_format(value, sizeof(value), blocks);
  caption("Logical blocks"); nk_label(&ui, value, NK_TEXT_LEFT);
  static const char *labels[NVME_HEALTH_COUNTERS] = {"Data units read", "Data units written", "Host reads", "Host writes", "Controller busy minutes",
    "Power cycles", "Power-on hours", "Unsafe shutdowns", "Media integrity errors", "Error log entries"};
  for (unsigned i = 0; i < NVME_HEALTH_COUNTERS; i++) {
    nvme_counter_format(value, sizeof(value), sample.counters[i]);
    caption(labels[i]); nk_label(&ui, value, NK_TEXT_LEFT);
  }
  caption("Warning temperature minutes"); label_value("", sample.warning_temperature_minutes, "");
  caption("Critical temperature minutes"); label_value("", sample.critical_temperature_minutes, "");
  for (unsigned i = 0; i < 8; i++) if (sample.temperature_sensors[i]) {
    char sensor[48], n[32]; number(n, i + 1);
    copy(sensor, "Temperature sensor ", sizeof(sensor)); append(sensor, n, sizeof(sensor));
    caption(sensor); label_value("", sample.temperature_sensors[i], " K");
  }
  for (unsigned i = 0; i < 2; i++) {
    caption(i ? "Thermal level 2 transitions" : "Thermal level 1 transitions");
    label_value("", sample.thermal_transitions[i], "");
    caption(i ? "Thermal level 2 seconds" : "Thermal level 1 seconds");
    label_value("", sample.thermal_seconds[i], "");
  }
  caption("Sample age"); label_value("", (clock_ticks() - sample.sampled_ticks) / 100, " seconds");
  nk_layout_row_dynamic(&ui, 48, 1);
  nk_label_wrap(&ui, "Read/write data units are 512,000 bytes, rounded up by the controller. Endurance used may exceed 100%.");
}
static void settings_view(void) {
  float columns[] = {0.25f, 0.75f};
  float area = nk_window_get_content_region(&ui).h;
  nk_layout_row(&ui, NK_DYNAMIC, area, 2, columns);
  if (nk_group_begin(&ui, "Settings navigation", 0)) {
    nk_layout_row_dynamic(&ui, 28, 1);
    caption("MAGENTA");
    const char *pages[] = {"Appearance", "System", "Storage",
                           "Pointer",    "About",  "Registry", "Default apps", "Clipboard", "Workspaces"};
    for (unsigned i = 0; i < 9; i++) {
      nk_layout_row_dynamic(&ui, 38, 1);
      nk_bool selected = settings_page == (int)i;
      if (nk_selectable_label(&ui, pages[i], NK_TEXT_LEFT, &selected))
        settings_page = i;
    }
    nk_layout_row_dynamic(&ui, 40, 1);
    caption("Local session");
    nk_layout_row_dynamic(&ui, 28, 1);
    if (nk_button_label(&ui, "Applications"))
      launcher = true;
    nk_group_end(&ui);
  }
  if (nk_group_begin(&ui, "Settings content", 0)) {
    if (settings_page == 0)
      appearance_view();
    else if (settings_page == 1) {
      heading("System");
      nk_layout_row_dynamic(&ui, 28, 1);
      nk_label(&ui, "Avalanche Kernel 0.4.0", NK_TEXT_LEFT);
      caption("Native x86-64 / Magenta desktop");
      label_value("Uptime: ", system_status.ticks / 100, " seconds");
      label_value("Managed memory: ", system_status.managed_kib, " KiB");
      label_value("Allocated memory: ", system_status.allocated_kib, " KiB");
      label_value("Running processes: ", task_count, "");
      nk_layout_row_dynamic(&ui, 34, 1);
      if (nk_button_label(&ui, "Open Task Manager"))
        open_app(TASKS);
    } else if (settings_page == 2) {
      heading("Storage");
      static char details[1024];
      capture("fs", details, sizeof(details));
      nk_layout_row_dynamic(&ui, 130, 1);
      nk_label_wrap(&ui, details);
      nk_layout_row_dynamic(&ui, 34, 2);
      if (nk_button_label(&ui, "Open Files")) {
        browse("/");
        open_app(FILES);
      }
      if (nk_button_label(&ui, "Flush writes")) {
        int disk = fs_mounted_disk();
        notify(disk >= 0 && disk_flush(disk)
                   ? "Storage synchronized"
                   : "No writable disk to synchronize");
      }
      nk_layout_row_dynamic(&ui, 28, 1);
      label_value("Detected disks: ", system_status.disks, "");
      storage_health_view();
    } else if (settings_page == 3) {
      heading("Pointer");
      nk_layout_row_dynamic(&ui, 50, 1);
      nk_label_wrap(
          &ui,
          mouse_absolute()
              ? "Absolute pointer input maps taps directly to the desktop. "
                "Pointer sensitivity applies to physical PS/2 mice."
              : "Adjust the PS/2 pointer speed. The pointer remains inside "
                "the desktop while moving and dragging windows.");
      nk_layout_row_dynamic(&ui, 34, 1);
      int speed = preferences.pointer_speed;
      if (nk_slider_int(&ui, 25, &speed, 250, 5)) {
        preferences.pointer_speed = speed;
        mouse_configure(display_width, display_height, speed);
        preferences_dirty = true;
      }
      nk_layout_row_dynamic(&ui, 28, 1);
      label_value("Sensitivity: ", preferences.pointer_speed, "%");
      nk_layout_row_dynamic(&ui, 34, 1);
      if (nk_button_label(&ui, "Reset pointer speed")) {
        preferences.pointer_speed = 100;
        mouse_configure(display_width, display_height, 100);
        preferences_dirty = true;
      }
    } else if (settings_page == 8) {
      workspace_view();
    } else if (settings_page == 6) {
      heading("Default applications");
      nk_layout_row_dynamic(&ui, 38, 1);
      caption("Choose how Files opens text and web documents.");
      for (unsigned type = FILE_TEXT; type <= FILE_WAV; type++) {
        nk_layout_row_dynamic(&ui, 32, 2);
        nk_label(&ui, file_type_mime(type), NK_TEXT_LEFT);
        if (type == FILE_TEXT || type == FILE_HTML) {
          const char *options[] = {"Text editor", "Browser"};
          int selected = default_handler(type) == BROWSER;
          int choice = nk_combo(&ui, options, 2, selected, 30, nk_vec2(220, 100));
          file_defaults.handlers[type] = choice ? BROWSER : EDITOR;
        } else nk_label(&ui, app_names[default_handler(type)], NK_TEXT_LEFT);
      }
      nk_layout_row_dynamic(&ui, 36, 2);
      if (accent_button("Save defaults")) {
        file_defaults.checksum = crc32(&file_defaults, offsetof(__typeof__(file_defaults), checksum));
        file_defaults_saved = fs_write("magenta-filetypes", &file_defaults, sizeof(file_defaults)) >= 0;
      }
      if (nk_button_label(&ui, "Reset defaults")) { defaults_reset(); file_defaults_saved = false; }
      nk_layout_row_dynamic(&ui, 30, 1);
      caption(file_defaults_saved ? "Saved on disk" : "Save to keep these defaults after reboot");
    } else if (settings_page == 7) {
      heading("Clipboard");
      nk_layout_row_dynamic(&ui, 50, 1);
      nk_label_wrap(&ui, "Text copied in the desktop is shared by its text fields, editor and terminal, and restored after reboot.");
      nk_layout_row_dynamic(&ui, 160, 1);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_BOX | NK_EDIT_READ_ONLY, clipboard, sizeof(clipboard), nk_filter_default);
      nk_layout_row_dynamic(&ui, 36, 2);
      if (nk_button_label(&ui, "Clear clipboard")) {
        clipboard[0] = 0;
        clipboard_dirty = true;
        clipboard_save();
      }
      if (nk_button_label(&ui, "Save clipboard")) clipboard_save();
      nk_layout_row_dynamic(&ui, 30, 1);
      caption(clipboard_dirty ? "Waiting to save to disk" : clipboard_saved ? "Saved on disk" : "Clipboard is empty");
    } else if (settings_page == 5) {
      heading("Registry editor");
      nk_layout_row_dynamic(&ui, 28, 1);
      caption("Desktop keys are saved to the disk when edited.");
      static const char *keys[] = {"desktop.shortcuts",  "desktop.top_bar",
                                   "desktop.dock",       "desktop.icons",
                                   "desktop.icons_x",    "desktop.icons_y",
                                   "desktop.dock_bottom", "workspace.count", "workspace.current"};
      for (unsigned i = 0; i < 9; i++) {
        char label[96], value[32];
        number(value, *registry_slot(keys[i]));
        copy(label, keys[i], sizeof(label));
        append(label, "  =  ", sizeof(label));
        append(label, value, sizeof(label));
        nk_layout_row_dynamic(&ui, 27, 1);
        nk_bool selected = str_eq(registry_key, keys[i]);
        if (nk_selectable_label(&ui, label, NK_TEXT_LEFT, &selected)) {
          copy(registry_key, keys[i], sizeof(registry_key));
          number(registry_value, *registry_slot(keys[i]));
        }
      }
      nk_layout_row_dynamic(&ui, 30, 1);
      caption("Set a key and decimal value. Boolean keys use 0 or 1.");
      nk_layout_row_dynamic(&ui, 35, 2);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, registry_key,
                                     sizeof(registry_key), nk_filter_default);
      nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, registry_value,
                                     sizeof(registry_value), nk_filter_decimal);
      nk_layout_row_dynamic(&ui, 36, 1);
      if (accent_button("Save registry value"))
        copy(registry_status,
             registry_set(registry_key, registry_value)
                 ? "Saved to disk. Terminal commands now read this value."
                 : "Save failed: check the key, value, and writable disk.",
             sizeof(registry_status));
      nk_layout_row_dynamic(&ui, 36, 1);
      nk_label_wrap(&ui, registry_status);
    } else {
      heading("About Magenta");
      nk_layout_row_dynamic(&ui, 54, 1);
      nk_label_wrap(&ui,
                    "A native x86-64 desktop powered by Avalanche Kernel.");
      nk_layout_row_dynamic(&ui, 28, 1);
      caption("Interface toolkit");
      nk_label(&ui, "Nuklear / MIT", NK_TEXT_LEFT);
      caption("Theme system");
      nk_label(&ui, "Catppuccin / MIT", NK_TEXT_LEFT);
      caption("Typography");
      nk_label(&ui, "DejaVu Sans / TrueType", NK_TEXT_LEFT);
      nk_layout_row_dynamic(&ui, 60, 1);
      nk_label_wrap(&ui, "The settings layout is inspired by COSMIC. Magenta's "
                         "controls run directly on the kernel framebuffer.");
    }
    nk_group_end(&ui);
  }
}
static void maximize(unsigned id) {
  if (id >= APP_COUNT)
    return;
  if (!apps[id].maximized) {
    apps[id].restore = apps[id].bounds;
    apps[id].bounds = nk_rect(16, 68, width - 32, height - 164);
  } else
    apps[id].bounds = apps[id].restore;
  apps[id].maximized = !apps[id].maximized;
  nk_window_set_bounds(&ui, app_names[id], apps[id].bounds);
}
static struct nk_rect window_control_bounds(unsigned id, unsigned control) {
  struct nk_rect r =
      id < APP_COUNT ? apps[id].bounds : client_bounds[id - APP_COUNT];
  return nk_rect(r.x + r.w - 144 + control * 44, r.y + 6, 40, 44);
}
static int window_controls(unsigned id) {
  struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
  struct nk_rect clip = canvas->clip;
  nk_push_scissor(canvas, nk_rect(0, 0, width, height));
  char client_title[96];
  const char *title, *summary, *name;
  bool maximized;
  unsigned icon;
  struct nk_rect bounds;
  if (id < APP_COUNT) {
    bounds = apps[id].bounds;
    title = name = app_names[id];
    summary = app_summaries[id];
    maximized = apps[id].maximized;
    icon = id == BROWSER  ? 4
           : id == IMAGES ? 9
           : id == MUSIC  ? 10
           : id == CALCULATOR ? 12
           : id == GAMES  ? 11
                          : id;
  } else {
    unsigned i = id - APP_COUNT;
    bounds = client_bounds[i];
    client_name(i, client_title);
    name = client_title;
    title = gui_clients[i].title;
    summary = "Magenta application";
    maximized = client_maximized[i];
    icon = 4;
  }
  bool active = nk_window_is_active(&ui, name);
  nk_fill_rect(canvas, nk_rect(bounds.x + 1, bounds.y + 1, bounds.w - 2, 55), 9,
               rgb(palette()->mantle));
  gui_canvas_gloss(canvas,
                   nk_rect(bounds.x + 1, bounds.y + 1, bounds.w - 2, 55), 12);
  gui_canvas_icon(canvas, nk_rect(bounds.x + 16, bounds.y + 13, 28, 28), icon,
                  active ? rgb(accent()) : rgb(palette()->muted));
  nk_draw_text(
      canvas, nk_rect(bounds.x + 56, bounds.y + 10, bounds.w - 216, 20), title,
      strlen(title), &body_font, nk_rgba(0, 0, 0, 0), rgb(palette()->text));
  nk_draw_text(canvas,
               nk_rect(bounds.x + 56, bounds.y + 30, bounds.w - 216, 18),
               summary, strlen(summary), &body_font, nk_rgba(0, 0, 0, 0),
               rgb(palette()->muted));
  nk_stroke_line(canvas, bounds.x + 16, bounds.y + 55, bounds.x + bounds.w - 16,
                 bounds.y + 55, 1, alpha_color(palette()->surface, 120));
  int action = -1;
  for (unsigned control = 0; control < 3; control++) {
    struct nk_rect r = window_control_bounds(id, control);
    bool hover = pointer_targets_window(name) &&
                 nk_input_is_mouse_hovering_rect(&ui.input, r);
    if (hover && (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) ||
                  nk_input_is_mouse_released(&ui.input, NK_BUTTON_LEFT))) {
      held_control_app = id;
      held_control = control;
    }
    bool held = held_control_app == (int)id && held_control == (int)control;
    if (held && nk_input_is_mouse_released(&ui.input, NK_BUTTON_LEFT)) {
      if (hover)
        action = control;
      held_control_app = held_control = -1;
    }
    struct nk_color fill =
        hover ? alpha_color(control == 2 ? 0xe54868 : palette()->surface,
                            held ? 255 : 180)
              : nk_rgba(0, 0, 0, 0);
    struct nk_color ink =
        hover && control == 2 ? nk_rgb(255, 255, 255) : rgb(palette()->muted);
    nk_fill_rect(canvas, r, 8, fill);
    float x = r.x + 14, y = r.y + 16;
    if (control == 0)
      nk_stroke_line(canvas, x, y + 8, x + 12, y + 8, 2, ink);
    else if (control == 1) {
      nk_stroke_rect(canvas, nk_rect(x, y, 12, 12), 1, 2, ink);
      if (maximized)
        nk_stroke_rect(canvas, nk_rect(x + 3, y - 3, 12, 12), 1, 1, ink);
    } else {
      nk_stroke_line(canvas, x, y, x + 12, y + 12, 2, ink);
      nk_stroke_line(canvas, x + 12, y, x, y + 12, 2, ink);
    }
  }
  nk_push_scissor(canvas, clip);
  return action;
}
struct browser_tab {
  bool used, load_requested, history_navigation, reset_scroll;
  char address[512], loaded_url[512], status[128], title[64];
  char source[65536], history[16][512];
  unsigned history_count, history_at;
};
static struct browser_tab browser_tabs[WEB_TABS];
static unsigned browser_order[WEB_TABS], browser_tab_count, browser_active;
static struct nk_user_font web_fonts[WEB_NODES];
static int browser_focus;
#define browser_url (browser_tabs[browser_active].address)
#define browser_status (browser_tabs[browser_active].status)
#define browser_source (browser_tabs[browser_active].source)
#define browser_load_requested (browser_tabs[browser_active].load_requested)
static void browser_group_name(char name[32], unsigned tab) {
  char digits[32];
  number(digits, tab);
  copy(name, "Web document ", 32);
  append(name, digits, 32);
}
static void browser_switch(unsigned tab) {
  if (tab >= WEB_TABS || !browser_tabs[tab].used)
    return;
  browser_active = tab;
  web_select(tab);
  browser_focus = 0;
}
static void browser_new_tab(const char *address) {
  if (browser_tab_count == WEB_TABS) {
    notify("Eight tabs are open. Close a tab to open another.");
    return;
  }
  unsigned slot = 0;
  while (slot < WEB_TABS && browser_tabs[slot].used)
    slot++;
  memset(&browser_tabs[slot], 0, sizeof(browser_tabs[slot]));
  web_reset(slot);
  browser_tabs[slot].used = true;
  browser_tabs[slot].load_requested = true;
  browser_tabs[slot].reset_scroll = true;
  copy(browser_tabs[slot].address, address, sizeof(browser_tabs[slot].address));
  copy(browser_tabs[slot].title, "New tab", sizeof(browser_tabs[slot].title));
  copy(browser_tabs[slot].status, "Ready", sizeof(browser_tabs[slot].status));
  browser_order[browser_tab_count++] = slot;
  browser_switch(slot);
  browser_focus = 1;
}
static void browser_close_tab(unsigned tab) {
  unsigned at = 0;
  while (at < browser_tab_count && browser_order[at] != tab)
    at++;
  if (at == browser_tab_count)
    return;
  bool was_active = browser_active == tab;
  web_reset(tab);
  memset(&browser_tabs[tab], 0, sizeof(browser_tabs[tab]));
  for (unsigned i = at; i + 1 < browser_tab_count; i++)
    browser_order[i] = browser_order[i + 1];
  browser_tab_count--;
  if (!browser_tab_count) {
    browser_new_tab("about:home");
    return;
  }
  if (was_active)
    browser_switch(browser_order[at < browser_tab_count ? at : at - 1]);
}
#define SESSION_PATH "/.magenta-session"
struct session_window {
  int x, y, w, h;
  unsigned visible, maximized;
};
static struct desktop_session {
  uint32_t version, enabled, reopen_document, tabs, selected_tab;
  char files[256], document[256];
  char addresses[WEB_TABS][512];
  struct session_window windows[APP_COUNT];
  uint32_t checksum;
} session;
static uint32_t session_checksum, session_last_save;
static void desktop_session_save(void) {
  memset(&session, 0, sizeof(session));
  session.version = 1;
  session.enabled = resume_desktop;
  session.reopen_document = resume_editor;
  if (resume_desktop) {
    copy(session.files, browser_path, sizeof(session.files));
    if (resume_editor && !editor_dirty && apps[EDITOR].visible)
      copy(session.document, editor_path, sizeof(session.document));
    for (unsigned i = 0; i < APP_COUNT; i++) {
      struct nk_rect bounds = apps[i].maximized ? apps[i].restore : apps[i].bounds;
      session.windows[i] = (struct session_window){
          bounds.x, bounds.y, bounds.w, bounds.h, apps[i].visible, apps[i].maximized};
    }
    session.tabs = browser_tab_count;
    for (unsigned i = 0; i < browser_tab_count; i++) {
      unsigned slot = browser_order[i];
      struct browser_tab *tab = &browser_tabs[slot];
      copy(session.addresses[i], *tab->loaded_url ? tab->loaded_url : tab->address,
           sizeof(session.addresses[i]));
      if (slot == browser_active) session.selected_tab = i;
    }
  }
  session.checksum = crc32(&session, offsetof(struct desktop_session, checksum));
  session_last_save = clock_ticks();
  if (session.checksum != session_checksum &&
      fs_write(SESSION_PATH, &session, sizeof(session)) >= 0)
    session_checksum = session.checksum;
}
static void desktop_session_load(void) {
  resume_desktop = resume_editor = false;
  session_checksum = 0;
  session_last_save = clock_ticks();
  if (fs_read(SESSION_PATH, &session, sizeof(session)) != (int)sizeof(session) ||
      session.version != 1 ||
      session.checksum != crc32(&session, offsetof(struct desktop_session, checksum)))
    return;
  session_checksum = session.checksum;
  resume_desktop = session.enabled == 1;
  resume_editor = session.reopen_document == 1;
  if (!resume_desktop) return;
  for (unsigned i = 0; i < APP_COUNT; i++) {
    const struct session_window *saved = &session.windows[i];
    if (saved->w < 160 || saved->h < 100) continue;
    int w = saved->w < (int)width ? saved->w : (int)width;
    int h = saved->h < (int)height - 42 ? saved->h : (int)height - 42;
    int x = saved->x >= 0 && saved->x <= (int)width - w ? saved->x : 0;
    int y = saved->y >= 36 && saved->y <= (int)height - h ? saved->y : 36;
    apps[i].bounds = nk_rect(x, y, w, h);
    apps[i].visible = saved->visible == 1 && i != EDITOR;
    if (saved->maximized == 1) {
      apps[i].restore = apps[i].bounds;
      apps[i].bounds = nk_rect(8, 42, width - 16, height - 100);
      apps[i].maximized = true;
    }
  }
  if (session.files[0] == '/' && !session.files[255] &&
      fs_directory(session.files) == 0)
    browse(session.files);
  if (session.tabs && session.tabs <= WEB_TABS &&
      session.selected_tab < session.tabs) {
    memset(browser_tabs, 0, sizeof(browser_tabs));
    browser_tab_count = 0;
    for (unsigned i = 0; i < session.tabs; i++) {
      const char *address = session.addresses[i];
      browser_new_tab(address[0] && !address[511] ? address : "about:home");
    }
    browser_switch(browser_order[session.selected_tab]);
  }
  if (resume_editor && !editor_draft_available &&
      session.document[0] == '/' && !session.document[255]) {
    int bytes = fs_read(session.document, editor, sizeof(editor) - 1);
    if (bytes >= 0 && !__builtin_memchr(editor, 0, bytes)) {
      editor[bytes] = 0;
      copy(editor_path, session.document, sizeof(editor_path));
      apps[EDITOR].visible = true;
    } else
      editor[0] = 0;
  }
}
static void browser_title(void) {
  struct browser_tab *tab = &browser_tabs[browser_active];
  const char *label = tab->loaded_url;
  if (starts_with(label, "https://"))
    label += 8;
  else if (starts_with(label, "http://"))
    label += 7;
  else if (starts_with(label, "file:"))
    label += 5;
  copy(tab->title, str_eq(tab->loaded_url, "about:home") ? "New tab" : label,
       sizeof(tab->title));
  const char *p = tab->source;
  while (*p) {
    if (starts_with(p, "<title>")) {
      p += 7;
      unsigned n = 0;
      while (*p && *p != '<' && n + 1 < sizeof(tab->title))
        tab->title[n++] = *p++;
      tab->title[n] = 0;
      break;
    }
    p++;
  }
}
static void browser_history_go(int direction) {
  struct browser_tab *tab = &browser_tabs[browser_active];
  if ((direction < 0 && !tab->history_at) ||
      (direction > 0 && tab->history_at + 1 >= tab->history_count))
    return;
  tab->history_at = direction < 0 ? tab->history_at - 1 : tab->history_at + 1;
  copy(browser_url, tab->history[tab->history_at], sizeof(browser_url));
  tab->history_navigation = true;
  browser_load_requested = true;
}
static void browser_load(void) {
  struct browser_tab *tab = &browser_tabs[browser_active];
  browser_load_requested = false;
  web_select(browser_active);
  browser_source[0] = 0;
  web_count = 0;
  web_message[0] = 0;
  /* Keep committed location separate from address-field edits. */
  if (!*browser_url)
    copy(browser_url, "about:home", sizeof(browser_url));
  else if (!contains(browser_url, ":")) {
    char address[512] = "https://";
    if (strlen(browser_url) + 9 >= sizeof(address)) {
      copy(browser_status, "The address is too long", sizeof(browser_status));
      return;
    }
    append(address, browser_url, sizeof(address));
    copy(browser_url, address, sizeof(browser_url));
  }
  copy(tab->loaded_url, browser_url, sizeof(tab->loaded_url));
  if (!tab->history_navigation &&
      (!tab->history_count ||
       !str_eq(tab->history[tab->history_at], browser_url))) {
    tab->history_count = tab->history_count ? tab->history_at + 1 : 0;
    if (tab->history_count == 16) {
      memmove(tab->history, tab->history + 1, 15 * sizeof(tab->history[0]));
      tab->history_count--;
    }
    tab->history_at = tab->history_count++;
    copy(tab->history[tab->history_at], browser_url, sizeof(tab->history[0]));
  }
  tab->history_navigation = false;
  tab->reset_scroll = true;
  if (str_eq(browser_url, "about:home")) {
    copy(browser_status, "New tab", sizeof(browser_status));
    copy(tab->title, "New tab", sizeof(tab->title));
    web_reset(browser_active);
    return;
  }
  if (starts_with(browser_url, "file:")) {
    int n =
        fs_read(browser_url + 5, browser_source, sizeof(browser_source) - 1);
    if (n < 0 && str_eq(browser_url, "file:/webnotes")) {
      extern const char webnotes_start[], webnotes_end[];
      size_t size = webnotes_end - webnotes_start;
      if (size < sizeof(browser_source)) {
        memcpy(browser_source, webnotes_start, size);
        n = size;
      }
    }
    if (n < 0) {
      copy(browser_status, "Could not open the local document",
           sizeof(browser_status));
      browser_title();
      return;
    }
    browser_source[n] = 0;
  } else {
    int result = http_get(browser_url, browser_source, sizeof(browser_source));
    if (result < 0) {
      copy(browser_status,
           result == -3 ? "TLS verification failed: check the address, clock "
                          "and certificates"
           : result == -4
               ? "Page exceeds the document limit or the connection ended early"
               : "Could not connect to this address",
           sizeof(browser_status));
      browser_source[0] = 0;
      browser_title();
      return;
    }
  }
  web_set_url(tab->loaded_url);
  web_parse(browser_source);
  browser_title();
  copy(browser_status,
       starts_with(tab->loaded_url, "https://") ? "HTTPS - certificate verified"
       : starts_with(tab->loaded_url, "file:")
           ? "Local document"
           : "HTTP - unencrypted connection",
       sizeof(browser_status));
}
static void browser_view(void) {
  if (!browser_tab_count)
    browser_new_tab("about:home");
  web_select(browser_active);
  /* Leave the tab strip in one row; labels shrink before navigation controls.
   */
  nk_layout_row_begin(&ui, NK_DYNAMIC, 34, browser_tab_count * 2 + 1);
  int switch_to = -1, close_tab = -1;
  bool add_tab = false;
  float tab_share = 0.90f / browser_tab_count;
  for (unsigned i = 0; i < browser_tab_count; i++) {
    unsigned slot = browser_order[i];
    nk_layout_row_push(&ui, tab_share * 0.78f);
    bool chosen = browser_active == slot;
    bool hovered = nk_widget_is_hovered(&ui);
    if (chosen ? accent_button(browser_tabs[slot].title)
               : nk_button_label(&ui, browser_tabs[slot].title))
      switch_to = slot;
    if (hovered)
      nk_tooltip(&ui, browser_tabs[slot].loaded_url);
    nk_layout_row_push(&ui, tab_share * 0.22f);
    nk_style_push_vec2(&ui, &ui.style.button.padding, nk_vec2(2, 6));
    if (nk_button_label(&ui, "x"))
      close_tab = slot;
    nk_style_pop_vec2(&ui);
  }
  nk_layout_row_push(&ui, 0.10f);
  add_tab = quiet_button("+", "New tab (Ctrl+T)", browser_tab_count < WEB_TABS);
  nk_layout_row_end(&ui);
  if (switch_to >= 0)
    browser_switch(switch_to);
  if (close_tab >= 0)
    browser_close_tab(close_tab);
  if (add_tab)
    browser_new_tab("about:home");

  const float navigation[] = {0.08f, 0.08f, 0.13f, 0.12f, 0.39f, 0.20f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 6, navigation);
  struct browser_tab *tab = &browser_tabs[browser_active];
  if (quiet_button("<", "Back", tab->history_count && tab->history_at > 0))
    browser_history_go(-1);
  if (quiet_button(">", "Forward", tab->history_at + 1 < tab->history_count))
    browser_history_go(1);
  if (quiet_button("Reload", "Reload this tab (Ctrl+R)", true)) {
    copy(browser_url, tab->loaded_url, sizeof(browser_url));
    browser_load_requested = true;
  }
  if (quiet_button("Home", "Open the start page", true)) {
    copy(browser_url, "about:home", sizeof(browser_url));
    browser_load_requested = true;
  }
  caption(browser_status);
  if (quiet_button("Save Page", "Send this tab's HTML to Downloads",
                   *browser_source != 0)) {
    unsigned size = strlen(browser_source);
    struct desktop_download *item = download_begin(0, "download.html", size);
    if (item) {
      memcpy(item->data, browser_source, size);
      item->received = size;
      item->state = DL_READY;
    } else notify("Dismiss an existing download to make room");
  }

  const float address[] = {0.85f, 0.15f};
  nk_layout_row(&ui, NK_DYNAMIC, 38, 2, address);
  if (browser_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    browser_focus = 0;
  }
  nk_flags entered = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_AUTO_SELECT, browser_url,
      sizeof(browser_url), nk_filter_default);
  if (accent_button("Go") || (entered & NK_EDIT_COMMITTED))
    browser_load_requested = true;
  float area = nk_window_get_content_region(&ui).h - 140;
  if (area < 80)
    area = 80;
  nk_layout_row_dynamic(&ui, area, 1);
  char group[32];
  browser_group_name(group, browser_active);
  if (tab->reset_scroll) {
    nk_group_set_scroll(&ui, group, 0, 0);
    tab->reset_scroll = false;
  }
  if (nk_group_begin(&ui, group, 0)) {
    if (str_eq(tab->loaded_url, "about:home")) {
      nk_layout_row_dynamic(&ui, 24, 1);
      nk_spacing(&ui, 1);
      heading("Start browsing");
      nk_layout_row_dynamic(&ui, 54, 1);
      nk_label_wrap(&ui, "Keep pages together in tabs. Enter a web address "
                         "above, or open a saved document with file:/path.");
      nk_layout_row_dynamic(&ui, 42, 2);
      if (accent_button("Notes")) {
        copy(browser_url, "file:/webnotes", sizeof(browser_url));
        browser_load_requested = true;
      }
      if (nk_button_label(&ui, "Text editor"))
        open_app(EDITOR);
      if (accent_button("Open Local Files"))
        open_app(FILES);
      if (nk_button_label(&ui, "Network Settings"))
        open_app(NETWORK);
      nk_layout_row_dynamic(&ui, 34, 1);
      caption("Ctrl+T   New tab     Ctrl+W   Close tab     Ctrl+L   Address");
      nk_layout_row_dynamic(&ui, 34, 1);
      caption("Each tab keeps its own page, scripts and history.");
    } else if (!web_count) {
      heading("This page could not be displayed");
      nk_layout_row_dynamic(&ui, 80, 1);
      nk_label_wrap(&ui, browser_status);
      nk_layout_row_dynamic(&ui, 38, 2);
      if (accent_button("Try Again"))
        browser_load_requested = true;
      if (nk_button_label(&ui, "New Tab"))
        browser_new_tab("about:home");
    }
    for (unsigned i = 0; i < web_count; i++) {
      struct web_node *node = &web_nodes[i];
      if (node->style.hidden || str_eq(node->input_type, "hidden") ||
          (!*node->text && !str_eq(node->tag, "input") &&
           !str_eq(node->tag, "textarea")))
        continue;
      web_fonts[i] = body_font;
      web_fonts[i].height = node->style.size;
      nk_style_push_font(&ui, &web_fonts[i]);
      struct nk_color fg =
          nk_rgb((node->style.color >> 16) & 255,
                 (node->style.color >> 8) & 255, node->style.color & 255);
      struct nk_color bg = nk_rgb(255, 255, 255);
      int parent = i;
      while (parent >= 0) {
        uint32_t c = web_nodes[parent].style.background;
        if (c != 0xffffffffu) {
          bg = nk_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
          break;
        }
        parent = web_nodes[parent].parent;
      }
      if (str_eq(node->tag, "input") || str_eq(node->tag, "textarea")) {
        bool multiline = str_eq(node->tag, "textarea");
        nk_layout_row_dynamic(&ui, multiline ? 130 : 36, 1);
        char previous[2048];
        copy(previous, node->value, sizeof(previous));
        nk_flags edited = nk_edit_string_zero_terminated(
            &ui, multiline ? NK_EDIT_BOX : NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
            node->value, sizeof(node->value), nk_filter_default);
        if (!str_eq(previous, node->value))
          web_input(i, false);
        if (edited & (NK_EDIT_COMMITTED | NK_EDIT_DEACTIVATED))
          web_input(i, true);
      } else if (*node->href || *node->onclick || str_eq(node->tag, "button")) {
        nk_layout_row_dynamic(&ui, node->style.size + 16, 1);
        if (nk_button_label(&ui, node->text)) {
          web_click(i);
          if (*node->href) {
            char resolved[512];
            if (http_resolve(browser_tabs[browser_active].loaded_url,
                             node->href, resolved, sizeof(resolved))) {
              copy(browser_url, resolved, sizeof(browser_url));
              browser_load_requested = true;
            } else
              copy(browser_status, "That link cannot be opened",
                   sizeof(browser_status));
          }
        }
      } else {
        const char *text = node->text;
        float available =
            nk_window_get_content_region(&ui).w - 24 - node->style.padding * 2;
        if (available < 20)
          available = 20;
        while (*text) {
          unsigned n = 0, split = 0;
          while (text[n] && n < 1023 &&
                 gui_font_width(nk_handle_ptr(NULL), node->style.size, text,
                                n + 1) <= available) {
            if (text[n] == ' ')
              split = n;
            n++;
          }
          if (!n)
            n = 1;
          if (text[n] && split)
            n = split;
          nk_layout_row_dynamic(
              &ui, node->style.size + 6 + node->style.padding * 2, 1);
          struct nk_rect bounds;
          if (nk_widget(&bounds, &ui)) {
            struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
            nk_fill_rect(canvas, bounds, 0, bg);
            bounds.x += node->style.padding;
            bounds.y += node->style.padding;
            bounds.w -= node->style.padding * 2;
            bounds.h -= node->style.padding * 2;
            if (node->style.center) {
              float w = gui_font_width(nk_handle_ptr(NULL), node->style.size,
                                       text, n);
              bounds.x += (bounds.w - w) / 2;
              bounds.w = w;
            }
            nk_draw_text(canvas, bounds, text, n, &web_fonts[i], bg, fg);
          }
          text += n;
          while (*text == ' ')
            text++;
        }
      }
      nk_style_pop_font(&ui);
      if (node->style.margin) {
        nk_layout_row_dynamic(&ui, node->style.margin, 1);
        nk_spacing(&ui, 1);
      }
    }
    if (*web_message) {
      nk_layout_row_dynamic(&ui, 24, 1);
      nk_label(&ui, web_message, NK_TEXT_LEFT);
    }
    nk_group_end(&ui);
  }
}
static void images_view(void) {
  nk_layout_row_begin(&ui, NK_DYNAMIC, 32, 2);
  nk_layout_row_push(&ui, 0.80f);
  if (picture_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    picture_focus = false;
  }
  nk_flags result = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, picture_path,
      sizeof(picture_path), nk_filter_default);
  nk_layout_row_push(&ui, 0.20f);
  if (nk_button_label(&ui, "Open") || (result & NK_EDIT_COMMITTED))
    picture_load_requested = true;
  nk_layout_row_end(&ui);
  nk_layout_row_dynamic(&ui, 24, 1);
  nk_label(&ui, image_error(), NK_TEXT_LEFT);
  if (!picture.pixels)
    return;
  float w = nk_window_get_content_region(&ui).w - 24;
  float h = nk_window_get_content_region(&ui).h - 90;
  if (w < 1 || h < 1)
    return;
  float factor = w / picture.width;
  if (factor > h / picture.height)
    factor = h / picture.height;
  if (factor > 1)
    factor = 1;
  nk_layout_row_dynamic(&ui, h, 1);
  struct nk_rect bounds;
  if (nk_widget(&bounds, &ui)) {
    struct nk_image img = nk_image_ptr(picture.pixels);
    img.w = picture.width;
    img.h = picture.height;
    float iw = picture.width * factor, ih = picture.height * factor;
    bounds.x += (bounds.w - iw) / 2;
    bounds.y += (bounds.h - ih) / 2;
    bounds.w = iw;
    bounds.h = ih;
    nk_draw_image(nk_window_get_canvas(&ui), bounds, &img,
                  nk_rgb(255, 255, 255));
  }
}
static void client_name(unsigned i, char name[96]) {
  copy(name, gui_clients[i].title, 96);
  append(name, " #", 96);
  char pid[32];
  number(pid, gui_clients[i].pid);
  append(name, pid, 96);
}
static bool pointer_targets_window(const char *name) {
  struct nk_window *target = nk_window_find(&ui, name);
  for (struct nk_window *window = ui.end; window; window = window->prev) {
    if (window->flags &
        (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED | NK_WINDOW_NO_INPUT))
      continue;
    struct nk_rect bounds = window->bounds;
    if (window->flags & NK_WINDOW_MINIMIZED)
      bounds.h = 56;
    if (nk_input_is_mouse_hovering_rect(&ui.input, bounds))
      return window == target;
  }
  return false;
}
static void client_windows(void) {
  for (unsigned i = 0; i < GUI_CLIENTS; i++) {
    struct gui_client *c = &gui_clients[i];
    if (!c->pixels || c->closing)
      continue;
    char name[96];
    client_name(i, name);
    if (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) &&
        pointer_targets_window(name))
      focused_client = i;
    if (focused_client == (int)i)
      nk_window_set_focus(&ui, name);
    bool first = client_seen[i] != c->pid;
    if (first) {
      client_workspace[i] = workspaces.current;
      client_workspace_hidden[i] = false;
      client_return_app[i] = active_app;
      float w = c->width + 32, h = c->height + 105;
      if (w > width - 48)
        w = width - 48;
      if (h > height - 100)
        h = height - 100;
      client_bounds[i] = nk_rect((width - w) / 2, (height - h) / 2, w, h);
      client_seen[i] = c->pid;
      client_maximized[i] = false;
    }
    if (!workspace_window(name, client_bounds[i], client_workspace[i], &client_workspace_hidden[i]))
      continue;
    nk_set_user_data(&ui, nk_handle_id(0));
    nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR;
    for (unsigned control = 0; control < 3; control++)
      if (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) &&
          nk_input_is_mouse_hovering_rect(
              &ui.input, window_control_bounds(APP_COUNT + i, control)))
        flags &= ~NK_WINDOW_MOVABLE;
    if (held_control_app == (int)(APP_COUNT + i))
      flags &= ~NK_WINDOW_MOVABLE;
    bool contents = nk_begin(&ui, name, client_bounds[i], flags);
    client_bounds[i] = nk_window_get_bounds(&ui);
    int action = window_controls(APP_COUNT + i);
    if (nk_window_is_active(&ui, name)) {
      focused_client = i;
      active_app = -1;
    }
    if (contents) {
      struct nk_rect region = nk_window_get_content_region(&ui);
      float scale = region.w / c->width;
      if (scale > (region.h - 8) / c->height)
        scale = (region.h - 8) / c->height;
      if (scale < 0.05f)
        scale = 0.05f;
      nk_layout_row_dynamic(&ui, c->height * scale, 1);
      struct nk_rect r;
      if (nk_widget(&r, &ui)) {
        r.x += (r.w - c->width * scale) / 2;
        r.w = c->width * scale;
        struct nk_image image = nk_image_ptr(c->pixels);
        image.w = c->width;
        image.h = c->height;
        nk_draw_image(nk_window_get_canvas(&ui), r, &image,
                      nk_rgb(255, 255, 255));
        if (nk_window_is_active(&ui, name) &&
            nk_input_is_mouse_hovering_rect(&ui.input, r)) {
          int x = (ui.input.mouse.pos.x - r.x) / scale,
              y = (ui.input.mouse.pos.y - r.y) / scale;
          int buttons = ui.input.mouse.buttons[NK_BUTTON_LEFT].down |
                        (ui.input.mouse.buttons[NK_BUTTON_RIGHT].down << 1) |
                        (ui.input.mouse.buttons[NK_BUTTON_MIDDLE].down << 2);
          if (x != client_mouse_x || y != client_mouse_y ||
              buttons != client_mouse_buttons) {
            client_event(i, AV_GUI_MOUSE, x, y, buttons);
            client_mouse_x = x;
            client_mouse_y = y;
            client_mouse_buttons = buttons;
          }
        }
      }
    }
    nk_end(&ui);
    if (first)
      client_focus_request = i;
    if (action == 0)
      nk_window_collapse(&ui, name,
                         nk_window_is_collapsed(&ui, name) ? NK_MAXIMIZED
                                                           : NK_MINIMIZED);
    else if (action == 1) {
      nk_window_collapse(&ui, name, NK_MAXIMIZED);
      if (!client_maximized[i]) {
        client_restore[i] = client_bounds[i];
        client_bounds[i] = nk_rect(16, 68, width - 32, height - 164);
      } else
        client_bounds[i] = client_restore[i];
      client_maximized[i] = !client_maximized[i];
      nk_window_set_bounds(&ui, name, client_bounds[i]);
    }
    if (action == 2 || nk_window_is_closed(&ui, name)) {
      client_event(i, AV_GUI_QUIT, 0, 0, 0);
      c->closing = true;
      if (focused_client == (int)i)
        focused_client = -1;
      int previous = client_return_app[i];
      if (previous >= 0 && previous < APP_COUNT && apps[previous].visible)
        focus_request = previous;
    }
  }
}
static char calculator_expression[256], calculator_result[64] = "0";
static char calculator_memory_text[64] = "0";
static char calculator_history[16][256], calculator_values[16][64];
static unsigned calculator_count;
static double calculator_answer, calculator_memory;
static bool calculator_focus, calculator_solve_requested;
static const char *calculator_error;
static void calculator_solve(void) {
  double value;
  if (!calculator_eval(calculator_expression, calculator_answer, &value,
                       &calculator_error))
    return;
  calculator_answer = value;
  calculator_format(calculator_result, sizeof(calculator_result), value);
  if (calculator_count == 16) {
    for (unsigned i = 1; i < 16; i++) {
      copy(calculator_history[i - 1], calculator_history[i], 256);
      copy(calculator_values[i - 1], calculator_values[i], 64);
    }
    calculator_count--;
  }
  copy(calculator_history[calculator_count], calculator_expression, 256);
  copy(calculator_values[calculator_count++], calculator_result, 64);
}
static void calculator_view(void) {
  heading("Calculator");
  nk_layout_row_dynamic(&ui, 32, 1);
  caption("Decimals, parentheses, + - * / and whole-number powers (^)");
  nk_layout_row_dynamic(&ui, 40, 1);
  if (calculator_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    calculator_focus = false;
  }
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, calculator_expression,
                                 sizeof(calculator_expression), nk_filter_default);
  if (calculator_solve_requested) {
    calculator_solve_requested = false;
    calculator_solve();
  }
  nk_layout_row_dynamic(&ui, 40, 1);
  nk_style_push_font(&ui, &heading_font);
  nk_label(&ui, calculator_result, NK_TEXT_RIGHT);
  nk_style_pop_font(&ui);
  nk_layout_row_dynamic(&ui, 26, 1);
  caption(calculator_error ? calculator_error : "Enter calculates; ans recalls the last answer");
  const char *keys[] = {"(", ")", "Clear", "Backspace", "7", "8", "9", "/",
                        "4", "5", "6", "*", "1", "2", "3", "-",
                        "0", ".", "ans", "+"};
  nk_layout_row_dynamic(&ui, 32, 4);
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    if (nk_button_label(&ui, keys[i])) {
      if (i == 2) calculator_expression[0] = 0;
      else if (i == 3) {
        size_t n = strlen(calculator_expression);
        if (n) calculator_expression[n - 1] = 0;
      } else append(calculator_expression, keys[i], sizeof(calculator_expression));
      calculator_error = NULL;
      calculator_focus = true;
    }
  nk_layout_row_dynamic(&ui, 34, 2);
  if (accent_button("Calculate")) calculator_solve();
  if (nk_button_label(&ui, "Copy result"))
    clipboard_copy(nk_handle_ptr(NULL), calculator_result, strlen(calculator_result));
  nk_layout_row_dynamic(&ui, 30, 4);
  if (nk_button_label(&ui, "MC")) calculator_memory = 0;
  if (nk_button_label(&ui, "MR")) {
    calculator_format(calculator_memory_text, sizeof(calculator_memory_text), calculator_memory);
    append(calculator_expression, "(", sizeof(calculator_expression));
    append(calculator_expression, calculator_memory_text, sizeof(calculator_expression));
    append(calculator_expression, ")", sizeof(calculator_expression));
    calculator_focus = true;
  }
  if (nk_button_label(&ui, "M+")) {
    double next = calculator_memory + calculator_answer;
    if (__builtin_isfinite(next)) calculator_memory = next;
    else calculator_error = "Memory value is out of range";
  }
  if (nk_button_label(&ui, "MS")) calculator_memory = calculator_answer;
  nk_layout_row_dynamic(&ui, 24, 1);
  calculator_format(calculator_memory_text, sizeof(calculator_memory_text), calculator_memory);
  nk_label(&ui, calculator_memory_text, NK_TEXT_RIGHT);
  nk_layout_row_dynamic(&ui, 28, 2);
  caption("HISTORY - click an expression to use it again");
  if (nk_button_label(&ui, "Clear history")) calculator_count = 0;
  for (unsigned i = calculator_count; i > 0; i--) {
    nk_layout_row_dynamic(&ui, 30, 2);
    if (nk_button_label(&ui, calculator_history[i - 1])) {
      copy(calculator_expression, calculator_history[i - 1], sizeof(calculator_expression));
      calculator_focus = true;
    }
    nk_label(&ui, calculator_values[i - 1], NK_TEXT_RIGHT);
  }
}
static void application(unsigned id) {
  if (app_close_requested[id]) {
    app_close_requested[id] = false;
    if (id == EDITOR && editor_dirty) {
      open_app(EDITOR); editor_discard = close_editor_after_discard = true;
    } else {
      apps[id].visible = false;
      if (active_app == (int)id) active_app = -1;
      nk_window_show(&ui, app_names[id], NK_HIDDEN);
      if (id == GAMES) games_ui_save();
      if (id == TERMINAL && terminal_pid) user_tty_input(3);
    }
  }
  if (!apps[id].visible)
    return;
  if (!workspace_window(app_names[id], apps[id].bounds, workspaces.homes[id], &workspace_hidden[id]))
    return;
  nk_set_user_data(&ui, nk_handle_id(id + 1));
  nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                   NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR;
  bool control_press = false;
  for (unsigned control = 0; control < 3; control++)
    if (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ui.input,
                                        window_control_bounds(id, control)))
      control_press = true;
  if (control_press || held_control_app == (int)id)
    flags &= ~NK_WINDOW_MOVABLE;
  bool content = nk_begin(&ui, app_names[id], apps[id].bounds, flags);
  apps[id].bounds = nk_window_get_bounds(&ui);
  int action = -1;
  if (!nk_window_is_hidden(&ui, app_names[id]) &&
      !nk_window_is_closed(&ui, app_names[id]))
    action = window_controls(id);
  if (content) {
    if (nk_window_is_active(&ui, app_names[id])) {
      active_app = id;
      if (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) &&
          pointer_targets_window(app_names[id]))
        focused_client = -1;
    }
    if (id == TERMINAL)
      terminal_view();
    else if (id == FILES)
      files_view();
    else if (id == EDITOR)
      editor_view();
    else if (id == TASKS)
      tasks_view();
    else if (id == NETWORK)
      network_view();
    else if (id == PACKAGES)
      packages_view();
    else if (id == BROWSER)
      browser_view();
    else if (id == GAMES)
      games_ui(&ui, rgb(palette()->text), rgb(palette()->muted),
               rgb(palette()->surface), rgb(accent()));
    else if (id == MUSIC) {
      struct av_audio_status status;
      audio_snapshot(&status);
      heading("Music");
      unsigned outputs = audio_output_count();
      if (outputs) {
        unsigned selected = audio_output_selected();
        static const char *types[] = {"Line out", "Speaker", "Headphones"};
        char label[64], index[32];
        number(index, selected + 1);
        copy(label, "Output ", sizeof(label));
        append(label, index, sizeof(label));
        append(label, " (", sizeof(label));
        unsigned type = audio_output_type(selected);
        append(label, type < 3 ? types[type] : "Analogue", sizeof(label));
        append(label, ")", sizeof(label));
        nk_layout_row_dynamic(&ui, 30, 1);
        if (nk_combo_begin_label(&ui, label, nk_vec2(250, 240))) {
          nk_layout_row_dynamic(&ui, 26, 1);
          for (unsigned i = 0; i < outputs; i++) {
            type = audio_output_type(i);
            number(index, i + 1);
            copy(label, index, sizeof(label));
            append(label, ". ", sizeof(label));
            append(label, type < 3 ? types[type] : "Analogue", sizeof(label));
            if (nk_button_label(&ui, label)) {
              int result = audio_output_select(i);
              if (result < 0)
                notify(result == -16 ? "Pause playback before switching outputs"
                                     : "Could not configure this audio output");
              nk_combo_close(&ui);
              break;
            }
          }
          nk_combo_end(&ui);
        }
        nk_layout_row_dynamic(&ui, 26, 2);
        int volume = audio_volume();
        nk_bool mute = audio_muted();
        bool changed = nk_slider_int(&ui, 0, &volume, 100, 1);
        if (nk_checkbox_label(&ui, "Mute", &mute)) changed = true;
        if (changed && audio_set_volume(volume, mute) < 0)
          notify("This output does not expose adjustable gain");
      }
      nk_layout_row_dynamic(&ui, 34, 1);
      if (music_focus) {
        nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
        music_focus = false;
      }
      nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, music_path,
                                     sizeof(music_path), nk_filter_default);
      nk_layout_row_dynamic(&ui, 38, 2);
      bool same_track = str_eq(status.path, music_path);
      bool paused = same_track && audio_paused();
      bool playing = same_track && status.playing;
      if (accent_button(playing ? "Pause" : paused ? "Resume" : "Play")) {
        if (playing)
          music_result = audio_pause();
        else if (paused)
          music_result = audio_resume();
        else {
          audio_stop();
          music_result = audio_play(music_path);
        }
      }
      if (nk_button_label(&ui, "Stop")) {
        audio_stop();
        music_result = 0;
      }
      nk_layout_row_dynamic(&ui, 36, 1);
      nk_label_wrap(&ui, !status.available
                             ? "No compatible audio output detected."
                         : status.playing ? "Playing"
                         : audio_paused() ? "Paused"
                         : status.frames_total &&
                                 status.frames_played >= status.frames_total
                             ? "Playback finished"
                             : "Choose a WAV file, or open one from Files.");
      if (*status.path) {
        nk_layout_row_dynamic(&ui, 42, 1);
        nk_label_wrap(&ui, status.path);
      }
      nk_layout_row_dynamic(&ui, 18, 1);
      if (same_track && status.frames_total > 1) {
        unsigned last_frame = status.frames_total - 1;
        unsigned frame = status.frames_played < last_frame
                             ? status.frames_played : last_frame;
        int position = (uint64_t)frame * 1000 / last_frame;
        if (nk_slider_int(&ui, 0, &position, 1000, 1))
          music_result = audio_seek((uint64_t)position * last_frame / 1000);
      } else {
        nk_size progress = 0;
        nk_progress(&ui, &progress, 1000, NK_FIXED);
      }
      nk_layout_row_dynamic(&ui, 26, 2);
      label_value("Sample rate: ", status.rate, " Hz");
      label_value("Channels: ", status.channels, "");
      label_value("Played: ",
                  status.rate ? status.frames_played / status.rate : 0, " s");
      label_value("Duration: ",
                  status.rate ? status.frames_total / status.rate : 0, " s");
      if (music_result < 0 || status.error < 0) {
        nk_layout_row_dynamic(&ui, 32, 1);
        label_value("Playback error: ",
                    -(music_result < 0 ? music_result : status.error), "");
      }
    } else if (id == CALCULATOR)
      calculator_view();
    else if (id == IMAGES)
      images_view();
    else
      settings_view();
  }
  nk_end(&ui);
  if (action == 0)
    nk_window_collapse(&ui, app_names[id],
                       nk_window_is_collapsed(&ui, app_names[id])
                           ? NK_MAXIMIZED
                           : NK_MINIMIZED);
  else if (action == 1) {
    nk_window_collapse(&ui, app_names[id], NK_MAXIMIZED);
    maximize(id);
  } else if (action == 2)
    nk_window_show(&ui, app_names[id], NK_HIDDEN);
  if (nk_window_is_closed(&ui, app_names[id]) ||
      nk_window_is_hidden(&ui, app_names[id])) {
    apps[id].visible = false;
    if (active_app == (int)id) active_app = -1;
    if (id == GAMES)
      games_ui_save();
    if (id == EDITOR && editor_dirty) {
      open_app(EDITOR);
      editor_discard = true;
      close_editor_after_discard = true;
    }
    if (id == TERMINAL && terminal_pid)
      user_tty_input(3);
  }
}
static void top_panel(void) {
  nk_style_push_style_item(&ui, &ui.style.window.fixed_background,
                           nk_style_item_color(nk_rgba(15, 21, 33, 170)));
  nk_style_push_vec2(&ui, &ui.style.window.padding, nk_vec2(10, 6));
  nk_style_push_style_item(&ui, &ui.style.button.normal,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  nk_style_push_vec2(&ui, &ui.style.button.padding, nk_vec2(8, 4));
  nk_style_push_color(&ui, &ui.style.text.color, nk_rgb(235, 232, 244));
  if (nk_begin(&ui, "Desktop panel", nk_rect(0, 0, width, 40),
               NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
    nk_layout_row_template_begin(&ui, 26);
    nk_layout_row_template_push_static(&ui, 32);
    nk_layout_row_template_push_static(&ui, 132);
    nk_layout_row_template_push_dynamic(&ui);
    nk_layout_row_template_push_static(&ui, width > 800 ? 140 : 128);
    nk_layout_row_template_push_static(&ui, 104);
    nk_layout_row_template_push_static(&ui, width > 800 ? 188 : 0);
    nk_layout_row_template_push_static(&ui, 32);
    nk_layout_row_template_end(&ui);
    struct nk_rect logo = nk_widget_bounds(&ui);
    if (nk_button_label(&ui, ""))
      launcher = !launcher;
    gui_canvas_icon(nk_window_get_canvas(&ui), logo, 7, nk_rgb(255, 140, 208));
    if (nk_button_label(&ui, "Applications"))
      launcher = !launcher;
    nk_spacer(&ui);
    if (nk_combo_begin_label(&ui, workspaces.names[workspaces.current], nk_vec2(240, 340))) {
      nk_layout_row_dynamic(&ui, 30, 1);
      for (unsigned i = 0; i < workspaces.count; i++)
        if (nk_button_label(&ui, workspaces.names[i])) { workspace_request(i); nk_combo_close(&ui); break; }
      if (nk_button_label(&ui, "Manage workspaces")) { settings_page = 8; open_app(SETTINGS); }
      nk_combo_end(&ui);
    }
    if (nk_button_label(&ui, network_status.ip ? "Network" : "Offline"))
      open_app(NETWORK);
    if (width > 800)
      nk_label(&ui, clock_text, NK_TEXT_RIGHT);
    else
      nk_spacer(&ui);
    struct nk_rect power = nk_widget_bounds(&ui);
    if (nk_button_label(&ui, ""))
      power_dialog = true;
    gui_canvas_icon(nk_window_get_canvas(&ui), power, 8, rgb(palette()->text));
  }
  nk_end(&ui);
  nk_style_pop_color(&ui);
  nk_style_pop_vec2(&ui);
  nk_style_pop_vec2(&ui);
  nk_style_pop_style_item(&ui);
  nk_style_pop_style_item(&ui);
}
static void app_badge(struct nk_command_buffer *canvas, struct nk_rect r,
                      int id) {
  static const uint32_t colors[] = {0x263347, 0x279ce0, 0xb561df, 0x23ad95,
                                    0x297fda, 0xe09132, 0x64758e, 0xe6007e};
  unsigned kind = id < 0          ? 7
                  : id == BROWSER ? 4
                  : id == IMAGES  ? 9
                  : id == MUSIC   ? 10
                  : id == CALCULATOR ? 12
                  : id == GAMES   ? 11
                                  : (unsigned)id;
  struct nk_color base =
      rgb(kind >= 9 ? (kind == 10 ? 0xd64a8c : 0x7d59c7) : colors[kind]);
  nk_fill_rect(canvas, nk_rect(r.x, r.y + 3, r.w, r.h), 10,
               nk_rgba(0, 0, 0, 65));
  nk_fill_rect(canvas, r, 10, base);
  gui_canvas_gloss(canvas, r, 34);
  nk_stroke_line(canvas, r.x + 10, r.y + r.h - 3, r.x + r.w - 10, r.y + r.h - 3,
                 1, nk_rgba(0, 0, 0, 80));
  gui_canvas_icon(canvas, r, kind, nk_rgb(250, 250, 255));
}
static void context_for(int app, struct nk_rect r) {
  if (nk_input_is_mouse_hovering_rect(&ui.input, r) &&
      nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_RIGHT)) {
    context_app = app;
    context_open = true;
    float x = ui.input.mouse.pos.x, y = ui.input.mouse.pos.y;
    float menu_width = app < 0 ? 316 : 250;
    float menu_height = app < 0 ? (*file_clipboard ? 426 : 388) : 116;
    context_bounds =
        nk_rect(x + menu_width > width ? width - menu_width - 6 : x,
                y + menu_height > height ? height - menu_height - 6 : y,
                menu_width, menu_height);
  }
}
static bool desktop_pointer_free(void) {
  if (launcher || power_dialog || editor_discard || deleting_file ||
      ending_task || removing_package)
    return false;
  if (context_open &&
      nk_input_is_mouse_hovering_rect(&ui.input, context_bounds))
    return false;
  for (unsigned i = 0; i < APP_COUNT; i++) {
    if (!apps[i].visible || workspaces.homes[i] != workspaces.current)
      continue;
    struct nk_rect bounds = apps[i].bounds;
    if (nk_window_is_collapsed(&ui, app_names[i]))
      bounds.h = 44;
    if (nk_input_is_mouse_hovering_rect(&ui.input, bounds))
      return false;
  }
  for (unsigned i = 0; i < GUI_CLIENTS; i++) {
    if (!gui_clients[i].pixels || gui_clients[i].closing || client_workspace[i] != workspaces.current)
      continue;
    char name[96];
    client_name(i, name);
    struct nk_rect bounds = client_bounds[i];
    if (nk_window_is_collapsed(&ui, name))
      bounds.h = 56;
    if (nk_input_is_mouse_hovering_rect(&ui.input, bounds))
      return false;
  }
  if (desktop.top_bar && ui.input.mouse.pos.y < 48)
    return false;
  if (desktop.dock_bar) {
    float dock_width = APP_COUNT * 58 + 58;
    if (dock_width > width - 28)
      dock_width = width - 28;
    if (nk_input_is_mouse_hovering_rect(
            &ui.input,
            nk_rect((width - dock_width) / 2, height - 66 - desktop.dock_bottom,
                    dock_width, 66)))
      return false;
  }
  return true;
}
static void desktop_context_input(void) {
  if (!nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_RIGHT) ||
      !desktop_pointer_free())
    return;
  context_for(-2, nk_rect(0, 0, width, height));
}
static void desktop_shortcuts(void) {
  if (!desktop.desktop_icons)
    return;
  nk_style_push_style_item(&ui, &ui.style.window.fixed_background,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  nk_style_push_style_item(&ui, &ui.style.button.normal,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  nk_style_push_style_item(&ui, &ui.style.button.hover,
                           nk_style_item_color(nk_rgba(255, 255, 255, 25)));
  nk_style_push_float(&ui, &ui.style.button.border, 0);
  unsigned count = 0;
  for (unsigned i = 0; i < APP_COUNT; i++)
    if (desktop.shortcuts & (1u << i))
      count++;
  if (nk_begin(&ui, "Desktop shortcuts",
               nk_rect(desktop.icons_x, desktop.icons_y, 116, count * 92 + 28),
               NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR)) {
    for (unsigned i = 0; i < APP_COUNT; i++) {
      if (!(desktop.shortcuts & (1u << i)))
        continue;
      nk_layout_row_dynamic(&ui, 54, 1);
      struct nk_rect r = nk_widget_bounds(&ui);
      if (nk_button_label(&ui, "")) {
        if (i == FILES)
          browse("/");
        open_app(i);
      }
      struct nk_rect shortcut = r;
      shortcut.h += 31;
      if (desktop_pointer_free())
        context_for(i, shortcut);
      app_badge(nk_window_get_canvas(&ui),
                nk_rect(r.x + (r.w - 44) / 2, r.y + 4, 44, 44), i);
      nk_layout_row_dynamic(&ui, 24, 1);
      nk_label_colored(&ui, app_names[i], NK_TEXT_CENTERED,
                       nk_rgb(245, 249, 255));
    }
  }
  nk_end(&ui);
  nk_style_pop_float(&ui);
  nk_style_pop_style_item(&ui);
  nk_style_pop_style_item(&ui);
  nk_style_pop_style_item(&ui);
}
static void shortcut_menu(void) {
  if (!context_open)
    return;
  nk_style_push_style_item(&ui, &ui.style.button.normal,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  nk_style_push_vec2(&ui, &ui.style.button.padding, nk_vec2(8, 6));
  nk_style_push_flags(&ui, &ui.style.button.text_alignment, NK_TEXT_LEFT);
  nk_window_set_bounds(&ui, "Shortcut menu", context_bounds);
  if (nk_begin(&ui, "Shortcut menu", context_bounds,
               NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_row_dynamic(&ui, 26, 1);
    if (context_app < 0) {
      caption("DESKTOP");
      nk_layout_row_dynamic(&ui, 32, 1);
      if (nk_button_label(&ui, "New Folder")) {
        browse("/");
        open_app(FILES);
        file_begin_create(true);
        context_open = false;
      }
      if (nk_button_label(&ui, "New File")) {
        browse("/");
        open_app(FILES);
        file_begin_create(false);
        context_open = false;
      }
      if (nk_button_label(&ui, "Open Files")) {
        browse("/");
        open_app(FILES);
        context_open = false;
      }
      if (*file_clipboard && nk_button_label(&ui, "Paste into Home")) {
        browse("/");
        file_paste();
        context_open = false;
      }
      if (nk_button_label(&ui, "Open Terminal")) {
        open_app(TERMINAL);
        context_open = false;
      }
      if (nk_button_label(&ui, "Change Wallpaper and Appearance")) {
        settings_page = 0;
        open_app(SETTINGS);
        context_open = false;
      }
      if (nk_button_label(&ui, desktop.desktop_icons ? "Hide Desktop Icons"
                                                     : "Show Desktop Icons")) {
        desktop.desktop_icons ^= 1;
        desktop_dirty = true;
        desktop_save();
        context_open = false;
      }
      if (nk_button_label(&ui, "System Monitor")) {
        open_app(TASKS);
        context_open = false;
      }
      if (nk_button_label(&ui, "Refresh Desktop")) {
        refresh_files();
        update_status();
        gui_canvas_wallpaper(accent(), preferences.wallpaper);
        context_open = false;
      }
    } else {
      nk_label(&ui, app_names[context_app], NK_TEXT_LEFT);
      nk_layout_row_dynamic(&ui, 34, 1);
      if (nk_button_label(&ui, desktop.shortcuts & (1u << context_app)
                                   ? "Remove shortcut"
                                   : "Add shortcut")) {
        desktop.shortcuts ^= 1u << context_app;
        desktop_dirty = true;
        desktop_save();
        context_open = false;
      }
    }
  }
  nk_end(&ui);
  nk_style_pop_flags(&ui);
  nk_style_pop_vec2(&ui);
  nk_style_pop_style_item(&ui);
  if (nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT) &&
      !nk_input_is_mouse_hovering_rect(&ui.input, context_bounds))
    context_open = false;
}
static void dock(void) {
  if (!desktop.dock_bar)
    return;
  float dock_width = APP_COUNT * 58 + 58, spacing = 58;
  if (dock_width > width - 28) {
    dock_width = width - 28;
    spacing = (dock_width - 22) / (APP_COUNT + 1);
  }
  nk_style_push_vec2(&ui, &ui.style.window.padding, nk_vec2(10, 8));
  if (nk_begin(&ui, "Application dock",
               nk_rect((width - dock_width) / 2,
                       height - 66 - desktop.dock_bottom, dock_width, 66),
               NK_WINDOW_NO_SCROLLBAR)) {
    gui_canvas_gloss(nk_window_get_canvas(&ui), nk_window_get_bounds(&ui), 46);
    nk_layout_row_static(&ui, 44, spacing - 12, APP_COUNT + 1);
    for (unsigned slot = 0; slot < APP_COUNT + 1; slot++) {
      int id = slot ? ((int)slot - 1) : -1;
      struct nk_rect bounds = nk_widget_bounds(&ui);
      bool selected = id >= 0 && apps[id].visible &&
                      workspaces.homes[id] == workspaces.current && active_app == id;
      nk_style_push_style_item(
          &ui, &ui.style.button.normal,
          nk_style_item_color(selected ? alpha_color(accent(), 35)
                                       : nk_rgba(0, 0, 0, 0)));
      if (nk_button_label(&ui, "")) {
        if (id < 0)
          launcher = !launcher;
        else
          open_app(id);
      }
      nk_style_pop_style_item(&ui);
      if (id >= 0)
        context_for(id, bounds);
      app_badge(nk_window_get_canvas(&ui),
                nk_rect(bounds.x + 3, bounds.y + 2, bounds.w - 6, bounds.h - 5),
                id);
      if (id >= 0 && apps[id].visible)
        nk_fill_circle(
            nk_window_get_canvas(&ui),
            nk_rect(bounds.x + bounds.w / 2 - 2, bounds.y + bounds.h - 5, 4, 4),
            rgb(accent()));
      if (nk_input_is_mouse_hovering_rect(&ui.input, bounds))
        nk_tooltip(&ui, id < 0 ? "Applications" : app_names[id]);
    }
  }
  nk_end(&ui);
  nk_style_pop_vec2(&ui);
}
static void launcher_view(void) {
  if (!launcher)
    return;
  struct nk_window *menu = nk_window_find(&ui, "Applications");
  if (menu)
    menu->flags &= ~(NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED);
  float h = height > 650 ? 560 : height - 110;
  float w = width > 740 ? 680 : width - 40;
  if (nk_begin(&ui, "Applications", nk_rect(20, 52, w, h),
               NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
    float title_columns[] = {0.78f, 0.22f};
    nk_layout_row(&ui, NK_DYNAMIC, 44, 2, title_columns);
    nk_style_push_font(&ui, &heading_font);
    nk_label(&ui, "Applications", NK_TEXT_LEFT);
    nk_style_pop_font(&ui);
    if (nk_button_label(&ui, "Close"))
      launcher = false;
    nk_layout_row_dynamic(&ui, 34, 1);
    nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, launcher_search,
                                   sizeof(launcher_search), nk_filter_default);
    nk_layout_row_dynamic(&ui, 28, 1);
    caption("Search by name, or choose a category");
    const char *categories[] = {"All apps", "Work", "System", "Network",
                                "Favorites"};
    nk_layout_row_dynamic(&ui, 32, 5);
    for (unsigned i = 0; i < 5; i++) {
      nk_bool chosen = launcher_category == i;
      if (nk_selectable_label(&ui, categories[i], NK_TEXT_CENTERED, &chosen))
        launcher_category = i;
    }
    nk_layout_row_dynamic(&ui, h - 245, 1);
    if (nk_group_begin(&ui, "Application grid", 0)) {
    nk_layout_row_dynamic(&ui, 112, 4);
    unsigned matches = 0;
    for (unsigned i = 0; i < APP_COUNT; i++)
      if ((launcher_category == 0 ||
           (launcher_category == 1 &&
            (i == FILES || i == EDITOR || i == IMAGES || i == CALCULATOR)) ||
           (launcher_category == 2 &&
            (i == TERMINAL || i == TASKS || i == SETTINGS)) ||
           (launcher_category == 3 &&
            (i == NETWORK || i == PACKAGES || i == BROWSER)) ||
           (launcher_category == 4 && i <= EDITOR)) &&
          (contains(app_names[i], launcher_search) ||
           contains(app_summaries[i], launcher_search))) {
        matches++;
        struct nk_rect r = nk_widget_bounds(&ui);
        if (nk_button_label(&ui, ""))
          open_app(i);
        context_for(i, r);
        struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
        app_badge(canvas, nk_rect(r.x + (r.w - 48) / 2, r.y + 12, 48, 48), i);
        float label_width = gui_font_width(body_font.userdata, 16, app_names[i],
                                           strlen(app_names[i]));
        nk_draw_text(
            canvas,
            nk_rect(r.x + (r.w - label_width) / 2, r.y + 76, label_width, 24),
            app_names[i], strlen(app_names[i]), &body_font, nk_rgba(0, 0, 0, 0),
            rgb(palette()->text));
      }
    static const struct {
      const char *name, *path, *summary;
      unsigned category, icon;
    } external_apps[] = {
        {"ClassiCube", "/classicube.mgnta", "Creative multiplayer block game", 1, GAMES},
        {"NetSurf", "/netsurf.mgnta", "Browse the web with HTML CSS and JavaScript", 3, BROWSER}};
    for (unsigned i = 0; i < sizeof(external_apps) / sizeof(external_apps[0]); i++) {
      if (launcher_category != 0 && launcher_category != external_apps[i].category &&
          launcher_category != 4)
        continue;
      if (!contains(external_apps[i].name, launcher_search) &&
          !contains(external_apps[i].summary, launcher_search))
        continue;
      matches++;
      struct nk_rect r = nk_widget_bounds(&ui);
      if (nk_button_label(&ui, "")) {
        if (user_spawn(external_apps[i].path, "", false) < 0)
          notify("Could not launch application from disk");
        else
          launcher = false;
      }
      struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
      app_badge(canvas, nk_rect(r.x + (r.w - 48) / 2, r.y + 12, 48, 48),
                external_apps[i].icon);
      const char *name = external_apps[i].name;
      float label_width = gui_font_width(body_font.userdata, 16, name, strlen(name));
      nk_draw_text(canvas, nk_rect(r.x + (r.w - label_width) / 2, r.y + 76,
                                  label_width, 24),
                   name, strlen(name), &body_font, nk_rgba(0, 0, 0, 0),
                   rgb(palette()->text));
    }
    if (launcher_category == 0 || launcher_category == 4) {
      for (unsigned i = 0; i < package_count; i++) {
        const struct package_item *item = &package_items[i];
        if (!item->installed || !contains(item->name, launcher_search))
          continue;
        matches++;
        if (nk_button_label(&ui, item->name)) {
          if (user_spawn(item->name, "", false) < 0)
            notify("Could not launch the installed application");
          else
            launcher = false;
        }
      }
    }
    if (!matches) {
      nk_layout_row_dynamic(&ui, 44, 1);
      caption("No applications match your search.");
    }
    nk_group_end(&ui);
    }
    nk_layout_row_dynamic(&ui, 32, 2);
    if (nk_button_label(&ui, "Text desktop")) {
      if (editor_dirty) {
        editor_discard = true;
        exit_after_discard = true;
      } else
        running = false;
    }
    if (nk_button_label(&ui, "Power"))
      power_dialog = true;
  }
  nk_end(&ui);
  if (nk_window_is_closed(&ui, "Applications") ||
      nk_window_is_hidden(&ui, "Applications"))
    launcher = false;
}
static void modal_dialogs(void) {
  const char *name = power_dialog       ? "Session"
                     : deleting_file    ? (emptying_trash ? "Empty Trash" : "Move to Trash")
                     : ending_task      ? "End process"
                     : removing_package ? "Remove package"
                     : editor_discard   ? "Discard edits"
                                        : NULL;
  if (!name)
    return;
  if (nk_begin(&ui, name,
               nk_rect((width - 520) / 2, (height - 280) / 2, 520, 280),
               NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_row_dynamic(&ui, 140, 1);
    char confirmation[384];
    if (deleting_file && emptying_trash) {
      copy(confirmation, "Permanently remove all items in Trash?",
           sizeof(confirmation));
    } else if (deleting_file) {
      copy(confirmation, "Move ", sizeof(confirmation));
      append(confirmation, deleting_path, sizeof(confirmation));
      append(confirmation, " to Trash?",
             sizeof(confirmation));
    } else if (ending_task) {
      char pid[32];
      number(pid, ending_pid);
      copy(confirmation, "End process ", sizeof(confirmation));
      append(confirmation, pid, sizeof(confirmation));
      append(confirmation, "? Unsaved work in this process will be lost.",
             sizeof(confirmation));
    } else
      copy(confirmation,
           "Confirm this action. Saved changes take effect immediately.",
           sizeof(confirmation));
    nk_label_wrap(&ui, power_dialog ? "Choose a session action."
                       : editor_discard
                           ? "Discard the unsaved changes in this document?"
                           : confirmation);
    nk_layout_row_dynamic(&ui, 36, power_dialog || editor_discard ? 3 : 2);
    if (power_dialog) {
      if (nk_button_label(&ui, "Restart"))
        kernel_power(AV_POWER_REBOOT);
      if (nk_button_label(&ui, "Halt"))
        kernel_power(AV_POWER_HALT);
      if (nk_button_label(&ui, "Text desktop")) {
        if (editor_dirty) {
          editor_discard = true;
          exit_after_discard = true;
        } else
          running = false;
        power_dialog = false;
      }
    } else if (editor_discard) {
      bool finish = false;
      if (accent_button("Save")) {
        save_editor();
        finish = !editor_dirty;
      }
      if (nk_button_label(&ui, "Discard")) {
        editor_dirty = false;
        if (editor_draft_written && !editor_draft_available &&
            fs_remove(EDITOR_DRAFT_PATH) == 0)
          editor_draft_written = false;
        finish = true;
      }
      if (finish) {
        if (exit_after_discard)
          running = false;
        else if (close_editor_after_discard) {
          apps[EDITOR].visible = false;
          nk_window_show(&ui, app_names[EDITOR], NK_HIDDEN);
        } else {
          editor[0] = 0;
          copy(editor_path, "/notes", sizeof(editor_path));
          editor_focus = 1;
        }
        editor_discard = close_editor_after_discard = exit_after_discard =
            false;
      }
    } else if (accent_button("Confirm")) {
      if (deleting_file && (emptying_trash || *deleting_path)) {
        char trash[256];
        trash_path(trash, browser_path);
        int r = emptying_trash ? empty_trash(trash)
                              : move_to_trash(deleting_path);
        if (r < 0)
          file_error("Deletion failed", r);
        else
          notify(emptying_trash ? "Trash emptied" : "Item moved to Trash");
        if (emptying_trash && r >= 0)
          browse("/");
        emptying_trash = false;
        refresh_files();
      }
      if (ending_task && ending_pid > 0) {
        int r = user_kill(ending_pid, 9);
        notify(r < 0 ? "Unable to end this process" : "Process terminated");
        update_status();
      }
      if (removing_package)
        package_action("remove");
      deleting_file = ending_task = removing_package = editor_discard = false;
    }
    if (nk_button_label(&ui, "Cancel")) {
      emptying_trash = false;
      power_dialog = deleting_file = ending_task = removing_package =
          editor_discard = close_editor_after_discard = exit_after_discard =
              false;
    }
  }
  nk_end(&ui);
}
static void poll_processes(void) {
  for (unsigned slot = 0; slot < 256; slot++) {
    struct task_info t;
    if (!user_task_info(slot, &t) || t.state != 6)
      continue;
    if (t.pid == terminal_pid) {
      user_wait_pid(t.pid);
      terminal_pid = 0;
      terminal_append("\nShell exited. Open Terminal to start a new shell.\n");
    } else if (t.parent == 0)
      user_wait_pid(t.pid);
  }
}
static void inputs(void) {
  nk_input_begin(&ui);
  for (unsigned key = 0; key < NK_KEY_MAX; key++)
    nk_input_key(&ui, key, nk_false);
  int x, y;
  uint8_t buttons;
  mouse_input_state(&x, &y, &buttons);
  x /= native_scale;
  y /= native_scale;
  nk_input_motion(&ui, x, y);
  nk_input_button(&ui, NK_BUTTON_LEFT, x, y, (buttons & 1) != 0);
  nk_input_button(&ui, NK_BUTTON_RIGHT, x, y, (buttons & 2) != 0);
  nk_input_button(&ui, NK_BUTTON_MIDDLE, x, y, (buttons & 4) != 0);
  for (unsigned n = 0; n < 64; n++) {
    if (ui.input.keyboard.text_len >= NK_INPUT_MAX - 4)
      break;
    char c = console_getchar();
    if (!c)
      break;
    if (focused_client >= 0 && c == 17) {
      client_event(focused_client, AV_GUI_QUIT, 0, 0, 0);
      focused_client = -1;
      continue;
    }
    if (focused_client >= 0 &&
        (c >= 32 || c == '\n' || c == '\b' || c == '\t' || c == 27)) {
      client_event(focused_client, AV_GUI_TEXT, (unsigned char)c, 0, 0);
      continue;
    }
    if (active_app == IMAGES && c == '\n') {
      picture_load_requested = true;
      continue;
    }
    if (active_app == MUSIC && c == '\n') {
      music_play_requested = true;
      continue;
    }
    if (c == 27) {
      escape_requested = true;
      continue;
    }
    if (active_app == BROWSER && c == 18) {
      copy(browser_url, browser_tabs[browser_active].loaded_url,
           sizeof(browser_url));
      browser_load_requested = true;
      continue;
    }
    if (active_app == BROWSER && c == 20) {
      browser_new_tab("about:home");
      continue;
    }
    if (active_app == BROWSER && c == 23) {
      browser_close_tab(browser_active);
      continue;
    }
    if (active_app == FILES && !files_editing &&
        (c == 3 || c == 24 || c == 22)) {
      if (c == 22)
        file_paste();
      else
        file_copy_selection(c == 24);
      continue;
    }
    if (active_app == FILES && c == 6) {
      files_search_focus = 1;
      continue;
    }
    if (active_app == FILES && c == '\n' && !files_editing &&
        selected_file >= 0) {
      open_file(selected_file);
      continue;
    }
    if (active_app == CALCULATOR && c == '\n') {
      calculator_solve_requested = true;
      continue;
    }
    if (active_app == GAMES && games_ui_key(c))
      continue;
    if (c == 7) {
      settings_page = 5;
      open_app(SETTINGS);
      continue;
    }
    if (c == 12) {
      if (active_app == FILES) {
        files_address_focus = 1;
        continue;
      }
      open_app(BROWSER);
      browser_focus = 1;
      continue;
    }
    int shortcut = c == 20   ? TERMINAL
                   : c == 6  ? FILES
                   : c == 5  ? EDITOR
                   : c == 11 ? TASKS
                   : c == 14 ? NETWORK
                   : c == 16 ? PACKAGES
                   : c == 2  ? BROWSER
                   : c == 15 ? SETTINGS
                   : c == 21 ? IMAGES
                   : c == 4  ? MUSIC
                   : c == 25 ? GAMES
                             : -1;
    if (shortcut >= 0) {
      open_app(shortcut);
      continue;
    }
    if (c == 19 && active_app == EDITOR) {
      save_requested = true;
      continue;
    }
    if (c == 13) {
      maximize(active_app);
      continue;
    }
    if (c == 3 && active_app == TERMINAL && terminal_pid) {
      user_tty_input(3);
      terminal_line[0] = 0;
      continue;
    }
    if (c == '\n')
      nk_input_key(&ui, NK_KEY_ENTER, nk_true);
    else if (c == '\b')
      nk_input_key(&ui, NK_KEY_BACKSPACE, nk_true);
    else if (c == '\t')
      nk_input_key(&ui, NK_KEY_TAB, nk_true);
    else if (c == 3)
      nk_input_key(&ui, NK_KEY_COPY, nk_true);
    else if (c == 22)
      nk_input_key(&ui, NK_KEY_PASTE, nk_true);
    else if (c == 24)
      nk_input_key(&ui, NK_KEY_CUT, nk_true);
    else if (c == 1)
      nk_input_key(&ui, NK_KEY_TEXT_SELECT_ALL, nk_true);
    else if (c == 26)
      nk_input_key(&ui, NK_KEY_TEXT_UNDO, nk_true);
    else if (c == 25)
      nk_input_key(&ui, NK_KEY_TEXT_REDO, nk_true);
    else if ((unsigned char)c >= 32 && (unsigned char)c < 127)
      nk_input_char(&ui, c);
  }
  for (unsigned event = 0; event < 128; event++) {
    int key = graphics_key_event();
    if (!key)
      break;
    if (key == 0x64b || key == 0x64d || key == 0x74b || key == 0x74d) {
      unsigned base = workspace_pending >= 0 ? (unsigned)workspace_pending : workspaces.current;
      unsigned target = (base + ((key & 255) == 0x4b ? workspaces.count - 1 : 1)) % workspaces.count;
      if (key & 0x100) {
        int moving = focused_client >= 0 ? APP_COUNT + focused_client : active_app;
        if (moving >= 0) {
          workspace_focus[target] = moving;
          workspace_move(moving, target);
        }
      }
      workspace_request(target);
      continue;
    }
    if (focused_client >= 0) {
      client_event(focused_client, AV_GUI_KEY, key, 0, 0);
      continue;
    }
    if (active_app == GAMES &&
        (key == 0x4b || key == 0x4d || key == 0x48 || key == 0x50)) {
      games_ui_direction(key == 0x4b   ? 0
                         : key == 0x4d ? 1
                         : key == 0x48 ? 2
                                       : 3);
      continue;
    }
    if (active_app == BROWSER && (key == 0x100 || key == 0x101)) {
      unsigned at = 0;
      while (at < browser_tab_count && browser_order[at] != browser_active)
        at++;
      if (browser_tab_count)
        browser_switch(
            browser_order[(at + browser_tab_count + (key == 0x100 ? 1 : -1)) %
                          browser_tab_count]);
    } else if (active_app == FILES && key == 0x53 && !files_editing &&
               selected_file >= 0) {
      file_begin_delete();
    } else if (active_app == FILES && key == 0x3c && !files_editing) {
      file_begin_rename();
    } else if (active_app == TERMINAL && (key == 0x48 || key == 0x50) &&
               history_count) {
      unsigned oldest = history_count > 16 ? history_count - 16 : 0;
      if (key == 0x48 && history_cursor > oldest)
        history_cursor--;
      if (key == 0x50 && history_cursor < history_count)
        history_cursor++;
      copy(terminal_line,
           history_cursor < history_count ? history[history_cursor % 16] : "",
           sizeof(terminal_line));
      terminal_focus = 1;
    } else {
      enum nk_keys mapped = key == 0x4b   ? NK_KEY_LEFT
                            : key == 0x4d ? NK_KEY_RIGHT
                            : key == 0x48 ? NK_KEY_UP
                            : key == 0x50 ? NK_KEY_DOWN
                            : key == 0x47 ? NK_KEY_TEXT_LINE_START
                            : key == 0x4f ? NK_KEY_TEXT_LINE_END
                            : key == 0x53 ? NK_KEY_DEL
                                          : NK_KEY_NONE;
      if (mapped != NK_KEY_NONE)
        nk_input_key(&ui, mapped, nk_true);
    }
  }
  nk_input_end(&ui);
}
int gui_run(void) {
  if (!graphics_size(&width, &height))
    return -19;
  if (!font_init())
    return -22;
  body_font = (struct nk_user_font){
      .userdata = nk_handle_ptr(NULL), .height = 16, .width = gui_font_width};
  heading_font = body_font;
  heading_font.height = 24;
  if (!nk_init_fixed(&ui, ui_memory, sizeof(ui_memory), &body_font))
    return -12;
  ui.clip.copy = clipboard_copy;
  ui.clip.paste = clipboard_paste;
  display_width = width;
  display_height = height;
  native_scale = width / 1280.0f;
  if (native_scale > height / 800.0f)
    native_scale = height / 800.0f;
  if (native_scale < 1)
    native_scale = 1;
  if (!gui_canvas_init(display_width, display_height)) {
    console_write("Insufficient memory for desktop buffers.\n");
    return -12;
  }
  gui_canvas_scale(native_scale);
  width = display_width / native_scale;
  height = display_height / native_scale;
  preference_load();
  desktop_load();
  workspace_load();
  defaults_load();
  clipboard_load();
  memset(browser_tabs, 0, sizeof(browser_tabs));
  for (unsigned i = 0; i < WEB_TABS; i++)
    web_reset(i);
  browser_tab_count = browser_active = 0;
  browser_new_tab("about:home");
  file_history_count = file_history_at = 0;
  copy(browser_path, "/", sizeof(browser_path));
  copy(browser_address, "/", sizeof(browser_address));
  file_filter[0] = file_clipboard[0] = task_filter[0] = 0;
  search_subfolders = tasks_paused = false;
  creating_file = renaming_file = creating_folder = context_open = false;
  task_sample_valid = false;
  sample_count = sample_at = selected_pid = 0;
  task_page = 2;
  style();
  for (unsigned i = 0; i < APP_COUNT; i++) {
    apps[i].visible = false;
    apps[i].maximized = false;
    float w = width > 900 ? 820 : width - 48,
          h = height > 800 ? 650 : height - 150;
    apps[i].bounds = nk_rect((width - w) / 2 + (i == SETTINGS ? 0 : 12),
                             (height - h) / 2 - 12, w, h);
  }
  terminal_used = 0;
  terminal_output[0] = terminal_line[0] = 0;
  terminal_pid = 0;
  terminal_focus = editor_focus = 0;
  held_control_app = held_control = -1;
  editor_dirty = false;
  editor[0] = 0;
  copy(editor_path, "/notes", sizeof(editor_path));
  int draft_bytes = fs_read(EDITOR_DRAFT_PATH, &editor_draft, sizeof(editor_draft));
  editor_draft_available = draft_bytes > (int)sizeof(editor_draft.path) &&
      editor_draft.path[0] == '/' && !editor_draft.path[255] &&
      !editor_draft.text[draft_bytes - sizeof(editor_draft.path) - 1];
  editor_draft_written = editor_draft_available;
  editor_draft_tick = clock_ticks();
  editor_draft_crc = 0;
  launcher = power_dialog = deleting_file = ending_task = removing_package =
      editor_discard = false;
  close_editor_after_discard = exit_after_discard = false;
  escape_requested = save_requested = false;
  preferences_dirty = theme_dirty = false;
  running = true;
  music_focus = false;
  music_play_requested = false;
  music_result = 0;
  graphics_console_capture(terminal_character);
  mouse_interrupt_poll(true);
  user_tty_external(true);
  update_status();
  browse("/");
  desktop_session_load();
  refresh_packages();
  calculator_focus = true;
  active_app = -1;
  focused_client = -1;
  client_enable(true);
  focus_request = -1;
  notification_until = 0;
  console_write("Magenta graphical desktop ready\n");
  uint32_t last_frame = clock_ticks() - 3;
  last_status = last_save = clock_ticks();
  while (running) {
    for (unsigned i = 0; i < 4; i++)
      devices_poll();
    xhci_service_ports();
    unsigned current_disk;
    if (fs_secondary_path(browser_path, &current_disk, NULL) &&
        fs_secondary_state(current_disk) < 1) {
      browse("/");
      notify("Drive disconnected");
    }
    net_poll();
    user_poll();
    user_yield_kernel();
    poll_processes();
    if (download_attention) {
      download_attention = false;
      showing_downloads = true;
      open_app(FILES);
    }
    if ((uint32_t)(clock_ticks() - last_status) >= 100) {
      update_status();
      last_status = clock_ticks();
    }
    if ((uint32_t)(clock_ticks() - last_frame) >= 3) {
      workspace_apply();
      inputs();
      if (browser_load_requested)
        browser_load();
      if (theme_dirty)
        style();
      nk_set_user_data(&ui, nk_handle_id(0));
      if (desktop.top_bar)
        top_panel();
      desktop_shortcuts();
      desktop_context_input();
      for (unsigned i = 0; i < APP_COUNT; i++)
        application(i);
      client_windows();
      nk_set_user_data(&ui, nk_handle_id(0));
      dock();
      launcher_view();
      shortcut_menu();
      if (escape_requested) {
        if (context_open)
          context_open = false;
        else if (creating_file || renaming_file)
          creating_file = renaming_file = false;
        else if (deleting_file || ending_task || removing_package)
          deleting_file = ending_task = removing_package = false;
        else if (launcher)
          launcher = false;
        else if (power_dialog)
          power_dialog = false;
        else if (editor_dirty) {
          editor_discard = true;
          exit_after_discard = true;
        }
        escape_requested = false;
      }
      modal_dialogs();
      if (focus_request >= 0) {
        nk_window_set_focus(&ui, app_names[focus_request]);
        focus_request = -1;
      }
      if (client_focus_request >= 0) {
        char name[96];
        client_name(client_focus_request, name);
        nk_window_set_focus(&ui, name);
        focused_client = client_focus_request;
        active_app = -1;
        client_focus_request = -1;
      }
      if (save_requested) {
        save_editor();
        save_requested = false;
      }
      editor_recovery_poll();
      if (gui_canvas_changed(&ui, preferences.blur)) {
        gui_canvas_begin();
        for (unsigned i = 0; i < APP_COUNT; i++)
          if (apps[i].visible && workspaces.homes[i] == workspaces.current &&
              !nk_window_is_collapsed(&ui, app_names[i]))
            gui_canvas_shadow(apps[i].bounds);
        gui_canvas_commands(&ui, preferences.blur);
      }
      int x, y;
      uint8_t buttons;
      mouse_state(&x, &y, &buttons);
      gui_canvas_cursor(x, y);
      gui_canvas_present();
      nk_clear(&ui);
      if (picture_load_requested) {
        picture_load_requested = false;
        image_load(&picture, picture_path);
      }
      if (music_play_requested) {
        music_play_requested = false;
        music_result = audio_play(music_path);
      }
      if (clipboard_dirty && (uint32_t)(clock_ticks() - clipboard_last_attempt) >= 50)
        clipboard_save();
      if (workspace_dirty && (uint32_t)(clock_ticks() - workspace_last_save) >= 50)
        workspace_save();
      if (resume_desktop && (uint32_t)(clock_ticks() - session_last_save) >= 200)
        desktop_session_save();
      last_frame = clock_ticks();
      if (preferences_dirty && !(buttons & 1) &&
          (uint32_t)(clock_ticks() - last_save) >= 50)
        preference_save(false);
    }
    __asm__ volatile("hlt");
  }
  if (terminal_pid)
    user_tty_input(3);
  poll_processes();
  client_enable(false);
  user_tty_external(false);
  clipboard_save();
  if (workspace_dirty) workspace_save();
  if (resume_desktop) desktop_session_save();
  if (preferences_dirty)
    preference_save(false);
  graphics_console_capture(NULL);
  mouse_interrupt_poll(false);
  image_release(&picture);
  games_ui_save();
  nk_free(&ui);
  console_clear();
  return 0;
}
