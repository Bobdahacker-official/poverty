#include "image.h"
#include "system.h"
int abs(int value) {
  return value == INT32_MIN ? INT32_MAX : value < 0 ? -value : value;
}
struct allocation {
  size_t size;
  unsigned pages;
  unsigned reserved;
};
static void *image_allocate(size_t size) {
  if (!size || size > 64u * 1024 * 1024)
    return NULL;
  unsigned pages = (size + sizeof(struct allocation) + 4095) / 4096;
  struct allocation *p = pages_alloc(pages);
  if (!p)
    return NULL;
  p->size = size;
  p->pages = pages;
  return p + 1;
}
static void image_free(void *data) {
  if (!data)
    return;
  struct allocation *p = (struct allocation *)data - 1;
  unsigned count = p->pages;
  for (unsigned i = 0; i < count; i++)
    page_free((uint8_t *)p + (size_t)i * 4096);
}
static void *image_reallocate(void *data, size_t size) {
  if (!data)
    return image_allocate(size);
  if (!size) {
    image_free(data);
    return NULL;
  }
  struct allocation *old = (struct allocation *)data - 1;
  void *next = image_allocate(size);
  if (!next)
    return NULL;
  memcpy(next, data, old->size < size ? old->size : size);
  image_free(data);
  return next;
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(n) image_allocate(n)
#define STBI_REALLOC(p, n) image_reallocate(p, n)
#define STBI_FREE(p) image_free(p)
#define STBI_ASSERT(x) ((void)0)
#include "../vendor/stb/stb_image.h"
static const char *last_error = "Open an image from Files or enter its path.";
const char *image_error(void) { return last_error; }
void image_release(struct image_document *document) {
  image_free(document->pixels);
  memset(document, 0, sizeof(*document));
}
bool image_load(struct image_document *document, const char *path) {
  console_write("Opening image: ");
  console_write(path);
  console_putc('\n');
  int length = fs_size(path);
  if (length <= 0 || length > 16 * 1024 * 1024) {
    last_error = "Image file is empty, unavailable, or exceeds 16 MiB.";
    return false;
  }
  uint8_t *source = image_allocate(length);
  if (!source) {
    last_error = "Insufficient memory to open this image.";
    return false;
  }
  int count = fs_read_at(path, 0, source, length);
  if (count != length) {
    image_free(source);
    last_error = "Could not read the complete image.";
    return false;
  }
  int w, h, channels;
  uint8_t *rgba = stbi_load_from_memory(source, length, &w, &h, &channels, 4);
  image_free(source);
  if (!rgba) {
    last_error = stbi_failure_reason();
    return false;
  }
  for (size_t i = 0; i < (size_t)w * h; i++) {
    uint8_t *p = rgba + i * 4;
    uint32_t value = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 |
                     (uint32_t)p[1] << 8 | p[2];
    ((uint32_t *)rgba)[i] = value;
  }
  image_release(document);
  document->pixels = (uint32_t *)rgba;
  document->width = w;
  document->height = h;
  last_error = "Image loaded";
  return true;
}
