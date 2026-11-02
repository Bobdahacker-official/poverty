#include "system.h"
#define AUDIO_SEGMENTS 8u
#define AUDIO_CHUNK 65536u
/* Intel High Definition Audio, codec verbs and a polled PCM output stream. */
static volatile uint8_t *registers;
static unsigned codec, function, converter, pin, stream, stream_offset;
static bool available;
static struct {
  char path[256];
  unsigned offset, size, loaded, played, previous, deadline, rate, channels;
  uint32_t start, last_poll;
  int result;
  bool active, pumping;
} playback;
static struct {
  uint64_t address;
  uint32_t length, flags;
} descriptors[AUDIO_SEGMENTS] __attribute__((aligned(128)));
static uint8_t pcm[AUDIO_SEGMENTS][AUDIO_CHUNK] __attribute__((aligned(4096)));
static unsigned path[16], path_index[16], path_length;
static uint32_t reg32(unsigned offset) {
  return *(volatile uint32_t *)(registers + offset);
}
static void set32(unsigned offset, uint32_t value) {
  *(volatile uint32_t *)(registers + offset) = value;
}
static uint16_t reg16(unsigned offset) {
  return *(volatile uint16_t *)(registers + offset);
}
static void set16(unsigned offset, uint16_t value) {
  *(volatile uint16_t *)(registers + offset) = value;
}
static bool wait_mask(unsigned offset, uint32_t mask, uint32_t value,
                      unsigned ticks) {
  uint32_t start = clock_ticks();
  while ((reg32(offset) & mask) != value) {
    if (clock_ticks() - start > ticks)
      return false;
    cpu_pause();
  }
  return true;
}
static bool verb(unsigned node, unsigned command, uint32_t *response) {
  if (!wait_mask(0x68, 1, 0, 20))
    return false;
  set16(0x68, 2);
  set32(0x60, (codec << 28) | (node << 20) | command);
  set16(0x68, 1);
  if (!wait_mask(0x68, 3, 2, 20))
    return false;
  *response = reg32(0x64);
  set16(0x68, 2);
  return true;
}
static uint32_t parameter(unsigned node, unsigned id) {
  uint32_t value = 0;
  (void)verb(node, 0xf0000 | id, &value);
  return value;
}
static bool send(unsigned node, unsigned command) {
  uint32_t ignored;
  return verb(node, command, &ignored);
}
static unsigned connections(unsigned node, unsigned out[32]) {
  uint32_t info = parameter(node, 14);
  unsigned count = info & 127, used = 0, previous = 0;
  bool wide = info & 128;
  if (count > 32)
    return 0;
  for (unsigned at = 0; at < count;) {
    uint32_t value;
    if (!verb(node, 0xf0200 | at, &value))
      return 0;
    for (unsigned i = 0; i < (wide ? 2u : 4u) && at < count; i++, at++) {
      unsigned encoded =
          (value >> (i * (wide ? 16 : 8))) & (wide ? 65535 : 255);
      unsigned target = encoded & (wide ? 32767 : 127);
      if (encoded & (wide ? 32768 : 128)) {
        if (!previous || target <= previous || target - previous > 32 - used)
          return 0;
        for (unsigned n = previous + 1; n <= target; n++)
          out[used++] = n;
      } else {
        if (used == 32)
          return 0;
        out[used++] = target;
      }
      previous = target;
    }
  }
  return used;
}
static bool route(unsigned node, unsigned depth) {
  if (depth >= 16)
    return false;
  for (unsigned i = 0; i < depth; i++)
    if (path[i] == node)
      return false;
  path[depth] = node;
  uint32_t caps = parameter(node, 9);
  unsigned type = (caps >> 20) & 15;
  if (caps & (1u << 9))
    return false; /* This path carries analogue PCM. */
  if (type == 0) {
    converter = node;
    path_length = depth + 1;
    return true;
  }
  if (type != 2 && type != 3 && type != 4)
    return false;
  unsigned nodes[32], count = connections(node, nodes);
  for (unsigned i = 0; i < count; i++) {
    path_index[depth] = i;
    if (route(nodes[i], depth + 1))
      return true;
  }
  return false;
}
void audio_init(void) {
  const struct pci_device *device = pci_find_class(4, 3, 0);
  uint64_t base = pci_memory_bar(device, 0);
  if (!base || !platform_mmio(base, 0x4000))
    return;
  pci_enable(device, 6);
  registers = page_pointer(base);
  set32(8, reg32(8) & ~1u);
  if (!wait_mask(8, 1, 0, 20))
    return;
  sleep_ticks(1);
  set32(8, reg32(8) | 1);
  if (!wait_mask(8, 1, 1, 20))
    return;
  sleep_ticks(1);
  uint16_t caps = reg16(0), present = reg16(14);
  unsigned outputs = (caps >> 12) & 15, inputs = (caps >> 8) & 15;
  if (!outputs)
    return;
  stream = 1;
  stream_offset = 0x80 + inputs * 0x20;
  set32(0x20, 0); /* Interrupts are masked; stream completion is polled. */
  for (codec = 0; codec < 15; codec++) {
    if (!(present & (1u << codec)))
      continue;
    uint32_t nodes = parameter(0, 4);
    for (unsigned fg = nodes >> 16; fg < (nodes >> 16) + (nodes & 255); fg++) {
      if ((parameter(fg, 5) & 255) != 1)
        continue;
      function = fg;
      if (!send(fg, 0x70500))
        continue;
      uint32_t widgets = parameter(fg, 4);
      for (unsigned n = widgets >> 16; n < (widgets >> 16) + (widgets & 255);
           n++) {
        uint32_t wcaps = parameter(n, 9), config = 0;
        if (((wcaps >> 20) & 15) != 4 || (wcaps & (1u << 9)) ||
            !(parameter(n, 12) & 16))
          continue;
        if (!verb(n, 0xf1c00, &config) || (config >> 30) == 1 ||
            ((config >> 20) & 15) > 2)
          continue;
        if (!route(n, 0))
          continue;
        pin = n;
        for (unsigned i = 0; i < path_length; i++) {
          unsigned node = path[i];
          uint32_t w = parameter(node, 9);
          (void)send(node, 0x70500);
          if (i + 1 < path_length && ((w >> 20) & 15) != 2)
            (void)send(node, 0x70100 | path_index[i]);
          if (w & 4)
            (void)send(node, 0x30000 | 0xb000 | (parameter(node, 18) & 127));
          if (w & 2)
            (void)send(node, 0x30000 | 0x7000 | (path_index[i] << 8) |
                                 (parameter(node, 13) & 127));
        }
        uint32_t pincaps = parameter(pin, 12);
        if (!send(pin, 0x70700 | 0x40 | ((pincaps & 8) ? 0x80 : 0)))
          continue;
        if (pincaps & (1u << 16))
          (void)send(pin, 0x70c02);
        available = true;
        console_write("Intel HD Audio PCM output ready\n");
        return;
      }
    }
  }
}
void audio_info(void) {
  if (!available) {
    console_write("No analogue HDA output detected.\n");
    return;
  }
  console_write("Intel HD Audio: codec ");
  print_uint(codec);
  console_write(" converter ");
  print_uint(converter);
  console_write(" pin ");
  print_uint(pin);
  console_write("; PCM16 output\n");
}
static uint32_t le32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
int audio_play(const char *name) {
  if (!available)
    return -19;
  if (playback.active)
    return -16;
  if (strlen(name) >= sizeof(playback.path))
    return -36;
  int file_size = fs_size(name);
  uint8_t header[40];
  if (file_size < 44 || fs_read_at(name, 0, header, 12) != 12 ||
      __builtin_memcmp(header, "RIFF", 4) ||
      __builtin_memcmp(header + 8, "WAVE", 4))
    return -22;
  unsigned offset = 12, data_start = 0, data_size = 0, rate = 0, channels = 0,
           align = 0;
  while (offset + 8 <= (unsigned)file_size) {
    if (fs_read_at(name, offset, header, 8) != 8)
      return -5;
    unsigned size = le32(header + 4);
    if (size > (unsigned)file_size - offset - 8)
      return -22;
    if (!__builtin_memcmp(header, "fmt ", 4)) {
      if (size < 16 || fs_read_at(name, offset + 8, header, 16) != 16 ||
          le16(header) != 1 || le16(header + 14) != 16)
        return -95;
      channels = le16(header + 2);
      rate = le32(header + 4);
      align = le16(header + 12);
    } else if (!__builtin_memcmp(header, "data", 4)) {
      data_start = offset + 8;
      data_size = size;
    }
    uint64_t next = (uint64_t)offset + 8 + size + (size & 1);
    if (next > (unsigned)file_size)
      return -22;
    offset = next;
  }
  if (!data_start || !data_size || (channels != 1 && channels != 2) ||
      align != channels * 2 || data_size % align)
    return -22;
  unsigned format = 0x10 | (channels - 1), rate_bit;
  switch (rate) {
  case 8000:
    format |= 5u << 8;
    rate_bit = 0;
    break;
  case 16000:
    format |= 2u << 8;
    rate_bit = 2;
    break;
  case 22050:
    format |= (1u << 14) | (1u << 8);
    rate_bit = 3;
    break;
  case 24000:
    format |= 1u << 8;
    rate_bit = 6;
    break;
  case 32000:
    format |= (1u << 11) | (2u << 8);
    rate_bit = 4;
    break;
  case 44100:
    format |= 1u << 14;
    rate_bit = 5;
    break;
  case 48000:
    rate_bit = 6;
    break;
  case 96000:
    format |= 1u << 11;
    rate_bit = 8;
    break;
  default:
    return -95;
  }
  unsigned supported = parameter(converter, 10);
  if (!supported)
    supported = parameter(function, 10);
  if (!(supported & (1u << rate_bit)) || !(supported & (1u << 17)))
    return -95;
  if (channels == 2 && !(parameter(converter, 9) & 1))
    return -95;
  unsigned sd = stream_offset;
  set32(sd, 0);
  set32(sd, 1);
  if (!wait_mask(sd, 1, 1, 20))
    return -5;
  set32(sd, 0);
  if (!wait_mask(sd, 1, 0, 20))
    return -5;
  unsigned loaded = 0;
  for (unsigned i = 0; i < AUDIO_SEGMENTS; i++) {
    memset(pcm[i], 0, AUDIO_CHUNK);
    unsigned n = data_size - loaded;
    if (n > AUDIO_CHUNK)
      n = AUDIO_CHUNK;
    if (n &&
        fs_read_at(name, (uint64_t)data_start + loaded, pcm[i], n) != (int)n)
      return -5;
    loaded += n;
    descriptors[i].address = page_physical(pcm[i]);
    descriptors[i].length = AUDIO_CHUNK;
    descriptors[i].flags = 1;
  }
  if (!send(converter, 0x20000 | format) ||
      !send(converter, 0x70600 | (stream << 4)))
    return -5;
  set32(sd + 8, sizeof(pcm));
  set16(sd + 12, AUDIO_SEGMENTS - 1);
  set16(sd + 18, format);
  uint64_t bdl = page_physical(descriptors);
  set32(sd + 24, bdl);
  set32(sd + 28, bdl >> 32);
  registers[sd + 3] = 0x1c;
  __asm__ volatile("sfence" ::: "memory");
  memset(&playback, 0, sizeof(playback));
  memcpy(playback.path, name, strlen(name) + 1);
  playback.offset = data_start;
  playback.size = data_size;
  playback.loaded = loaded;
  playback.rate = rate;
  playback.channels = channels;
  playback.start = clock_ticks();
  playback.deadline = data_size / (rate * align) * 100 + 500;
  playback.last_poll = playback.start;
  playback.active = true;
  set32(sd, (stream << 20) | 2);
  return 0;
}
int audio_status(void) { return playback.active ? 1 : playback.result; }
void audio_snapshot(struct av_audio_status *out) {
  memset(out, 0, sizeof(*out));
  out->available = available;
  out->playing = playback.active;
  out->rate = playback.rate;
  out->channels = playback.channels;
  out->error = playback.result;
  if (playback.channels) {
    out->frames_total = playback.size / (playback.channels * 2);
    unsigned played =
        playback.played > playback.size ? playback.size : playback.played;
    out->frames_played = played / (playback.channels * 2);
  }
  memcpy(out->path, playback.path, sizeof(out->path));
}
void audio_stop(void) {
  if (!playback.active)
    return;
  playback.active = false;
  set32(stream_offset, stream << 20);
  (void)send(converter, 0x70600);
}
void audio_poll(void) {
  if (!playback.active || playback.pumping)
    return;
  playback.pumping = true;
  unsigned ring_ticks =
      (uint64_t)sizeof(pcm) * 100 / (playback.rate * playback.channels * 2);
  uint32_t now_ticks = clock_ticks();
  if (now_ticks - playback.last_poll >= ring_ticks) {
    playback.result = -32;
    audio_stop();
    playback.pumping = false;
    return;
  }
  playback.last_poll = now_ticks;
  unsigned sd = stream_offset, position = reg32(sd + 4) % sizeof(pcm);
  playback.played += (position + sizeof(pcm) - playback.previous) % sizeof(pcm);
  if (playback.played >= playback.size) {
    audio_stop();
    playback.pumping = false;
    return;
  }
  unsigned old = playback.previous / AUDIO_CHUNK, now = position / AUDIO_CHUNK;
  while (old != now) {
    memset(pcm[old], 0, AUDIO_CHUNK);
    unsigned n = playback.size - playback.loaded;
    if (n > AUDIO_CHUNK)
      n = AUDIO_CHUNK;
    if (n &&
        fs_read_at(playback.path, (uint64_t)playback.offset + playback.loaded,
                   pcm[old], n) != (int)n) {
      playback.result = -5;
      break;
    }
    playback.loaded += n;
    old = (old + 1) % AUDIO_SEGMENTS;
  }
  __asm__ volatile("sfence" ::: "memory");
  playback.previous = position;
  if ((registers[sd + 3] & 0x18) ||
      clock_ticks() - playback.start > playback.deadline)
    playback.result = -5;
  if (playback.result)
    audio_stop();
  playback.pumping = false;
}
