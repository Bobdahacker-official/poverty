#ifndef AVALANCHE_WEB_H
#define AVALANCHE_WEB_H
#include "avalanche.h"
#define WEB_NODES 256
#define WEB_TABS 8
struct web_style {
  uint32_t color, background;
  unsigned size, padding, margin;
  bool hidden, center;
};
struct web_node {
  char tag[16], id[64], classes[128], text[1024], href[512], onclick[512],
      inline_style[512];
  int parent;
  struct web_style style;
  uint64_t object;
};
/* Each tab owns its DOM, script heap and resources. Switching never reparses
 * a page or re-runs its scripts. */
struct web_context {
  struct web_node nodes[WEB_NODES];
  unsigned count, css_used, script_used;
  char message[128], url[512], css_text[16384], script_text[32768];
  unsigned char js_heap[262144] __attribute__((aligned(8)));
  struct js *js_engine;
};
struct web_context *web_current(void);
bool web_select(unsigned tab);
void web_reset(unsigned tab);
#define web_nodes (web_current()->nodes)
#define web_count (web_current()->count)
#define web_message (web_current()->message)
void web_set_url(const char *url);
void web_parse(const char *html);
void web_click(unsigned node);
#endif
