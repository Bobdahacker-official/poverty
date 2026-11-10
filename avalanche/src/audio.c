#include "system.h"
#include "usb.h"
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
  bool active, pumping, paused, usb;
} playback;
static struct {
  uint64_t address;
  uint32_t length, flags;
} descriptors[AUDIO_SEGMENTS] __attribute__((aligned(128)));
static uint8_t pcm[AUDIO_SEGMENTS][AUDIO_CHUNK] __attribute__((aligned(4096)));
static unsigned path[16], path_index[16], path_length;
#define HDA_OUTPUT_LIMIT 32
static struct hda_output {
  volatile uint8_t *registers;
  unsigned stream_offset, codec, function, pin, device;
  uint16_t vendor, controller;
  bool present, digital;
} outputs[HDA_OUTPUT_LIMIT];
static unsigned output_count, output_selected, output_volume = 60;
static bool output_muted;
static bool usb_selected;
#define HDA_INPUT_LIMIT 32
#define CAPTURE_RING_BYTES 16384u
#define CAPTURE_LIMIT (1024u * 1024u)
static struct hda_input {
  volatile uint8_t *registers;
  unsigned codec, function, adc, pin;
} microphone[HDA_INPUT_LIMIT];
static unsigned microphone_count;
static uint8_t capture_ring[CAPTURE_RING_BYTES] __attribute__((aligned(4096)));
static struct {
  uint64_t address;
  uint32_t length, flags;
} capture_bdl[4] __attribute__((aligned(128)));
static struct {
  bool active;
  unsigned channels, position, used;
  uint32_t last_poll;
  int error;
  uint8_t wav[44 + CAPTURE_LIMIT];
} recording;
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
static bool find_route(unsigned node, unsigned depth, unsigned target,
                       bool digital) {
  if (depth >= 16)
    return false;
  for (unsigned i = 0; i < depth; i++)
    if (path[i] == node)
      return false;
  path[depth] = node;
  uint32_t caps = parameter(node, 9);
  unsigned type = (caps >> 20) & 15;
  if (!!(caps & (1u << 9)) != digital) return false;
  if ((!target && type == 0) || (target && node == target && type == 4)) {
    converter = node;
    path_length = depth + 1;
    return true;
  }
  if (type != 2 && type != 3 && (!target ? type != 4 : type != 1))
    return false;
  unsigned nodes[32], count = connections(node, nodes);
  for (unsigned i = 0; i < count; i++) {
    path_index[depth] = i;
    if (find_route(nodes[i], depth + 1, target, digital))
      return true;
  }
  return false;
}
static void probe_microphones(unsigned fg, uint32_t widgets) {
  unsigned start = widgets >> 16, end = start + (widgets & 255);
  if (end > 256) return;
  for (unsigned p = start; p < end && microphone_count < HDA_INPUT_LIMIT; p++) {
    uint32_t wcaps = parameter(p, 9), config = 0;
    if (((wcaps >> 20) & 15) != 4 || (wcaps & (1u << 9)) ||
        !(parameter(p, 12) & (1u << 5)) ||
        !verb(p, 0xf1c00, &config) || (config >> 30) == 1 ||
        ((config >> 20) & 15) != 10)
      continue;
    for (unsigned adc = start; adc < end; adc++) {
      uint32_t caps = parameter(adc, 9);
      if (((caps >> 20) & 15) != 1 ||
          !find_route(adc, 0, p, false))
        continue;
      uint32_t pcm_rates = parameter(adc, 10);
      if (!pcm_rates) pcm_rates = parameter(fg, 10);
      if ((pcm_rates & ((1u << 6) | (1u << 17))) != ((1u << 6) | (1u << 17)))
        continue;
      microphone[microphone_count++] = (struct hda_input){
          registers, codec, fg, adc, p};
      break;
    }
  }
}
static void probe_controller(const struct pci_device *device) {
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
  unsigned output_streams = (caps >> 12) & 15, inputs = (caps >> 8) & 15;
  if (!output_streams && !inputs)
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
      if (inputs) probe_microphones(fg, widgets);
      if (!output_streams) continue;
      for (unsigned n = widgets >> 16; n < (widgets >> 16) + (widgets & 255);
           n++) {
        uint32_t wcaps = parameter(n, 9), config = 0, pincaps = parameter(n, 12);
        bool digital = wcaps & (1u << 9);
        if (((wcaps >> 20) & 15) != 4 || !(pincaps & 16) ||
            (digital && !(pincaps & ((1u << 7) | (1u << 24)))))
          continue;
        if (!digital && (!verb(n, 0xf1c00, &config) || (config >> 30) == 1 ||
                         ((config >> 20) & 15) > 2))
          continue;
        if (!find_route(n, 0, 0, digital))
          continue;
        if (output_count == HDA_OUTPUT_LIMIT) return;
        unsigned type = digital ? ((pincaps & (1u << 24)) ? 17 : 16)
                                : (config >> 20) & 15;
        uint32_t sense = 0;
        bool plugged = (parameter(n, 12) & 4) && verb(n, 0xf0900, &sense) &&
                       (sense >> 31);
        outputs[output_count++] = (struct hda_output){
            registers, stream_offset, codec, fg, n, type,
            device->vendor, device->device, plugged, digital};
      }
    }
  }
}
static uint32_t amp_caps(unsigned node, unsigned id, uint32_t widget) {
  return parameter(widget & (1u << 3) ? node : function, id);
}
static bool set_output_gain(void) {
  bool adjustable = false;
  for (unsigned i = 0; i < path_length; i++) {
    unsigned node = path[i];
    uint32_t widget = parameter(node, 9);
    if (!(widget & 4)) continue;
    uint32_t caps = amp_caps(node, 18, widget);
    unsigned zero = caps & 127, steps = (caps >> 8) & 127;
    unsigned gain = (zero < steps ? zero : steps) * output_volume / 100;
    unsigned mute = (output_muted || !output_volume) && (caps >> 31) ? 128 : 0;
    if (send(node, 0x30000 | 0xb000 | gain | mute))
      adjustable |= (caps >> 31) != 0 || steps != 0;
  }
  return adjustable;
}
static bool configure_output(unsigned index) {
  const struct hda_output *out = &outputs[index];
  registers = out->registers;
  stream_offset = out->stream_offset;
  codec = out->codec;
  function = out->function;
  pin = out->pin;
  if (!send(function, 0x70500) || !find_route(pin, 0, 0, out->digital))
    return false;
  for (unsigned i = 0; i < path_length; i++) {
    unsigned node = path[i];
    uint32_t widget = parameter(node, 9);
    (void)send(node, 0x70500);
    if (i + 1 < path_length && ((widget >> 20) & 15) != 2)
      (void)send(node, 0x70100 | path_index[i]);
    if (widget & 2) {
      uint32_t caps = amp_caps(node, 13, widget);
      unsigned gain = caps & 127, steps = (caps >> 8) & 127;
      if (gain > steps) gain = steps;
      (void)send(node, 0x30000 | 0x7000 | (path_index[i] << 8) | gain);
    }
  }
  uint32_t caps = parameter(pin, 12);
  if (out->digital) {
    if (!send(pin, 0x70740) || !send(converter, 0x70d01)) return false;
    output_selected = index;
    return true;
  }
  if (!send(pin, 0x70700 | 0x40 | ((caps & 8) ? 0x80 : 0))) return false;
  if (caps & (1u << 16)) {
    uint32_t power = 0;
    (void)verb(pin, 0xf0c00, &power);
    (void)send(pin, 0x70c00 | (power & 7) | 2);
  }
  (void)set_output_gain();
  output_selected = index;
  return true;
}
void audio_init(void) {
  output_count = microphone_count = 0;
  available = false;
  for (unsigned i = 0; i < pci_count_devices(); i++) {
    const struct pci_device *device = pci_device_at(i);
    if (device->class_id == 4 && device->subclass == 3 && device->prog_if == 0)
      probe_controller(device);
  }
  for (unsigned pass = 0; pass < 4 && !available; pass++)
    for (unsigned i = 0; i < output_count; i++)
      if (outputs[i].digital == (pass >= 2) &&
          ((pass & 1) || outputs[i].present) && configure_output(i)) {
        available = true;
        break;
      }
  if (available) console_write("HD Audio PCM output ready\n");
}
unsigned audio_output_count(void) { return output_count + usb_audio_available(); }
unsigned audio_output_selected(void) {
  return usb_selected || (!available && usb_audio_available())
             ? output_count : output_selected;
}
unsigned audio_output_type(unsigned index) {
  if (index == output_count && usb_audio_available()) return 18;
  return index < output_count ? outputs[index].device : 15;
}
int audio_output_select(unsigned index) {
  if (index >= audio_output_count()) return -22;
  if (playback.active || recording.active) return -16;
  if (index == output_count) {
    usb_selected = true;
    return 0;
  }
  usb_selected = false;
  if (available && index == output_selected) return 0;
  unsigned old = output_selected;
  bool had_output = available;
  if (had_output) (void)send(pin, 0x70700);
  available = configure_output(index);
  if (available) return 0;
  if (had_output) available = configure_output(old);
  return -5;
}
unsigned audio_volume(void) { return output_volume; }
bool audio_muted(void) { return output_muted; }
int audio_set_volume(unsigned volume, bool muted) {
  if (recording.active) return -16;
  if (!available || volume > 100) return -22;
  if (outputs[output_selected].digital) return -95;
  output_volume = volume;
  output_muted = muted;
  return set_output_gain() ? 0 : -95;
}
static void capture_restore_output(void) {
  if (available)
    available = configure_output(output_selected);
}
unsigned audio_capture_count(void) { return microphone_count; }
bool audio_capture_active(void) { return recording.active; }
unsigned audio_capture_bytes(void) { return recording.used; }
int audio_capture_error(void) { return recording.error; }
int audio_capture_start(unsigned index) {
  if (index >= microphone_count) return -19;
  if (recording.active || playback.active || playback.paused) return -16;
  const struct hda_input *input = &microphone[index];
  registers = input->registers;
  codec = input->codec;
  function = input->function;
  if (!send(function, 0x70500) || !find_route(input->adc, 0, input->pin, false))
    goto failed;
  for (unsigned i = 0; i < path_length; i++) {
    unsigned node = path[i];
    uint32_t widget = parameter(node, 9);
    (void)send(node, 0x70500);
    if (i + 1 < path_length && ((widget >> 20) & 15) != 2)
      (void)send(node, 0x70100 | path_index[i]);
    if ((widget & 2) && i + 1 < path_length) {
      uint32_t caps = amp_caps(node, 13, widget);
      unsigned gain = caps & 127, steps = (caps >> 8) & 127;
      (void)send(node, 0x30000 | 0x7000 | (path_index[i] << 8) |
                           (gain < steps ? gain : steps));
    }
  }
  unsigned pin_caps = parameter(input->pin, 12);
  unsigned references = (pin_caps >> 8) & 255;
  unsigned reference = references & (1u << 4) ? 4 : references & (1u << 1) ? 1 : 0;
  if (!send(input->pin, 0x70720 | reference))
    goto failed;
  unsigned channels = parameter(input->adc, 9) & 1 ? 2 : 1;
  unsigned format = 0x10 | (channels - 1);
  const unsigned sd = 0x80;
  set32(sd, 0);
  set32(sd, 1);
  if (!wait_mask(sd, 1, 1, 20)) goto disable_pin;
  set32(sd, 0);
  if (!wait_mask(sd, 1, 0, 20)) goto disable_pin;
  for (unsigned i = 0; i < 4; i++) {
    capture_bdl[i].address = page_physical(capture_ring + i * 4096);
    capture_bdl[i].length = 4096;
    capture_bdl[i].flags = 0;
  }
  if (!send(input->adc, 0x20000 | format) || !send(input->adc, 0x70620))
    goto disable_pin;
  set32(sd + 8, CAPTURE_RING_BYTES);
  set16(sd + 12, 3);
  set16(sd + 18, format);
  uint64_t bdl = page_physical(capture_bdl);
  set32(sd + 24, bdl);
  set32(sd + 28, bdl >> 32);
  registers[sd + 3] = 0x1c;
  recording.channels = channels;
  recording.position = recording.used = 0;
  recording.last_poll = clock_ticks();
  recording.error = 0;
  recording.active = true;
  __asm__ volatile("sfence" ::: "memory");
  set32(sd, (2u << 20) | 2);
  return 0;
disable_pin:
  (void)send(input->pin, 0x70700);
failed:
  capture_restore_output();
  return -5;
}
static void capture_finish(void) {
  if (!recording.active) return;
  set32(0x80, 0);
  (void)wait_mask(0x80, 2, 0, 20);
  (void)send(path[0], 0x70600);
  (void)send(path[path_length - 1], 0x70700);
  recording.active = false;
  capture_restore_output();
}
static void capture_poll(void) {
  if (!recording.active) return;
  uint32_t now = clock_ticks();
  unsigned frame = recording.channels * 2;
  if (now - recording.last_poll >= CAPTURE_RING_BYTES * 100 / (48000 * frame) ||
      (registers[0x83] & 0x18)) {
    recording.error = -5;
    capture_finish();
    return;
  }
  recording.last_poll = now;
  unsigned pos = reg32(0x84) % CAPTURE_RING_BYTES;
  pos -= pos % frame;
  unsigned count = (pos + CAPTURE_RING_BYTES - recording.position) % CAPTURE_RING_BYTES;
  if (count > CAPTURE_LIMIT - recording.used)
    count = CAPTURE_LIMIT - recording.used;
  unsigned first = CAPTURE_RING_BYTES - recording.position;
  if (first > count) first = count;
  uint8_t *output = recording.wav + 44 + recording.used;
  __asm__ volatile("lfence" ::: "memory");
  memcpy(output, capture_ring + recording.position, first);
  memcpy(output + first, capture_ring, count - first);
  recording.position = pos;
  recording.used += count;
  if (recording.used == CAPTURE_LIMIT) capture_finish();
}
void audio_capture_stop(void) {
  capture_poll();
  capture_finish();
}
static void wav_integer(unsigned offset, unsigned value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; i++)
    recording.wav[offset + i] = value >> (8 * i);
}
int audio_capture_save(const char *path) {
  if (recording.active) return -16;
  if (!recording.used || !recording.channels) return -22;
  unsigned length = strlen(path);
  if (path[0] != '/' || length + sizeof(".part") > 256) return -22;
  char partial[256];
  memcpy(partial, path, length);
  memcpy(partial + length, ".part", sizeof(".part"));
  struct file_metadata info;
  if (fs_metadata(path, &info) != -2 || fs_metadata(partial, &info) != -2)
    return -17;
  unsigned frame = recording.channels * 2;
  memcpy(recording.wav, "RIFF", 4);
  wav_integer(4, recording.used + 36, 4);
  memcpy(recording.wav + 8, "WAVEfmt ", 8);
  wav_integer(16, 16, 4);
  wav_integer(20, 1, 2);
  wav_integer(22, recording.channels, 2);
  wav_integer(24, 48000, 4);
  wav_integer(28, 48000 * frame, 4);
  wav_integer(32, frame, 2);
  wav_integer(34, 16, 2);
  memcpy(recording.wav + 36, "data", 4);
  wav_integer(40, recording.used, 4);
  int r = fs_write_new_stream(partial, recording.wav, recording.used + 44);
  return r < 0 ? r : fs_rename(partial, path);
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
  console_write("Analogue outputs: ");
  print_uint(output_count);
  console_putc('\n');
}
static uint32_t le32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static int display_audio_prepare(unsigned rate, unsigned channels) {
  if (channels != 2) return -95;
  unsigned rate_bit;
  switch (rate) {
  case 32000: rate_bit = 1; break;
  case 44100: rate_bit = 2; break;
  case 48000: rate_bit = 4; break;
  case 96000: rate_bit = 16; break;
  default: return -95;
  }
  uint32_t sense, size;
  if (!verb(pin, 0xf0900, &sense) || (sense & 0xc0000000u) != 0xc0000000u ||
      !verb(pin, 0xf2e08, &size) || size < 20 || size > 256)
    return -19;
  uint8_t eld[256];
  for (unsigned i = 0; i < size; i++) {
    uint32_t entry;
    if (!verb(pin, 0xf2f00 | i, &entry) || !(entry & 0x80000000u))
      return -5;
    eld[i] = entry;
  }
  unsigned connection = (eld[5] >> 2) & 3;
  unsigned sad = 20 + (eld[4] & 31), count = eld[5] >> 4;
  if (connection > 1 || sad > size || count > (size - sad) / 3) return -95;
  bool supported = false;
  for (unsigned i = 0; i < count; i++, sad += 3)
    if (((eld[sad] >> 3) & 15) == 1 && (eld[sad] & 7) >= 1 &&
        (eld[sad + 1] & rate_bit) && (eld[sad + 2] & 1))
      supported = true;
  if (!supported) return -95;
  if (!send(converter, 0x72d01) || !send(pin, 0x73200) ||
      !send(pin, 0x73000))
    return -5;
  for (unsigned channel = 0; channel < 8; channel++)
    if (!send(pin, 0x73400 | ((channel < 2 ? channel : 15) << 4) | channel))
      return -5;
  uint8_t info[14] = {0};
  if (connection == 1) {
    info[1] = 0x84;
    info[2] = 0x1b;
    info[3] = 0x44;
  } else {
    info[0] = 0x84;
    info[1] = 1;
    info[2] = 10;
    info[3] = (uint8_t)(0 - 0x84 - 1 - 10 - 1);
  }
  info[4] = 1;
  for (unsigned i = 0; i < sizeof(info); i++)
    if (!send(pin, 0x73100 | info[i])) return -5;
  return send(pin, 0x732c0) ? 0 : -5;
}
static int audio_play_from(const char *name, unsigned frame) {
  if (!available && usb_audio_available()) usb_selected = true;
  if (!available && !usb_selected)
    return -19;
  if (playback.active || recording.active)
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
  if (frame >= data_size / align)
    return -22;
  if (usb_selected) {
    if (rate != 48000 || channels != 2) return -95;
    int result = usb_audio_start();
    if (result < 0) return result;
    memset(&playback, 0, sizeof(playback));
    memcpy(playback.path, name, strlen(name) + 1);
    playback.offset = data_start;
    playback.size = data_size;
    playback.loaded = playback.played = frame * align;
    playback.previous = playback.played;
    playback.rate = rate;
    playback.channels = channels;
    playback.active = playback.usb = true;
    return 0;
  }
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
  if (outputs[output_selected].digital) {
    int status = display_audio_prepare(rate, channels);
    if (status < 0) return status;
  }
  unsigned sd = stream_offset;
  set32(sd, 0);
  set32(sd, 1);
  if (!wait_mask(sd, 1, 1, 20))
    return -5;
  set32(sd, 0);
  if (!wait_mask(sd, 1, 0, 20))
    return -5;
  unsigned loaded = frame * align;
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
  playback.played = frame * align;
  playback.rate = rate;
  playback.channels = channels;
  playback.start = clock_ticks();
  playback.deadline = (data_size - playback.played) / (rate * align) * 100 + 500;
  playback.last_poll = playback.start;
  playback.active = true;
  set32(sd, (stream << 20) | 2);
  return 0;
}
int audio_play(const char *name) { return audio_play_from(name, 0); }
bool audio_paused(void) { return playback.paused; }
int audio_pause(void) {
  audio_poll();
  if (!playback.active)
    return -22;
  audio_stop();
  playback.paused = true;
  return 0;
}
int audio_seek(unsigned frame) {
  if (!playback.channels || frame >= playback.size / (playback.channels * 2))
    return -22;
  if (!playback.active) {
    playback.played = frame * playback.channels * 2;
    playback.paused = true;
    return 0;
  }
  char name[sizeof(playback.path)];
  memcpy(name, playback.path, sizeof(name));
  audio_stop();
  return audio_play_from(name, frame);
}
int audio_resume(void) {
  if (!playback.paused || !playback.channels)
    return -22;
  char name[sizeof(playback.path)];
  memcpy(name, playback.path, sizeof(name));
  return audio_play_from(name, playback.played / (playback.channels * 2));
}
int audio_status(void) { return playback.active ? 1 : playback.result; }
void audio_snapshot(struct av_audio_status *out) {
  memset(out, 0, sizeof(*out));
  out->available = available || usb_audio_available();
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
  playback.paused = false;
  if (!playback.active)
    return;
  playback.active = false;
  if (playback.usb) {
    usb_audio_stop();
    return;
  }
  set32(stream_offset, stream << 20);
  (void)send(converter, 0x70600);
  if (outputs[output_selected].digital)
    (void)send(pin, 0x73200);
}
static void poll_usb_audio(void) {
  int error = usb_audio_error();
  if (error < 0) {
    playback.result = error;
    audio_stop();
    return;
  }
  playback.played = playback.previous + usb_audio_completed();
  if (playback.played >= playback.size) {
    audio_stop();
    return;
  }
  for (unsigned i = 0; i < 32 && playback.loaded < playback.size; i++) {
    uint8_t packet[192];
    unsigned count = playback.size - playback.loaded;
    if (count > sizeof(packet)) count = sizeof(packet);
    if (fs_read_at(playback.path, (uint64_t)playback.offset + playback.loaded,
                   packet, count) != (int)count) {
      playback.result = -5;
      audio_stop();
      return;
    }
    int queued = usb_audio_queue(packet, count);
    if (queued <= 0) {
      if (queued < 0) {
        playback.result = queued;
        audio_stop();
      }
      return;
    }
    playback.loaded += queued;
  }
}
void audio_poll(void) {
  capture_poll();
  if (usb_selected && !usb_audio_available() && !playback.active)
    usb_selected = false;
  if (!playback.active || playback.pumping)
    return;
  playback.pumping = true;
  if (playback.usb) {
    poll_usb_audio();
    playback.pumping = false;
    return;
  }
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
