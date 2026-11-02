#include "system.h"
#include "virtio.h"
bool secure_random(void *out, unsigned size) {
  if (!size)
    return true;
  if (!out)
    return false;
  uint8_t *data = out;
  uint32_t a = 1, b, c, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
  if (c & (1u << 30)) {
    unsigned offset = 0;
    while (offset < size) {
      uint64_t value = 0;
      uint8_t ready = 0;
      for (unsigned retry = 0; retry < 64 && !ready; retry++)
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ready));
      if (!ready)
        break;
      unsigned count = size - offset < 8 ? size - offset : 8;
      memcpy(data + offset, &value, count);
      offset += count;
    }
    if (offset == size)
      return true;
  }
  if (virtio_random(data, size))
    return true;
  memset(data, 0, size);
  return false;
}
