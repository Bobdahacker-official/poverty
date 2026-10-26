#include "web.h"
#include "../vendor/elk/elk.h"
#include "http.h"
#include "system.h"
struct web_node web_nodes[WEB_NODES];
unsigned web_count;
char web_message[128];
static unsigned char js_memory[262144] __attribute__((aligned(8)));
static struct js *engine;
static char css[16384], scripts[32768];
static char document_url[512];
void web_set_url(const char *url) {
  unsigned n = strlen(url);
  if (n >= sizeof(document_url))
    n = sizeof(document_url) - 1;
  memcpy(document_url, url, n);
  document_url[n] = 0;
}
static int resource(const char *ref, char *out, unsigned size) {
  char url[512];
  if (!http_resolve(document_url, ref, url, sizeof(url)))
    return -1;
  /* Block active mixed content in encrypted documents. */
  if (!__builtin_memcmp(document_url, "https://", 8) &&
      __builtin_memcmp(url, "https://", 8))
    return -1;
  return http_get(url, out, size);
}
static unsigned css_size, script_size;
static bool equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
static bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static void copy_n(char *out, unsigned cap, const char *s, unsigned n) {
  if (n >= cap)
    n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}
static unsigned length(const char *a) { return strlen(a); }
static const char *find(const char *s, const char *needle) {
  unsigned n = length(needle);
  for (; *s; s++) {
    unsigned i = 0;
    while (i < n && s[i] && s[i] == needle[i])
      i++;
    if (i == n)
      return s;
  }
  return NULL;
}
static unsigned number(const char *s, unsigned max) {
  unsigned n = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + *s++ - '0';
    if (n >= max)
      return max;
  }
  return n;
}
static unsigned hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return 255;
}
static uint32_t color(const char *s, uint32_t fallback) {
  if (*s == '#') {
    s++;
    unsigned n = length(s), v = 0;
    if (n != 3 && n != 6)
      return fallback;
    for (unsigned i = 0; i < n; i++) {
      unsigned h = hex(s[i]);
      if (h == 255)
        return fallback;
      v = n == 3 ? (v << 8) | h * 17 : (v << 4) | h;
    }
    return v;
  }
  static const struct {
    const char *name;
    uint32_t rgb;
  } names[] = {{"black", 0},          {"white", 0xffffff},
               {"red", 0xff0000},     {"green", 0x008000},
               {"blue", 0x0000ff},    {"yellow", 0xffff00},
               {"gray", 0x808080},    {"purple", 0x800080},
               {"magenta", 0xff00ff}, {"transparent", 0xffffffff}};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (equal(s, names[i].name))
      return names[i].rgb;
  return fallback;
}
static void declarations(struct web_style *style, const char *s, unsigned len) {
  const char *end = s + len;
  while (s < end) {
    while (s < end && (space(*s) || *s == ';'))
      s++;
    const char *key = s;
    while (s < end && *s != ':' && *s != ';')
      s++;
    if (s == end)
      break;
    if (*s != ':') {
      s++;
      continue;
    }
    const char *keyend = s;
    while (keyend > key && space(keyend[-1]))
      keyend--;
    char name[48], value[128];
    copy_n(name, sizeof(name), key, keyend - key);
    s++;
    while (s < end && space(*s))
      s++;
    const char *val = s;
    while (s < end && *s != ';')
      s++;
    const char *ve = s;
    while (ve > val && space(ve[-1]))
      ve--;
    copy_n(value, sizeof(value), val, ve - val);
    if (equal(name, "color"))
      style->color = color(value, style->color);
    else if (equal(name, "background") || equal(name, "background-color"))
      style->background = color(value, style->background);
    else if (equal(name, "font-size")) {
      unsigned n = number(value, 72);
      if (n >= 8)
        style->size = n;
    } else if (equal(name, "padding"))
      style->padding = number(value, 64);
    else if (equal(name, "margin") || equal(name, "margin-bottom"))
      style->margin = number(value, 64);
    else if (equal(name, "display"))
      style->hidden = equal(value, "none");
    else if (equal(name, "text-align"))
      style->center = equal(value, "center");
  }
}
static bool class_has(const char *classes, const char *name) {
  unsigned n = length(name);
  while (*classes) {
    while (space(*classes))
      classes++;
    const char *start = classes;
    while (*classes && !space(*classes))
      classes++;
    if ((unsigned)(classes - start) == n && !__builtin_memcmp(start, name, n))
      return true;
  }
  return false;
}
static unsigned selector(struct web_node *node, const char *s, unsigned n) {
  while (n && space(*s)) {
    s++;
    n--;
  }
  while (n && space(s[n - 1]))
    n--;
  char text[128];
  copy_n(text, sizeof(text), s, n);
  if (text[0] == '#')
    return equal(node->id, text + 1) ? 100 : 0;
  if (text[0] == '.')
    return class_has(node->classes, text + 1) ? 10 : 0;
  if (equal(text, "*"))
    return 1;
  return equal(node->tag, text) ? 2 : 0;
}
static void styles(void) {
  for (unsigned i = 0; i < web_count; i++) {
    struct web_node *node = &web_nodes[i];
    struct web_style inherited =
        node->parent >= 0
            ? web_nodes[node->parent].style
            : (struct web_style){0x222222, 0xffffffff, 16, 0, 4, false, false};
    node->style = inherited;
    node->style.background = 0xffffffff;
    node->style.padding = 0;
    node->style.margin = 4;
    if (equal(node->tag, "h1"))
      node->style.size = 32;
    if (equal(node->tag, "h2"))
      node->style.size = 24;
    if (equal(node->tag, "a"))
      node->style.color = 0x2463db;
    if (equal(node->tag, "head") || equal(node->tag, "title") ||
        equal(node->tag, "meta") || equal(node->tag, "link"))
      node->style.hidden = true;
    /* Apply increasing specificity; equal specificity follows source order. */
    for (unsigned pass = 1; pass <= 100; pass = pass == 1    ? 2
                                                : pass == 2  ? 10
                                                : pass == 10 ? 100
                                                             : 101) {
      const char *s = css;
      while (*s) {
        const char *open = find(s, "{");
        if (!open)
          break;
        const char *close = find(open + 1, "}");
        if (!close)
          break;
        const char *sel = s;
        while (sel < open) {
          const char *end = sel;
          while (end < open && *end != ',')
            end++;
          if (selector(node, sel, end - sel) == pass)
            declarations(&node->style, open + 1, close - open - 1);
          sel = end < open ? end + 1 : open;
        }
        s = close + 1;
      }
    }
    declarations(&node->style, node->inline_style, length(node->inline_style));
    if (node->parent >= 0 && web_nodes[node->parent].style.hidden)
      node->style.hidden = true;
  }
}
static jsval_t document_write(struct js *js, jsval_t *args, int count) {
  for (int i = 0; i < count && web_count < WEB_NODES; i++) {
    struct web_node *n = &web_nodes[web_count++];
    memset(n, 0, sizeof(*n));
    n->parent = -1;
    copy_n(n->tag, sizeof(n->tag), "p", 1);
    size_t size = 0;
    char *s = js_getstr(js, args[i], &size);
    if (s)
      copy_n(n->text, sizeof(n->text), s, size);
    else {
      s = (char *)js_str(js, args[i]);
      copy_n(n->text, sizeof(n->text), s, length(s));
    }
    n->style = (struct web_style){0x222222, 0xffffffff, 16, 0, 4, false, false};
  }
  return js_mkundef();
}
static jsval_t document_get(struct js *js, jsval_t *args, int count) {
  if (count != 1 || js_type(args[0]) != JS_STR)
    return js_mknull();
  size_t len;
  const char *id = js_getstr(js, args[0], &len);
  for (unsigned i = 0; i < web_count; i++)
    if (length(web_nodes[i].id) == len &&
        !__builtin_memcmp(id, web_nodes[i].id, len))
      return web_nodes[i].object;
  return js_mknull();
}
static void sync_dom(void) {
  for (unsigned i = 0; i < web_count; i++) {
    struct web_node *n = &web_nodes[i];
    if (!n->object)
      continue;
    jsval_t v = js_get(engine, n->object, "textContent");
    size_t len;
    char *s = js_getstr(engine, v, &len);
    if (s)
      copy_n(n->text, sizeof(n->text), s, len);
  }
}
static void evaluate(const char *code, unsigned n) {
  js_budget(50000);
  jsval_t value = js_eval(engine, code, n);
  if (js_type(value) == JS_ERR) {
    const char *s = js_str(engine, value);
    copy_n(web_message, sizeof(web_message), s, length(s));
  }
  sync_dom();
}
static void decode(char *out, unsigned cap, const char *s, unsigned n) {
  unsigned o = 0;
  for (unsigned i = 0; i < n && o + 1 < cap; i++) {
    if (s[i] == '&') {
      static const char *names[] = {"&amp;",  "&lt;",   "&gt;",
                                    "&quot;", "&nbsp;", "&#39;"};
      static const char values[] = {'&', '<', '>', '"', ' ', '\''};
      bool found = false;
      for (unsigned e = 0; e < 6; e++) {
        unsigned len = length(names[e]);
        if (len <= n - i && !__builtin_memcmp(s + i, names[e], len)) {
          out[o++] = values[e];
          i += len - 1;
          found = true;
          break;
        }
      }
      if (found)
        continue;
    }
    char c = space(s[i]) ? ' ' : s[i];
    if (c == ' ' && (!o || out[o - 1] == ' '))
      continue;
    out[o++] = c;
  }
  out[o] = 0;
}
void web_parse(const char *html) {
  memset(web_nodes, 0, sizeof(web_nodes));
  web_count = css_size = script_size = 0;
  web_message[0] = css[0] = scripts[0] = 0;
  int parent = -1;
  const char *s = html;
  while (*s && web_count < WEB_NODES) {
    if (*s != '<') {
      const char *end = s;
      while (*end && *end != '<')
        end++;
      if (parent >= 0) {
        struct web_node *node = &web_nodes[parent];
        unsigned used = length(node->text);
        decode(node->text + used, sizeof(node->text) - used, s, end - s);
      }
      s = end;
      continue;
    }
    if (!__builtin_memcmp(s, "<!--", 4)) {
      const char *end = find(s + 4, "-->");
      if (!end)
        break;
      s = end + 3;
      continue;
    }
    s++;
    bool closing = *s == '/';
    if (closing)
      s++;
    const char *start = s;
    while (*s && !space(*s) && *s != '>' && *s != '/')
      s++;
    char tag[16];
    copy_n(tag, sizeof(tag), start, s - start);
    for (unsigned i = 0; tag[i]; i++)
      if (tag[i] >= 'A' && tag[i] <= 'Z')
        tag[i] += 32;
    if (closing) {
      while (*s && *s != '>')
        s++;
      if (*s)
        s++;
      int p = parent;
      while (p >= 0 && !equal(web_nodes[p].tag, tag))
        p = web_nodes[p].parent;
      if (p >= 0)
        parent = web_nodes[p].parent;
      continue;
    }
    struct web_node node = {0};
    node.parent = parent;
    copy_n(node.tag, sizeof(node.tag), tag, length(tag));
    while (*s && *s != '>') {
      while (space(*s) || *s == '/')
        s++;
      if (!*s || *s == '>')
        break;
      const char *name = s;
      while (*s && !space(*s) && *s != '=' && *s != '>')
        s++;
      char key[32];
      copy_n(key, sizeof(key), name, s - name);
      while (space(*s))
        s++;
      if (*s != '=')
        continue;
      s++;
      while (space(*s))
        s++;
      char quote = *s == '\'' || *s == '"' ? *s++ : 0;
      const char *value = s;
      while (*s && (quote ? *s != quote : !space(*s) && *s != '>'))
        s++;
      unsigned n = s - value;
      if (equal(key, "id"))
        decode(node.id, sizeof(node.id), value, n);
      if (equal(key, "class"))
        decode(node.classes, sizeof(node.classes), value, n);
      if (equal(key, "href") || equal(key, "src"))
        decode(node.href, sizeof(node.href), value, n);
      if (equal(key, "onclick"))
        decode(node.onclick, sizeof(node.onclick), value, n);
      if (equal(key, "style"))
        decode(node.inline_style, sizeof(node.inline_style), value, n);
      if (quote && *s)
        s++;
    }
    if (*s)
      s++;
    if (equal(tag, "link") && *node.href && css_size + 2 < sizeof(css)) {
      int n = resource(node.href, css + css_size, sizeof(css) - css_size - 1);
      if (n > 0) {
        css_size += n;
        css[css_size++] = '\n';
        css[css_size] = 0;
      }
    }
    if (equal(tag, "script") || equal(tag, "style")) {
      const char *end = find(s, equal(tag, "script") ? "</script" : "</style");
      if (!end)
        break;
      char *buffer = equal(tag, "script") ? scripts : css;
      unsigned *used = equal(tag, "script") ? &script_size : &css_size;
      unsigned cap = equal(tag, "script") ? sizeof(scripts) : sizeof(css);
      if (equal(tag, "script") && *node.href && *used + 2 < cap) {
        int loaded = resource(node.href, buffer + *used, cap - *used - 1);
        if (loaded > 0) {
          *used += loaded;
          buffer[(*used)++] = '\n';
          buffer[*used] = 0;
        }
      }
      unsigned n = end - s;
      if (n > cap - *used - 2)
        n = cap - *used - 2;
      memcpy(buffer + *used, s, n);
      *used += n;
      buffer[(*used)++] = '\n';
      buffer[*used] = 0;
      s = end;
      continue;
    }
    if (!*tag || tag[0] == '!')
      continue;
    web_nodes[web_count] = node;
    if (!equal(tag, "br") && !equal(tag, "hr") && !equal(tag, "img") &&
        !equal(tag, "input") && !equal(tag, "meta") && !equal(tag, "link"))
      parent = web_count;
    web_count++;
  }
  styles();
  engine = js_create(js_memory, sizeof(js_memory));
  js_setmaxcss(engine, 8192);
  js_setgct(engine, sizeof(js_memory));
  jsval_t document = js_mkobj(engine);
  js_set(engine, js_glob(engine), "document", document);
  js_set(engine, document, "write", js_mkfun(document_write));
  js_set(engine, document, "getElementById", js_mkfun(document_get));
  jsval_t console = js_mkobj(engine);
  js_set(engine, js_glob(engine), "console", console);
  js_set(engine, console, "log", js_mkfun(document_write));
  for (unsigned i = 0; i < web_count; i++) {
    char key[16];
    key[0] = 'n';
    unsigned n = i, at = 1;
    do {
      key[at++] = '0' + n % 10;
      n /= 10;
    } while (n);
    key[at] = 0;
    web_nodes[i].object = js_mkobj(engine);
    js_set(engine, document, key, web_nodes[i].object);
    js_set(engine, web_nodes[i].object, "textContent",
           js_mkstr(engine, web_nodes[i].text, length(web_nodes[i].text)));
  }
  if (script_size)
    evaluate(scripts, script_size);
}
void web_click(unsigned node) {
  if (node < web_count && *web_nodes[node].onclick)
    evaluate(web_nodes[node].onclick, length(web_nodes[node].onclick));
}
