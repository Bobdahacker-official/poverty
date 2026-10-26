#ifndef AVALANCHE_WEB_H
#define AVALANCHE_WEB_H
#include "avalanche.h"
#define WEB_NODES 256
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
extern struct web_node web_nodes[WEB_NODES];
extern unsigned web_count;
extern char web_message[128];
void web_set_url(const char *url);
void web_parse(const char *html);
void web_click(unsigned node);
#endif
