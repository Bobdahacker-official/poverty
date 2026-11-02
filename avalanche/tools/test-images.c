#define _GNU_SOURCE
#include "../src/image.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static unsigned live_pages;
void console_write(const char *text) { (void)text; }
void console_putc(char c) { (void)c; }
void *pages_alloc(unsigned count) {
  void *p = mmap(NULL, (size_t)count * 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return NULL;
  live_pages += count;
  return p;
}
bool page_free(void *p) {
  assert(live_pages);
  live_pages--;
  return munmap(p, 4096) == 0;
}
int fs_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -2;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}
int fs_read_at(const char *path, uint64_t offset, void *out, uint32_t size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -2;
  assert(!fseek(f, offset, SEEK_SET));
  int n = fread(out, 1, size, f);
  fclose(f);
  return n;
}
int main(int argc, char **argv) {
  assert(argc >= 3);
  struct image_document doc = {0};
  for (int i = 1; i < argc - 1; i++) {
    assert(image_load(&doc, argv[i]));
    assert(doc.width == 3 && doc.height == 2);
    if (strstr(argv[i], ".jpg")) {
      assert((doc.pixels[0] >> 24) == 255 && (doc.pixels[5] >> 24) == 255);
      assert((doc.pixels[0] >> 16 & 255) > 245 && (doc.pixels[0] & 255) < 10);
      assert((doc.pixels[5] & 255) > 245 && (doc.pixels[5] >> 16 & 255) < 10);
    } else
      assert(doc.pixels[0] == 0xffff0000 && doc.pixels[5] == 0xff0000ff);
    puts("PASS image pixels, dimensions and channel order");
  }
  uint32_t *previous = doc.pixels;
  assert(!image_load(&doc, argv[argc - 1]));
  assert(doc.pixels == previous && doc.width == 3 && doc.height == 2);
  image_release(&doc);
  assert(!live_pages);
  puts("PASS malformed image preserves open document and releases allocations");
}
