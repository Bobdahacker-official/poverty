#include "font.h"
#include "gui_canvas.h"
#include "gui_style.h"
#include "system.h"

enum { TERMINAL, FILES, EDITOR, TASKS, NETWORK, PACKAGES, SETTINGS, APP_COUNT };
static struct nk_context ui;
static uint8_t ui_memory[4 * 1024 * 1024];
static struct nk_user_font body_font, heading_font;
static unsigned width, height;
static const char *app_names[APP_COUNT] = {"Terminal", "Files",   "Text editor",
                                           "Tasks",    "Network", "Packages",
                                           "Settings"};
static const char *app_summaries[APP_COUNT] = {
    "Run programs and shell commands",
    "Browse your disks and documents",
    "Write, edit and save text",
    "Inspect and manage processes",
    "Configure IPv4 and watch traffic",
    "Install and update applications",
    "Make Magenta your own"};
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
static bool editor_dirty, editor_discard;
static char browser_path[256] = "/", browser_address[256] = "/",
            file_filter[64], new_name[64];
static struct file_item {
  char name[256];
  uint64_t size;
  bool directory;
} file_items[64];
static unsigned file_count, file_page;
static int selected_file = -1;
static bool file_more, creating_file, deleting_file;
static struct task_info task_items[256];
static unsigned task_slots;
static int selected_pid, task_priority = 16;
static bool ending_task;
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
static char launcher_search[64];

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
  preferences = (struct preferences){0x4d475549, 2, 3, 0, 2, 238, 1, 100, 0};
  struct preferences saved;
  if (fs_read("gui-style", &saved, sizeof(saved)) == (int)sizeof(saved) &&
      saved.magic == preferences.magic &&
      (saved.version == 1 || saved.version == 2) && saved.theme < 4 &&
      saved.accent < 5 && saved.wallpaper < 3 && saved.alpha >= 195 &&
      saved.alpha <= 255 && saved.blur <= 1 && saved.pointer_speed >= 25 &&
      saved.pointer_speed <= 250 &&
      saved.checksum == crc32(&saved, offsetof(struct preferences, checksum))) {
    preferences = saved;
    if (saved.version == 1) {
      preferences.version = 2;
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
  ui.style.window.rounding = 10;
  ui.style.window.border = 1;
  ui.style.window.border_color = alpha_color(p->surface, 210);
  ui.style.window.padding = nk_vec2(16, 14);
  ui.style.window.spacing = nk_vec2(10, 10);
  ui.style.window.group_padding = nk_vec2(12, 12);
  ui.style.window.header.normal = ui.style.window.header.hover =
      ui.style.window.header.active =
          nk_style_item_color(alpha_color(p->mantle, 250));
  ui.style.window.header.padding = nk_vec2(14, 12);
  ui.style.window.header.label_padding = nk_vec2(6, 2);
  ui.style.window.header.label_normal = ui.style.window.header.label_hover =
      ui.style.window.header.label_active = text;
  ui.style.window.header.close_button.rounding = 8;
  ui.style.window.header.minimize_button.rounding = 8;
  ui.style.button.rounding = 8;
  ui.style.button.padding = nk_vec2(12, 8);
  ui.style.button.border = 1;
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
  ui.style.selectable.padding = nk_vec2(10, 8);
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
  mouse_configure(width, height, preferences.pointer_speed);
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
static void clipboard_copy(nk_handle handle, const char *text, int length) {
  (void)handle;
  if (length < 0)
    return;
  unsigned n = length;
  if (n >= sizeof(clipboard))
    n = sizeof(clipboard) - 1;
  memcpy(clipboard, text, n);
  clipboard[n] = 0;
}
static void clipboard_paste(nk_handle handle, struct nk_text_edit *edit) {
  (void)handle;
  nk_textedit_paste(edit, clipboard, strlen(clipboard));
}
static void open_app(unsigned id) {
  if (id >= APP_COUNT)
    return;
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
  task_count = 0;
  task_slots = 0;
  for (unsigned i = 0; i < 256; i++) {
    struct task_info info;
    if (user_task_info(i, &info)) {
      task_items[task_slots++] = info;
      if (info.state != 6)
        task_count++;
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
  if (a + b + 2 > 256)
    return false;
  memcpy(out, parent, a);
  if (!a || out[a - 1] != '/')
    out[a++] = '/';
  memcpy(out + a, name, b + 1);
  return true;
}
static void refresh_files(void) {
  file_count = 0;
  file_more = false;
  selected_file = -1;
  unsigned found = 0;
  for (unsigned index = 0; index < 65536; index++) {
    struct file_item item;
    int r =
        fs_browse(browser_path, index, item.name, &item.size, &item.directory);
    if (r == -2)
      break;
    if (r) {
      notify("Unable to read this folder");
      break;
    }
    if (!contains(item.name, file_filter))
      continue;
    if (found++ < file_page)
      continue;
    if (file_count == 64) {
      file_more = true;
      break;
    }
    file_items[file_count++] = item;
  }
}
static void browse(const char *path) {
  int r = fs_directory(path);
  if (r) {
    notify("That folder is unavailable");
    return;
  }
  copy(browser_path, path, sizeof(browser_path));
  copy(browser_address, path, sizeof(browser_address));
  file_page = 0;
  refresh_files();
}
static void open_file(unsigned index) {
  if (index >= file_count)
    return;
  char path[256];
  if (!join_path(path, browser_path, file_items[index].name)) {
    notify("The path is too long");
    return;
  }
  if (file_items[index].directory) {
    browse(path);
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
  if (n >= 4 && contents[0] == 0x7f && contents[1] == 'E' &&
      contents[2] == 'L' && contents[3] == 'F') {
    int pid = user_spawn(path, "", false);
    notify(pid < 0 ? "Program launch failed" : "Program started");
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
  editor_dirty = false;
  open_app(EDITOR);
}
static void save_editor(void) {
  int n = fs_write(editor_path, editor, strlen(editor));
  if (n >= 0) {
    editor_dirty = false;
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
static void shell_submit(void) {
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
static void file_contents_view(void) {
  float toolbar[] = {0.1f, 0.65f, 0.12f, 0.13f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 4, toolbar);
  if (nk_button_symbol(&ui, NK_SYMBOL_CHEVRON_LEFT)) {
    char parent[256];
    copy(parent, browser_path, sizeof(parent));
    unsigned n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
      parent[--n] = 0;
    while (n > 1 && parent[n - 1] != '/')
      parent[--n] = 0;
    if (n > 1)
      parent[n - 1] = 0;
    browse(parent);
  }
  nk_flags entered = nk_edit_string_zero_terminated(
      &ui, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, browser_address,
      sizeof(browser_address), nk_filter_default);
  bool go = nk_button_label(&ui, "Go");
  if ((entered & NK_EDIT_COMMITTED) || go)
    browse(browser_address);
  if (nk_button_symbol(&ui, NK_SYMBOL_CIRCLE_OUTLINE))
    refresh_files();
  nk_layout_row_dynamic(&ui, 32, 3);
  if (nk_button_label(&ui, "New file")) {
    creating_file = !creating_file;
    new_name[0] = 0;
  }
  bool can_open = selected_file >= 0;
  if (!can_open)
    nk_widget_disable_begin(&ui);
  if (nk_button_label(&ui, "Open") && selected_file >= 0)
    open_file(selected_file);
  if (!can_open)
    nk_widget_disable_end(&ui);
  bool can_delete = selected_file >= 0 && !file_items[selected_file].directory;
  if (!can_delete)
    nk_widget_disable_begin(&ui);
  if (nk_button_label(&ui, "Delete") && selected_file >= 0 &&
      !file_items[selected_file].directory)
    deleting_file = true;
  if (!can_delete)
    nk_widget_disable_end(&ui);
  if (creating_file) {
    nk_layout_row_dynamic(&ui, 32, 3);
    nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, new_name,
                                   sizeof(new_name), nk_filter_default);
    if (accent_button("Create")) {
      char path[256];
      if (!*new_name || contains(new_name, "/") ||
          !join_path(path, browser_path, new_name))
        notify("Enter a file name");
      else if (fs_size(path) >= 0)
        notify("A file with that name already exists");
      else if (fs_write(path, "", 0) < 0)
        notify("File creation failed");
      else {
        creating_file = false;
        refresh_files();
        notify("File created");
      }
    }
    if (nk_button_label(&ui, "Cancel"))
      creating_file = false;
  }
  nk_layout_row_dynamic(&ui, 32, 1);
  char old_filter[64];
  copy(old_filter, file_filter, sizeof(old_filter));
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, file_filter,
                                 sizeof(file_filter), nk_filter_default);
  if (!str_eq(old_filter, file_filter)) {
    file_page = 0;
    refresh_files();
  }
  float area =
      nk_window_get_content_region(&ui).h - (creating_file ? 202 : 158);
  if (area < 80)
    area = 80;
  nk_layout_row_dynamic(&ui, area, 1);
  if (nk_group_begin(&ui, "Folder contents", 0)) {
    nk_layout_row_dynamic(&ui, 24, 2);
    caption("NAME");
    caption("SIZE / TYPE");
    for (unsigned i = 0; i < file_count; i++) {
      float row[] = {0.72f, 0.28f};
      nk_layout_row(&ui, NK_DYNAMIC, 34, 2, row);
      char label[280];
      copy(label, file_items[i].directory ? ">  " : "   ", sizeof(label));
      append(label, file_items[i].name, sizeof(label));
      nk_bool selected = selected_file == (int)i;
      if (nk_selectable_label(&ui, label, NK_TEXT_LEFT, &selected))
        selected_file = i;
      if (file_items[i].directory)
        caption("Folder");
      else {
        char size[32], label[64];
        number(size, file_items[i].size);
        copy(label, size, sizeof(label));
        append(label, " bytes", sizeof(label));
        caption(label);
      }
    }
    if (!file_count) {
      nk_layout_row_dynamic(&ui, 32, 1);
      caption(*file_filter ? "No matching files" : "This folder is empty");
    }
    nk_group_end(&ui);
  }
  nk_layout_row_dynamic(&ui, 30, 3);
  if (nk_button_label(&ui, "Previous") && file_page >= 64) {
    file_page -= 64;
    refresh_files();
  }
  char page[32], label[64] = "Page ";
  number(page, file_page / 64 + 1);
  append(label, page, sizeof(label));
  caption(label);
  if (nk_button_label(&ui, "Next") && file_more) {
    file_page += 64;
    refresh_files();
  }
}
static void files_view(void) {
  float columns[] = {0.22f, 0.78f};
  nk_layout_row(&ui, NK_DYNAMIC, nk_window_get_content_region(&ui).h, 2,
                columns);
  if (nk_group_begin(&ui, "File locations", NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_row_dynamic(&ui, 30, 1);
    caption("LOCATIONS");
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "Home")) {
      file_filter[0] = 0;
      browse("/");
    }
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "Applications"))
      launcher = true;
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "Storage")) {
      settings_page = 2;
      open_app(SETTINGS);
    }
    nk_layout_row_dynamic(&ui, 30, 1);
    caption("QUICK ACTIONS");
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "New document")) {
      creating_file = true;
      new_name[0] = 0;
    }
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "Refresh"))
      refresh_files();
    nk_layout_row_dynamic(&ui, 40, 1);
    if (nk_button_label(&ui, "Packages"))
      open_app(PACKAGES);
    nk_layout_row_dynamic(&ui, 36, 1);
    caption(fs_mounted_disk() < 0 ? "Session storage" : "Persistent disk");
    nk_group_end(&ui);
  }
  if (nk_group_begin(&ui, "File browser", 0)) {
    file_contents_view();
    nk_group_end(&ui);
  }
}
static void editor_view(void) {
  float row[] = {0.64f, 0.18f, 0.18f};
  nk_layout_row(&ui, NK_DYNAMIC, 34, 3, row);
  char previous[256];
  copy(previous, editor_path, sizeof(previous));
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, editor_path,
                                 sizeof(editor_path), nk_filter_default);
  if (!str_eq(previous, editor_path))
    editor_dirty = true;
  if (accent_button("Save"))
    save_requested = true;
  if (nk_button_label(&ui, "New")) {
    if (editor_dirty)
      editor_discard = true;
    else {
      editor[0] = 0;
      copy(editor_path, "/notes", sizeof(editor_path));
      editor_focus = 1;
    }
  }
  float area = nk_window_get_content_region(&ui).h - 82;
  if (area < 70)
    area = 70;
  nk_layout_row_dynamic(&ui, area, 1);
  if (editor_focus) {
    nk_edit_focus(&ui, NK_EDIT_ALWAYS_INSERT_MODE);
    editor_focus = 0;
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
static void tasks_view(void) {
  nk_layout_row_dynamic(&ui, 30, 3);
  label_value("Processes: ", task_count, "");
  label_value("Memory: ", system_status.allocated_kib, " KiB");
  if (nk_button_label(&ui, "Refresh"))
    update_status();
  float area = nk_window_get_content_region(&ui).h - 124;
  if (area < 80)
    area = 80;
  nk_layout_row_dynamic(&ui, area, 1);
  if (nk_group_begin(&ui, "Process list", 0)) {
    float columns[] = {0.36f, 0.12f, 0.20f, 0.17f, 0.15f};
    nk_layout_row(&ui, NK_DYNAMIC, 24, 5, columns);
    caption("Application");
    caption("PID");
    caption("State");
    caption("Memory");
    caption("Priority");
    const char *states[] = {"Empty", "Ready",   "Running", "Sleeping",
                            "Input", "Waiting", "Exited"};
    for (unsigned i = 0; i < task_slots; i++) {
      struct task_info *t = &task_items[i];
      nk_layout_row(&ui, NK_DYNAMIC, 30, 5, columns);
      nk_bool selected = selected_pid == t->pid;
      if (nk_selectable_label(&ui, t->name, NK_TEXT_LEFT, &selected)) {
        selected_pid = t->pid;
        task_priority = t->priority;
      }
      label_value("", t->pid, "");
      caption(t->state < 7 ? states[t->state] : "Unknown");
      label_value("", t->pages * 4, " KiB");
      label_value("", t->priority, "");
    }
    if (!task_slots) {
      nk_layout_row_dynamic(&ui, 30, 1);
      caption("No userspace processes are running");
    }
    nk_group_end(&ui);
  }
  nk_layout_row_dynamic(&ui, 34, 3);
  if (!selected_pid)
    nk_widget_disable_begin(&ui);
  nk_property_int(&ui, "Priority", 0, &task_priority, 31, 1, 0.3f);
  if (nk_button_label(&ui, "Apply priority") && selected_pid) {
    int r = user_priority(selected_pid, task_priority);
    notify(r < 0 ? "Priority update failed" : "Priority updated");
    update_status();
  }
  if (nk_button_label(&ui, "End process") && selected_pid)
    ending_task = true;
  if (!selected_pid)
    nk_widget_disable_end(&ui);
  nk_layout_row_dynamic(&ui, 24, 1);
  caption("Lower priority numbers receive earlier CPU service.");
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
static void network_view(void) {
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
  nk_layout_row_dynamic(&ui, 32, 3);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, package_filter,
                                 sizeof(package_filter), nk_filter_default);
  nk_checkbox_label(&ui, "Installed only", &installed_only);
  if (nk_button_label(&ui, "Refresh"))
    refresh_packages();
  float ratios[] = {0.43f, 0.57f};
  nk_layout_row(&ui, NK_DYNAMIC, 250, 2, ratios);
  if (nk_group_begin(&ui, "Applications", 0)) {
    for (unsigned i = 0; i < package_count; i++) {
      struct package_item *p = &package_items[i];
      if (!contains(p->name, package_filter) ||
          (installed_only && !p->installed))
        continue;
      nk_layout_row_dynamic(&ui, 36, 1);
      nk_bool selected = selected_package == (int)i;
      char label[96];
      copy(label, p->name, sizeof(label));
      append(label, p->installed ? "   Installed" : "   Available",
             sizeof(label));
      if (nk_selectable_label(&ui, label, NK_TEXT_LEFT, &selected)) {
        selected_package = i;
        char command[96] = "pkg query ";
        append(command, p->name, sizeof(command));
        capture(command, package_details, sizeof(package_details));
      }
    }
    nk_group_end(&ui);
  }
  if (nk_group_begin(&ui, "Package details", 0)) {
    if (package_count) {
      struct package_item *p = &package_items[selected_package];
      heading(p->name);
      nk_layout_row_dynamic(&ui, 24, 1);
      caption(p->version);
      nk_layout_row_dynamic(&ui, 55, 1);
      nk_label_wrap(&ui, package_summary(p->name));
      bool installed = p->installed;
      nk_layout_row_dynamic(&ui, 34, installed ? 2 : 1);
      if (accent_button(installed ? "Upgrade" : "Install"))
        package_action(installed ? "upgrade" : "install");
      if (installed && nk_button_label(&ui, "Remove"))
        removing_package = true;
    }
    nk_group_end(&ui);
  }
  nk_layout_row_dynamic(&ui, 60, 1);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                                 package_details, sizeof(package_details),
                                 nk_filter_default);
  nk_layout_row_dynamic(&ui, 24, 1);
  caption("Install from an HTTP repository");
  float fields[] = {0.38f, 0.43f, 0.19f};
  nk_layout_row(&ui, NK_DYNAMIC, 32, 3, fields);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, download_host,
                                 sizeof(download_host), nk_filter_default);
  nk_edit_string_zero_terminated(&ui, NK_EDIT_FIELD, download_path,
                                 sizeof(download_path), nk_filter_default);
  if (nk_button_label(&ui, "Download")) {
    if (!*download_host || *download_path != '/')
      notify("Enter a host and /manifest path");
    else {
      char command[384] = "pkg fetch ";
      append(command, download_host, sizeof(command));
      append(command, " ", sizeof(command));
      append(command, download_path, sizeof(command));
      capture(command, package_result, sizeof(package_result));
      refresh_packages();
      refresh_files();
      notify(*package_result ? package_result : "Package operation completed");
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
  const char *wallpapers[] = {"Ribbon", "Graphite", "Aurora"};
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
static void settings_view(void) {
  float columns[] = {0.25f, 0.75f};
  float area = nk_window_get_content_region(&ui).h;
  nk_layout_row(&ui, NK_DYNAMIC, area, 2, columns);
  if (nk_group_begin(&ui, "Settings navigation", NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_row_dynamic(&ui, 28, 1);
    caption("MAGENTA");
    const char *pages[] = {"Appearance", "System", "Storage", "Pointer",
                           "About"};
    for (unsigned i = 0; i < 5; i++) {
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
        mouse_configure(width, height, speed);
        preferences_dirty = true;
      }
      nk_layout_row_dynamic(&ui, 28, 1);
      label_value("Sensitivity: ", preferences.pointer_speed, "%");
      nk_layout_row_dynamic(&ui, 34, 1);
      if (nk_button_label(&ui, "Reset pointer speed")) {
        preferences.pointer_speed = 100;
        mouse_configure(width, height, 100);
        preferences_dirty = true;
      }
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
  struct nk_rect r = apps[id].bounds;
  return nk_rect(r.x + r.w - 120 + control * 40, r.y + 4, 36, 36);
}
static int window_controls(unsigned id) {
  struct nk_command_buffer *canvas = nk_window_get_canvas(&ui);
  struct nk_rect clip = canvas->clip;
  nk_push_scissor(canvas, nk_rect(0, 0, width, height));
  gui_canvas_gloss(
      canvas,
      nk_rect(apps[id].bounds.x, apps[id].bounds.y, apps[id].bounds.w, 44), 22);
  bool active = nk_window_is_active(&ui, app_names[id]);
  int action = -1;
  for (unsigned control = 0; control < 3; control++) {
    struct nk_rect r = window_control_bounds(id, control);
    bool hover = active && nk_input_is_mouse_hovering_rect(&ui.input, r);
    if (hover && nk_input_is_mouse_pressed(&ui.input, NK_BUTTON_LEFT)) {
      held_control_app = id;
      held_control = control;
    }
    bool held = held_control_app == (int)id && held_control == (int)control;
    if (held && nk_input_is_mouse_released(&ui.input, NK_BUTTON_LEFT)) {
      if (hover)
        action = control;
      held_control_app = held_control = -1;
    }
    static const uint32_t traffic[] = {0xf2c252, 0x49c68a, 0xf26378};
    struct nk_color fill =
        hover ? alpha_color(palette()->surface, 180) : nk_rgba(0, 0, 0, 0);
    struct nk_color ink =
        hover && control == 2 ? nk_rgb(255, 255, 255) : rgb(palette()->text);
    nk_fill_rect(canvas, r, 8, fill);
    nk_fill_circle(canvas, nk_rect(r.x + 7, r.y + 7, 22, 22),
                   active ? rgb(traffic[control]) : rgb(palette()->muted));
    if (held && hover)
      nk_stroke_rect(canvas, r, 8, 2, rgb(accent()));
    float x = r.x + 12, y = r.y + 12;
    if (control == 0)
      nk_stroke_line(canvas, x, y + 8, x + 12, y + 8, 2, ink);
    else if (control == 1) {
      nk_stroke_rect(canvas, nk_rect(x, y, 12, 12), 1, 2, ink);
      if (apps[id].maximized)
        nk_stroke_rect(canvas, nk_rect(x + 3, y - 3, 12, 12), 1, 1, ink);
    } else {
      nk_stroke_line(canvas, x, y, x + 12, y + 12, 2, ink);
      nk_stroke_line(canvas, x + 12, y, x, y + 12, 2, ink);
    }
  }
  nk_push_scissor(canvas, clip);
  return action;
}
static void application(unsigned id) {
  if (!apps[id].visible)
    return;
  nk_set_user_data(&ui, nk_handle_id(id + 1));
  nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                   NK_WINDOW_TITLE;
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
    if (nk_window_is_active(&ui, app_names[id]))
      active_app = id;
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
    if (id == EDITOR && editor_dirty) {
      open_app(EDITOR);
      editor_discard = true;
      close_editor_after_discard = true;
    }
    if (id == TERMINAL && terminal_pid)
      user_tty_input(3);
    if (!apps[id].visible)
      notify("Window closed");
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
    nk_layout_row_template_push_static(&ui, 94);
    nk_layout_row_template_push_dynamic(&ui);
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
    if (nk_button_label(&ui, "Desktop")) {
      for (unsigned i = 0; i < APP_COUNT; i++)
        if (apps[i].visible)
          nk_window_collapse(&ui, app_names[i], NK_MINIMIZED);
    }
    nk_spacer(&ui);
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
  unsigned kind = id < 0 ? 7 : (unsigned)id;
  struct nk_color base = rgb(colors[kind]);
  nk_fill_rect(canvas, nk_rect(r.x, r.y + 3, r.w, r.h), 10,
               nk_rgba(0, 0, 0, 65));
  nk_fill_rect(canvas, r, 10, base);
  gui_canvas_gloss(canvas, r, 90);
  nk_stroke_line(canvas, r.x + 10, r.y + r.h - 3, r.x + r.w - 10, r.y + r.h - 3,
                 1, nk_rgba(0, 0, 0, 80));
  gui_canvas_icon(canvas, r, kind, nk_rgb(250, 250, 255));
}
static void desktop_shortcuts(void) {
  nk_style_push_style_item(&ui, &ui.style.window.fixed_background,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  if (nk_begin(&ui, "Desktop shortcuts", nk_rect(10, 58, 100, 210),
               NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR)) {
    for (unsigned i = 0; i < 2; i++) {
      nk_layout_row_dynamic(&ui, 54, 1);
      struct nk_rect r = nk_widget_bounds(&ui);
      if (nk_button_label(&ui, "")) {
        if (!i)
          browse("/");
        open_app(i ? TERMINAL : FILES);
      }
      app_badge(nk_window_get_canvas(&ui),
                nk_rect(r.x + (r.w - 44) / 2, r.y + 4, 44, 44),
                i ? TERMINAL : FILES);
      nk_layout_row_dynamic(&ui, 24, 1);
      nk_label_colored(&ui, i ? "Terminal" : "Home", NK_TEXT_CENTERED,
                       nk_rgb(245, 249, 255));
    }
  }
  nk_end(&ui);
  nk_style_pop_style_item(&ui);
}
static void dock(void) {
  float dock_width = APP_COUNT * 58 + 58, spacing = 58;
  if (dock_width > width - 28) {
    dock_width = width - 28;
    spacing = (dock_width - 22) / (APP_COUNT + 1);
  }
  nk_style_push_vec2(&ui, &ui.style.window.padding, nk_vec2(10, 8));
  if (nk_begin(&ui, "Application dock",
               nk_rect((width - dock_width) / 2, height - 82, dock_width, 66),
               NK_WINDOW_NO_SCROLLBAR)) {
    gui_canvas_gloss(nk_window_get_canvas(&ui), nk_window_get_bounds(&ui), 46);
    nk_layout_row_static(&ui, 44, spacing - 12, APP_COUNT + 1);
    for (unsigned slot = 0; slot < APP_COUNT + 1; slot++) {
      int id = slot ? ((int)slot - 1) : -1;
      struct nk_rect bounds = nk_widget_bounds(&ui);
      bool selected = id >= 0 && apps[id].visible && active_app == id;
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
               NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE)) {
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
    nk_layout_row_dynamic(&ui, 112, 4);
    unsigned matches = 0;
    for (unsigned i = 0; i < APP_COUNT; i++)
      if ((launcher_category == 0 ||
           (launcher_category == 1 && (i == FILES || i == EDITOR)) ||
           (launcher_category == 2 &&
            (i == TERMINAL || i == TASKS || i == SETTINGS)) ||
           (launcher_category == 3 && (i == NETWORK || i == PACKAGES)) ||
           (launcher_category == 4 && i <= EDITOR)) &&
          (contains(app_names[i], launcher_search) ||
           contains(app_summaries[i], launcher_search))) {
        matches++;
        struct nk_rect r = nk_widget_bounds(&ui);
        if (nk_button_label(&ui, ""))
          open_app(i);
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
    if (!matches) {
      nk_layout_row_dynamic(&ui, 44, 1);
      caption("No applications match your search.");
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
static void notification_view(void) {
  if ((int32_t)(notification_until - clock_ticks()) <= 0)
    return;
  if (nk_begin(&ui, "Notification", nk_rect(width - 340, 70, 320, 94),
               NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND |
                   NK_WINDOW_NO_INPUT)) {
    nk_layout_row_dynamic(&ui, 20, 1);
    nk_label_colored(&ui, "Magenta", NK_TEXT_LEFT, rgb(accent()));
    nk_layout_row_dynamic(&ui, 42, 1);
    nk_label_wrap(&ui, notification);
  }
  nk_end(&ui);
}
static void modal_dialogs(void) {
  const char *name = power_dialog       ? "Session"
                     : deleting_file    ? "Delete file"
                     : ending_task      ? "End process"
                     : removing_package ? "Remove package"
                     : editor_discard   ? "Discard edits"
                                        : NULL;
  if (!name)
    return;
  if (nk_begin(&ui, name,
               nk_rect((width - 400) / 2, (height - 200) / 2, 400, 200),
               NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_row_dynamic(&ui, 44, 1);
    nk_label_wrap(
        &ui,
        power_dialog ? "Choose a session action."
        : editor_discard
            ? "Discard the unsaved changes in this document?"
            : "Confirm this action. Saved changes take effect immediately.");
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
      if (deleting_file && selected_file >= 0) {
        char path[256];
        if (join_path(path, browser_path, file_items[selected_file].name)) {
          int r = fs_remove(path);
          notify(r < 0 ? "File deletion failed" : "File deleted");
          refresh_files();
        }
      }
      if (ending_task && selected_pid) {
        int r = user_kill(selected_pid, 9);
        notify(r < 0 ? "Unable to end this process" : "Process terminated");
        update_status();
      }
      if (removing_package)
        package_action("remove");
      deleting_file = ending_task = removing_package = editor_discard = false;
    }
    if (nk_button_label(&ui, "Cancel"))
      power_dialog = deleting_file = ending_task = removing_package =
          editor_discard = close_editor_after_discard = exit_after_discard =
              false;
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
  nk_input_motion(&ui, x, y);
  nk_input_button(&ui, NK_BUTTON_LEFT, x, y, (buttons & 1) != 0);
  nk_input_button(&ui, NK_BUTTON_RIGHT, x, y, (buttons & 2) != 0);
  nk_input_button(&ui, NK_BUTTON_MIDDLE, x, y, (buttons & 4) != 0);
  for (unsigned n = 0; n < 64; n++) {
    char c = console_getchar();
    if (!c)
      break;
    if (c == 27) {
      escape_requested = true;
      continue;
    }
    int shortcut = c == 20   ? TERMINAL
                   : c == 6  ? FILES
                   : c == 5  ? EDITOR
                   : c == 11 ? TASKS
                   : c == 14 ? NETWORK
                   : c == 16 ? PACKAGES
                   : c == 15 ? SETTINGS
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
  int key = graphics_key_event();
  if (active_app == TERMINAL && (key == 0x48 || key == 0x50) && history_count) {
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
  gui_canvas_init(width, height);
  preference_load();
  style();
  for (unsigned i = 0; i < APP_COUNT; i++) {
    apps[i].visible = false;
    apps[i].maximized = false;
    float w = width > 900 ? 820 : width - 48,
          h = height > 650 ? 570 : height - 158;
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
  launcher = power_dialog = deleting_file = ending_task = removing_package =
      editor_discard = false;
  close_editor_after_discard = exit_after_discard = false;
  escape_requested = save_requested = false;
  preferences_dirty = theme_dirty = false;
  running = true;
  clipboard[0] = 0;
  graphics_console_capture(terminal_character);
  mouse_interrupt_poll(true);
  user_tty_external(true);
  update_status();
  refresh_files();
  refresh_packages();
  active_app = -1;
  focus_request = -1;
  notification_until = 0;
  console_write("Magenta graphical desktop ready\n");
  uint32_t last_frame = clock_ticks() - 2;
  last_status = last_save = clock_ticks();
  while (running) {
    for (unsigned i = 0; i < 64; i++)
      devices_poll();
    net_poll();
    user_poll();
    user_yield_kernel();
    poll_processes();
    if ((uint32_t)(clock_ticks() - last_status) >= 100) {
      update_status();
      last_status = clock_ticks();
    }
    if ((uint32_t)(clock_ticks() - last_frame) >= 2) {
      inputs();
      if (theme_dirty)
        style();
      nk_set_user_data(&ui, nk_handle_id(0));
      top_panel();
      desktop_shortcuts();
      for (unsigned i = 0; i < APP_COUNT; i++)
        application(i);
      nk_set_user_data(&ui, nk_handle_id(0));
      dock();
      launcher_view();
      notification_view();
      if (escape_requested) {
        if (launcher)
          launcher = false;
        else if (power_dialog)
          power_dialog = false;
        else if (editor_dirty) {
          editor_discard = true;
          exit_after_discard = true;
        } else
          running = false;
        escape_requested = false;
      }
      modal_dialogs();
      if (focus_request >= 0) {
        nk_window_set_focus(&ui, app_names[focus_request]);
        focus_request = -1;
      }
      if (save_requested) {
        save_editor();
        save_requested = false;
      }
      if (gui_canvas_changed(&ui, preferences.blur)) {
        gui_canvas_begin();
        for (unsigned i = 0; i < APP_COUNT; i++)
          if (apps[i].visible && !nk_window_is_collapsed(&ui, app_names[i]))
            gui_canvas_shadow(apps[i].bounds);
        gui_canvas_commands(&ui, preferences.blur);
      }
      int x, y;
      uint8_t buttons;
      mouse_state(&x, &y, &buttons);
      gui_canvas_cursor(x, y);
      gui_canvas_present();
      nk_clear(&ui);
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
  user_tty_external(false);
  if (preferences_dirty)
    preference_save(false);
  graphics_console_capture(NULL);
  mouse_interrupt_poll(false);
  nk_free(&ui);
  console_clear();
  return 0;
}
