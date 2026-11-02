#include "usb.h"
/* USB 1.1 UHCI with boot-protocol HID keyboard and mouse. Polling schedules
 * use the controller's real TD/QH DMA format. */
#define ACTIVE (1u << 23)
#define LOW_SPEED (1u << 26)
#define ERROR_BITS (0x7eu << 16)
#define END 1u
struct td {
  volatile uint32_t next, status;
  uint32_t token, buffer;
} __attribute__((packed, aligned(16)));
struct qh {
  volatile uint32_t next, element;
} __attribute__((packed, aligned(16)));
struct hid {
  struct qh queue;
  struct td transaction;
  uint8_t data[16] __attribute__((aligned(16)));
  uint8_t old_keys[6], modifiers;
  unsigned address, endpoint, packet, interval, protocol, toggle, last_tick;
  bool low_speed, ready;
};
static struct hid devices[8];
static unsigned hid_count, next_address;
static uint32_t frames[1024] __attribute__((aligned(4096)));
static struct qh control_queue;
static struct td transfers[514];
static uint8_t setup[8] __attribute__((aligned(16)));
static uint8_t control_data[512] __attribute__((aligned(16)));
static uint16_t port;
static bool running;
static uint32_t pointer(const void *p) { return (uintptr_t)p; }
static uint32_t token(unsigned pid, unsigned address, unsigned endpoint,
                      unsigned toggle, unsigned count) {
  return pid | (address << 8) | (endpoint << 15) | (toggle << 19) |
         (((count - 1) & 0x7ff) << 21);
}
static void td(struct td *t, unsigned pid, unsigned address, unsigned endpoint,
               unsigned toggle, unsigned count, void *data, bool low) {
  t->next = END;
  t->status = ACTIVE | (3u << 27) | (low ? LOW_SPEED : 0);
  t->token = token(pid, address, endpoint, toggle, count);
  t->buffer = pointer(data);
}
bool usb_control(unsigned address, bool low, unsigned packet,
                 uint8_t request_type, uint8_t request, uint16_t value,
                 uint16_t index, uint16_t size, bool input) {
  if (address>=128)
    return size<=sizeof(control_data) && xhci_control(address-128,request_type,
             request,value,index,control_data,size);
  if (size > sizeof(control_data) || !packet || packet > 64)
    return false;
  setup[0] = request_type;
  setup[1] = request;
  setup[2] = value;
  setup[3] = value >> 8;
  setup[4] = index;
  setup[5] = index >> 8;
  setup[6] = size;
  setup[7] = size >> 8;
  td(&transfers[0], 0x2d, address, 0, 0, 8, setup, low);
  unsigned count = 1, toggle = 1;
  for (unsigned offset = 0; offset < size;) {
    unsigned chunk = size - offset;
    if (chunk > packet)
      chunk = packet;
    td(&transfers[count++], input ? 0x69 : 0xe1, address, 0, toggle, chunk,
       control_data + offset, low);
    offset += chunk;
    toggle ^= 1;
  }
  td(&transfers[count++], input ? 0xe1 : 0x69, address, 0, 1, 0, setup, low);
  for (unsigned i = 0; i < count - 1; i++)
    transfers[i].next = pointer(&transfers[i + 1]);
  control_queue.element = pointer(transfers);
  __asm__ volatile("sfence" ::: "memory");
  uint32_t start = clock_ticks();
  while (!(control_queue.element & END)) {
    uint32_t element = control_queue.element;
    if (!(element & 1)) {
      struct td *current = (void *)(uintptr_t)(element & ~15u);
      if (current >= transfers && current < transfers + count &&
          (current->status & ERROR_BITS) && !(current->status & ACTIVE)) {
        control_queue.element = END;
        sleep_ticks(2);
        return false;
      }
    }
    if (clock_ticks() - start > 100) {
      io_out16(port, 0);
      running = false;
      return false;
    }
    cpu_pause();
  }
  __asm__ volatile("lfence" ::: "memory");
  for (unsigned i = 0; i < count; i++)
    if ((transfers[i].status & (ACTIVE | ERROR_BITS)) ||
        ((transfers[i].status & 0x7ff) != 7 && i == 0))
      return false;
  return true;
}
#define transfer usb_control
int usb_bulk(unsigned address, unsigned endpoint, unsigned packet,
             unsigned *toggle, void *data, unsigned size, bool input) {
  if (address>=128) return xhci_bulk(address-128,endpoint,data,size,input);
  if (!running || !size || size > 4096 || !endpoint || endpoint > 15 ||
      (packet != 8 && packet != 16 && packet != 32 && packet != 64))
    return -1;
  unsigned count = (size + packet - 1) / packet;
  if (count > sizeof(transfers) / sizeof(transfers[0]))
    return -1;
  for (unsigned i = 0; i < count; i++) {
    unsigned amount = size - i * packet;
    if (amount > packet)
      amount = packet;
    td(&transfers[i], input ? 0x69 : 0xe1, address, endpoint, (*toggle + i) & 1,
       amount, (uint8_t *)data + i * packet, false);
    if (input)
      transfers[i].status |= 1u << 29; /* Stop on a short packet. */
    if (i + 1 < count)
      transfers[i].next = pointer(&transfers[i + 1]) | 4;
  }
  __asm__ volatile("sfence" ::: "memory");
  control_queue.element = pointer(transfers);
  unsigned completed = 0, bytes = 0;
  uint32_t start = clock_ticks();
  while (completed < count) {
    /* HID queues run alongside bulk traffic; drain reports during disk I/O. */
    usb_poll();
    unsigned status = *(volatile uint32_t *)&transfers[completed].status;
    if (!(status & ACTIVE)) {
      __asm__ volatile("lfence" ::: "memory");
      if (status & ERROR_BITS) {
        control_queue.element = END;
        sleep_ticks(2);
        return -1;
      }
      unsigned amount = (status + 1) & 0x7ff;
      unsigned expected = size - completed * packet;
      if (expected > packet)
        expected = packet;
      if (amount > expected) {
        control_queue.element = END;
        sleep_ticks(1);
        return -1;
      }
      bytes += amount;
      completed++;
      *toggle ^= 1;
      if (amount < expected)
        break;
    } else if (clock_ticks() - start > 200) {
      io_out16(port, 0);
      running = false;
      return -1;
    } else
      cpu_pause();
  }
  control_queue.element = END;
  __asm__ volatile("sfence" ::: "memory");
  /* DMA descriptors stay alive; wait a frame before reusing their storage. */
  uint16_t frame = io_in16(port + 6);
  start = clock_ticks();
  while (io_in16(port + 6) == frame) {
    usb_poll();
    if (clock_ticks() - start > 2) {
      io_out16(port, 0);
      running = false;
      return -1;
    }
    cpu_pause();
  }
  return bytes;
}
static uint8_t scancode(unsigned usage) {
  static const uint8_t map[] = {
      [4] = 30,  [5] = 48,  [6] = 46,  [7] = 32,  [8] = 18,  [9] = 33,
      [10] = 34, [11] = 35, [12] = 23, [13] = 36, [14] = 37, [15] = 38,
      [16] = 50, [17] = 49, [18] = 24, [19] = 25, [20] = 16, [21] = 19,
      [22] = 31, [23] = 20, [24] = 22, [25] = 47, [26] = 17, [27] = 45,
      [28] = 21, [29] = 44, [30] = 2,  [31] = 3,  [32] = 4,  [33] = 5,
      [34] = 6,  [35] = 7,  [36] = 8,  [37] = 9,  [38] = 10, [39] = 11,
      [40] = 28, [41] = 1,  [42] = 14, [43] = 15, [44] = 57, [45] = 12,
      [46] = 13, [47] = 26, [48] = 27, [49] = 43, [51] = 39, [52] = 40,
      [53] = 41, [54] = 51, [55] = 52, [56] = 53, [57] = 58, [58] = 59,
      [59] = 60, [60] = 61, [61] = 62, [62] = 63, [63] = 64, [64] = 65,
      [65] = 66, [66] = 67, [67] = 68, [79] = 77, [80] = 75, [81] = 80,
      [82] = 72, [73] = 82, [74] = 71, [75] = 73, [76] = 83, [77] = 79,
      [78] = 81};
  return usage < sizeof(map) ? map[usage] : 0;
}
static bool present(const uint8_t *keys, uint8_t value) {
  for (unsigned i = 0; i < 6; i++)
    if (keys[i] == value)
      return true;
  return false;
}
static void keyboard_report(struct hid *device) {
  usb_keyboard_report(device->old_keys,&device->modifiers,device->data);
}
void usb_keyboard_report(uint8_t old_keys[6],uint8_t *old_modifiers,const uint8_t *data) {
  for (unsigned i=2;i<8;i++) if (data[i] && data[i]<=3) return;
  uint8_t modifiers = data[0];
  static const uint8_t modifier_codes[] = {29, 42, 56, 0, 29, 54, 56, 0};
  for (unsigned i = 0; i < 8; i++)
    if ((modifiers ^ *old_modifiers) & (1u << i)) {
      uint8_t code = modifier_codes[i];
      if (!code)
        continue;
      if (i == 4 || i == 6)
        keyboard_feed_scancode(0xe0);
      keyboard_feed_scancode((modifiers & (1u << i)) ? code : code | 0x80);
    }
  *old_modifiers = modifiers;
  for (unsigned i = 0; i < 6; i++) {
    uint8_t old = old_keys[i], now = data[2 + i];
    if (old && !present(data + 2, old)) {
      uint8_t code = scancode(old);
      if (code)
        keyboard_feed_scancode(code | 0x80);
    }
    if (now && now < 0x80 && !present(old_keys, now)) {
      uint8_t code = scancode(now);
      if (code)
        keyboard_feed_scancode(code);
    }
  }
  memcpy(old_keys, data + 2, 6);
}
static void prepare(struct hid *device) {
  td(&device->transaction, 0x69, device->address, device->endpoint,
     device->toggle, device->packet, device->data, device->low_speed);
  device->queue.element = pointer(&device->transaction);
  __asm__ volatile("sfence" ::: "memory");
}
void usb_poll(void) {
  xhci_poll();
  if (!running)
    return;
  for (unsigned i = 0; i < hid_count; i++) {
    struct hid *device = &devices[i];
    if (!device->ready)
      continue;
    if (device->transaction.status & ACTIVE)
      continue;
    __asm__ volatile("lfence" ::: "memory");
    unsigned status = device->transaction.status;
    if (!(status & ERROR_BITS)) {
      unsigned length = (status + 1) & 0x7ff;
      if (device->protocol == 1 && length >= 8)
        keyboard_report(device);
      if (device->protocol == 2 && length >= 3)
        mouse_feed_relative((int8_t)device->data[1], (int8_t)device->data[2],
                            device->data[0]);
      device->toggle ^= 1;
    }
    prepare(device);
  }
}
static bool enumerate_device(bool low, unsigned depth) {
  if (next_address >= 16)
    return false;
  if (!transfer(0, low, 8, 0x80, 6, 0x0100, 0, 8, true))
    return false;
  unsigned packet = control_data[7];
  if (packet != 8 && packet != 16 && packet != 32 && packet != 64)
    return false;
  unsigned address = ++next_address;
  if (!transfer(0, low, packet, 0, 5, address, 0, 0, false))
    return false;
  sleep_ticks(1);
  if (!transfer(address, low, packet, 0x80, 6, 0x0100, 0, 18, true))
    return false;
  unsigned device_class = control_data[4];
  if (!transfer(address, low, packet, 0x80, 6, 0x0200, 0, 9, true))
    return false;
  unsigned total = control_data[2] | ((unsigned)control_data[3] << 8);
  if (total < 9 || total > 256)
    return false;
  if (!transfer(address, low, packet, 0x80, 6, 0x0200, 0, total, true))
    return false;
  unsigned configuration = control_data[5], interface = 0, protocol = 0,
           interface_class = 0, endpoint = 0, length = 0, interval = 0;
  unsigned storage_interface = 0, bulk_in = 0, bulk_out = 0, input_packet = 0,
           output_packet = 0;
  bool storage = false;
  for (unsigned at = 0; at + 2 <= total;) {
    unsigned size = control_data[at], type = control_data[at + 1];
    if (size < 2 || size > total - at)
      return false;
    if (type == 4 && size >= 9) {
      interface_class = control_data[at + 5];
      protocol = (interface_class == 3 && control_data[at + 6] == 1)
                     ? control_data[at + 7]
                     : 0;
      interface = control_data[at + 2];
      storage = interface_class == 8 && control_data[at + 6] == 6 &&
                control_data[at + 7] == 0x50 && !control_data[at + 3];
      if (storage)
        storage_interface = interface;
    } else if (type == 5 && size >= 7 && storage &&
               (control_data[at + 3] & 3) == 2) {
      unsigned address = control_data[at + 2];
      unsigned packet =
          control_data[at + 4] | ((unsigned)control_data[at + 5] << 8);
      if (address & 0x80) {
        bulk_in = address & 15;
        input_packet = packet;
      } else {
        bulk_out = address & 15;
        output_packet = packet;
      }
    } else if (type == 5 && size >= 7 && protocol &&
               (control_data[at + 2] & 0x80) &&
               (control_data[at + 3] & 3) == 3) {
      endpoint = control_data[at + 2] & 15;
      length = control_data[at + 4] | ((unsigned)control_data[at + 5] << 8);
      interval = control_data[at + 6];
      break;
    }
    at += size;
  }
  if (!low && configuration && bulk_in && bulk_out) {
    if (!transfer(address, false, packet, 0, 9, configuration, 0, 0, false))
      return false;
    return usb_storage_attach(address, packet, storage_interface, bulk_in,
                              bulk_out, input_packet, output_packet);
  }
  if ((device_class == 9 || interface_class == 9) && configuration &&
      depth < 2) {
    if (!transfer(address, low, packet, 0, 9, configuration, 0, 0, false))
      return false;
    if (!transfer(address, low, packet, 0xa0, 6, 0x2900, 0, 9, true))
      return false;
    unsigned ports = control_data[2];
    if (ports > 8)
      ports = 8;
    console_write("USB hub ready\n");
    for (unsigned child = 1; child <= ports; child++) {
      transfer(address, low, packet, 0x23, 3, 8, child, 0, false);
      sleep_ticks(2);
      if (!transfer(address, low, packet, 0xa3, 0, 0, child, 4, true) ||
          !(control_data[0] & 1))
        continue;
      if (!transfer(address, low, packet, 0x23, 3, 4, child, 0, false))
        continue;
      sleep_ticks(5);
      transfer(address, low, packet, 0x23, 1, 4, child, 0, false);
      if (!transfer(address, low, packet, 0xa3, 0, 0, child, 4, true))
        continue;
      bool child_low = control_data[1] & 2;
      enumerate_device(child_low, depth + 1);
    }
    return true;
  }
  if ((protocol != 1 && protocol != 2) || !endpoint || !length || length > 16 ||
      !configuration)
    return false;
  if (!transfer(address, low, packet, 0, 9, configuration, 0, 0, false))
    return false;
  if (!transfer(address, low, packet, 0x21, 11, 0, interface, 0, false))
    return false;
  if (hid_count >= 8)
    return false;
  struct hid *device = &devices[hid_count++];
  device->address = address;
  device->low_speed = low;
  device->endpoint = endpoint;
  device->packet = length;
  device->interval = interval;
  device->protocol = protocol;
  device->toggle = 0;
  device->ready = true;
  console_write(protocol == 1 ? "USB keyboard ready\n" : "USB mouse ready\n");
  return true;
}
static bool enumerate_port(unsigned slot) {
  uint16_t reg = port + 0x10 + slot * 2;
  if (!(io_in16(reg) & 1))
    return false;
  io_out16(reg, io_in16(reg) | 0x200);
  sleep_ticks(10);
  io_out16(reg, io_in16(reg) & ~0x200);
  sleep_ticks(2);
  if (!(io_in16(reg) & 1))
    return false;
  io_out16(reg, io_in16(reg) | 4);
  sleep_ticks(2);
  uint16_t state = io_in16(reg);
  if (!(state & 4))
    return false;
  return enumerate_device(state & 0x100, 0);
}
void usb_init(void) {
  xhci_init();
  const struct pci_device *controller = pci_find_class(12, 3, 0);
  if (!controller || !(controller->bar[4] & 1))
    return;
  port = controller->bar[4] & ~3u;
  console_write("UHCI controller found\n");
  pci_enable(controller, 5); /* I/O decoding and DMA bus mastering. */
  io_out16(port, 2);
  sleep_ticks(2);
  io_out16(port, 0);
  memset(frames, 0, sizeof(frames));
  control_queue.next = END;
  control_queue.element = END;
  for (unsigned i = 0; i < 1024; i++)
    frames[i] = pointer(&control_queue) | 2;
  io_out16(port + 4, 0);
  io_out16(port + 6, 0);
  io_out32(port + 8, pointer(frames));
  io_out16(port, 1);
  running = true;
  for (unsigned slot = 0; slot < 2; slot++)
    enumerate_port(slot);
  struct qh *last = &control_queue;
  for (unsigned i = 0; i < hid_count; i++)
    if (devices[i].ready) {
      last->next = pointer(&devices[i].queue) | 2;
      devices[i].queue.next = END;
      prepare(&devices[i]);
      last = &devices[i].queue;
    }
}
