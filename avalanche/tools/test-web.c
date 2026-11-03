#include "../src/web.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#undef printf
#undef snprintf
#undef vsnprintf
int fs_read(const char *path, void *out, uint32_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -2;
  size_t n = fread(out, 1, cap, f);
  int extra = fgetc(f);
  fclose(f);
  return extra == EOF ? (int)n : -27;
}
int fs_write(const char *path, const void *data, uint32_t n) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return -5;
  size_t written = fwrite(data, 1, n, f);
  int r = fclose(f);
  return written == n && !r ? (int)n : -5;
}
int fs_remove(const char *path) { return remove(path) ? -5 : 0; }
int http_get(const char *url, char *out, unsigned cap) {
  (void)url;
  (void)out;
  (void)cap;
  abort();
}
bool http_resolve(const char *a, const char *b, char *out, unsigned cap) {
  (void)a;
  (void)b;
  (void)out;
  (void)cap;
  abort();
}
void _putchar(char c) { putchar(c); }
static unsigned node(const char *id) {
  for (unsigned i = 0; i < web_count; i++)
    if (!strcmp(web_nodes[i].id, id))
      return i;
  abort();
}
static void page(const char *url) {
  web_reset(0);
  web_select(0);
  web_set_url(url);
  web_parse("<input id='field' value='initial' "
            "oninput=\"document.getElementById('out').textContent=document."
            "getElementById('field').value;\" "
            "onchange=\"document.getElementById('out').textContent='Committed';\">"
            "<p id='out'>Ready</p><button id='save' "
            "onclick=\"localStorage.setItem('key',document.getElementById('"
            "field').value);\">Save</button>");
  assert(!*web_message);
}
int main(int argc, char **argv) {
  page("https://example.org/a");
  unsigned field = node("field");
  assert(!strcmp(web_nodes[field].value, "initial"));
  strcpy(web_nodes[field].value, "Persistent notes\nSecond line");
  web_input(field, false);
  assert(!strcmp(web_nodes[node("out")].text, "Persistent notes\nSecond line"));
  web_input(field,true);
  assert(!strcmp(web_nodes[node("out")].text,"Committed"));
  web_click(node("save"));
  assert(!*web_message);
  page("https://EXAMPLE.org:443/another-page");
  evaluate(
      "document.getElementById('field').value=localStorage.getItem('key');",
      ~0u);
  assert(!*web_message && !strcmp(web_nodes[node("field")].value,
                                  "Persistent notes\nSecond line"));
  puts("PASS browser input events, JavaScript values and disk persistence "
       "after context reset");
  const char *isolated[] = {"http://example.org/a", "https://other.org/a",
                            "https://example.org:444/a", "file:/notes"};
  for (unsigned i = 0; i < 4; i++) {
    page(isolated[i]);
    evaluate("let result=localStorage.getItem('key');", ~0u);
    assert(!*web_message &&
           js_type(js_get(engine, js_glob(engine), "result")) == JS_NULL);
  }
  puts("PASS browser storage isolates schemes, hosts, ports and local "
       "documents");
  page("https://example.org/");
  evaluate("localStorage.removeItem('key');", ~0u);
  evaluate("let result=localStorage.getItem('key');", ~0u);
  assert(!*web_message &&
         js_type(js_get(engine, js_glob(engine), "result")) == JS_NULL);
  puts("PASS browser storage deletion persists");
  char oversized[4096];
  memset(oversized, 'x', sizeof(oversized));
  jsval_t args[2] = {js_mkstr(engine, "quota", 5),
                     js_mkstr(engine, oversized, sizeof(oversized))};
  assert(js_type(storage_set(engine, args, 2)) == JS_ERR);
  uint8_t digest[32];
  char path[32];
  assert(storage_key(engine, args[0], digest, path));
  assert(fs_write(path, "damaged", 7) == 7);
  args[1] = storage_get(engine, args, 1);
  assert(js_type(args[1]) == JS_ERR);
  assert(fs_remove(path) == 0);
  puts("PASS browser storage rejects oversized values and damaged records");
  assert(argc == 2);
  FILE *source = fopen(argv[1], "rb");
  assert(source);
  char html[8192];
  size_t used = fread(html, 1, sizeof(html) - 1, source);
  fclose(source);
  html[used] = 0;
  web_set_url("file:/webnotes");
  web_parse(html);
  assert(!*web_message);
  strcpy(web_nodes[node("note")].value,
         "A note saved by the actual bundled app");
  web_input(node("note"), false);
  assert(!strcmp(web_nodes[node("status")].text, "Unsaved changes"));
  for (unsigned i = 0; i < 1000; i++) {
    memset(web_nodes[node("note")].value, 'a' + i % 26, 1500);
    web_nodes[node("note")].value[1500] = 0;
    web_input(node("note"), false);
    if (*web_message)
      fprintf(stderr, "edit=%u error=%s\n", i, web_message);
    assert(!*web_message);
  }
  strcpy(web_nodes[node("note")].value,
         "A note saved by the actual bundled app");
  web_input(node("note"), false);
  puts("PASS repeated textarea edits reclaim the JavaScript heap and retain "
       "DOM handles");
  for (unsigned i = 0; i < web_count; i++)
    if (!strcmp(web_nodes[i].text, "Save note"))
      web_click(i);
  assert(!*web_message &&
         !strcmp(web_nodes[node("status")].text, "Saved on disk"));
  web_reset(0);
  web_set_url("file:/webnotes");
  web_parse(html);
  assert(!*web_message && !strcmp(web_nodes[node("note")].value,
                                  "A note saved by the actual bundled app"));
  puts("PASS bundled Notes saves and restores its textarea through JavaScript");
  return 0;
}
