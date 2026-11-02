#ifndef AVALANCHE_IMAGE_H
#define AVALANCHE_IMAGE_H
#include "avalanche.h"
struct image_document {
  uint32_t *pixels;
  unsigned width, height;
};
bool image_load(struct image_document *, const char *path);
void image_release(struct image_document *);
const char *image_error(void);
#endif
