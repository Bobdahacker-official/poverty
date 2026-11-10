#include "usb.h"
#define SLOTS 8
#define RING_SIZE 64
struct trb {
  uint64_t parameter;
  uint32_t status;
  volatile uint32_t control;
};
struct ring {
  struct trb trb[RING_SIZE];
  unsigned tail, cycle;
} __attribute__((aligned(64)));
struct endpoint {
  struct ring ring;
  uint64_t awaited;
  unsigned expected, actual, code;
  bool pending, configured;
};
struct device {
  uint8_t output[2048] __attribute__((aligned(64)));
  uint8_t input[2112] __attribute__((aligned(64)));
  struct endpoint ep[32];
  uint8_t descriptor[512] __attribute__((aligned(64)));
  uint8_t report[16] __attribute__((aligned(64)));
  uint8_t old_keys[6], modifiers;
  unsigned slot, port, speed, packet, hid, hid_ep, hid_packet;
  unsigned route, depth, tt_slot, tt_port;
  unsigned hub_ports, hub_connected;
  bool used, hid_pending;
};
static struct device devices[SLOTS];
static struct ring commands;
static struct trb events[256] __attribute__((aligned(4096)));
static struct {
  uint64_t base;
  uint32_t size, reserved;
} segment __attribute__((aligned(64)));
static uint64_t dcbaa[256] __attribute__((aligned(64)));
static uint64_t scratch_list[1024] __attribute__((aligned(4096)));
static uint8_t scratch[1024][4096] __attribute__((aligned(4096)));
static uint8_t bounce[4096] __attribute__((aligned(4096)));
static volatile uint32_t *cap, *op, *runtime, *doorbells;
static unsigned context_bytes, event_head, event_cycle, ports;
static uint64_t awaited;
static unsigned command_code, command_slot;
static bool running, polling, command_done;
static bool port_present[64], port_pending[64];
static uint32_t port_changed[64];
static uint32_t read_reg(volatile uint32_t *base, unsigned offset) {
  return base[offset / 4];
}
static void write_reg(volatile uint32_t *base, unsigned offset,
                      uint32_t value) {
  base[offset / 4] = value;
}
static void write64(volatile uint32_t *base, unsigned offset, uint64_t value) {
  write_reg(base, offset, value);
  write_reg(base, offset + 4, value >> 32);
}
static bool wait_bits(volatile uint32_t *base, unsigned offset, uint32_t mask,
                      uint32_t value, unsigned timeout) {
  uint32_t start = clock_ticks();
  while ((read_reg(base, offset) & mask) != value) {
    if (clock_ticks() - start > timeout)
      return false;
    cpu_pause();
  }
  return true;
}
static void ring_init(struct ring *ring) {
  memset(ring, 0, sizeof(*ring));
  ring->cycle = 1;
  ring->trb[RING_SIZE - 1].parameter = page_physical(ring->trb);
  ring->trb[RING_SIZE - 1].control = (6u << 10) | 2;
}
static uint64_t enqueue(struct ring *ring, uint64_t parameter, uint32_t status,
                        uint32_t control) {
  struct trb *trb = &ring->trb[ring->tail];
  trb->parameter = parameter;
  trb->status = status;
  __asm__ volatile("sfence" ::: "memory");
  trb->control = control | ring->cycle;
  uint64_t address = page_physical(trb);
  if (++ring->tail == RING_SIZE - 1) {
    __asm__ volatile("sfence" ::: "memory");
    ring->trb[RING_SIZE - 1].control = (6u << 10) | 2 | ring->cycle;
    ring->tail = 0;
    ring->cycle ^= 1;
  }
  return address;
}
static struct device *find(unsigned slot) {
  for (unsigned i = 0; i < SLOTS; i++)
    if (devices[i].used && devices[i].slot == slot)
      return &devices[i];
  return NULL;
}
static void arm_hid(struct device *device) {
  struct endpoint *ep = &device->ep[device->hid_ep];
  ep->pending = true;
  ep->expected = device->hid_packet;
  ep->code = 0;
  ep->awaited = enqueue(&ep->ring, page_physical(device->report),
                        device->hid_packet, (1u << 10) | 32 | 4);
  __asm__ volatile("sfence" ::: "memory");
  doorbells[device->slot] = device->hid_ep;
  device->hid_pending = true;
}
void xhci_poll(void) {
  if (!running || polling)
    return;
  polling = true;
  for (unsigned n = 0; n < 256; n++) {
    struct trb *event = &events[event_head];
    uint32_t control = event->control;
    if ((control & 1) != event_cycle)
      break;
    __asm__ volatile("lfence" ::: "memory");
    unsigned type = (control >> 10) & 63, code = event->status >> 24;
    if (type == 33 && event->parameter == awaited) {
      command_code = code;
      command_slot = control >> 24;
      command_done = true;
    } else if (type == 32) {
      struct device *device = find(control >> 24);
      unsigned endpoint = (control >> 16) & 31;
      if (device && endpoint) {
        struct endpoint *ep = &device->ep[endpoint];
        if (!ep->pending ||
            (event->parameter != ep->awaited && (code == 1 || code == 13))) {
          if (++event_head == 256) {
            event_head = 0;
            event_cycle ^= 1;
          }
          write64(runtime, 0x38, page_physical(&events[event_head]) | 8);
          continue;
        }
        unsigned residue = event->status & 0xffffff;
        ep->actual = residue <= ep->expected ? ep->expected - residue : 0;
        ep->code = code;
        ep->pending = false;
      }
    }
    if (++event_head == 256) {
      event_head = 0;
      event_cycle ^= 1;
    }
    write64(runtime, 0x38, page_physical(&events[event_head]) | 8);
  }
  for (unsigned i = 0; i < SLOTS; i++) {
    struct device *device = &devices[i];
    if (!device->used || !device->hid_pending)
      continue;
    struct endpoint *ep = &device->ep[device->hid_ep];
    if (ep->pending)
      continue;
    device->hid_pending = false;
    if (ep->code != 1 && ep->code != 13)
      continue;
    if (device->hid == 1 && ep->actual >= 8)
      usb_keyboard_report(device->old_keys, &device->modifiers, device->report);
    else if (device->hid == 2 && ep->actual >= 3)
      mouse_feed_relative((int8_t)device->report[1], (int8_t)device->report[2],
                          device->report[0]);
    arm_hid(device);
  }
  polling = false;
}
static bool command(uint64_t parameter, uint32_t control) {
  if (!running)
    return false;
  command_done = false;
  awaited = enqueue(&commands, parameter, 0, control);
  __asm__ volatile("sfence" ::: "memory");
  doorbells[0] = 0;
  uint32_t start = clock_ticks();
  while (!command_done) {
    xhci_poll();
    if (clock_ticks() - start > 200) {
      write_reg(op, 0, read_reg(op, 0) & ~1u);
      (void)wait_bits(op, 4, 1, 1, 100);
      running = false;
      return false;
    }
    cpu_pause();
  }
  return command_code == 1;
}
static bool complete(struct device *device, unsigned endpoint) {
  __asm__ volatile("sfence" ::: "memory");
  doorbells[device->slot] = endpoint;
  uint32_t start = clock_ticks();
  while (device->ep[endpoint].pending) {
    xhci_poll();
    if (clock_ticks() - start > 200) {
      /* Stop DMA before any queue or transfer buffer is reused. */
      write_reg(op, 0, read_reg(op, 0) & ~1u);
      (void)wait_bits(op, 4, 1, 1, 100);
      running = false;
      return false;
    }
    cpu_pause();
  }
  if (device->ep[endpoint].code == 1 || device->ep[endpoint].code == 13)
    return true;
  console_write("xHCI transfer error: slot ");
  print_uint(device->slot);
  console_write(" endpoint ");
  print_uint(endpoint);
  console_write(" completion ");
  print_uint(device->ep[endpoint].code);
  console_putc('\n');
  return false;
}
bool xhci_control(unsigned slot, uint8_t type, uint8_t request, uint16_t value,
                  uint16_t index, void *data, unsigned size) {
  struct device *device = find(slot);
  if (!running || !device || size > 512)
    return false;
  struct endpoint *ep = &device->ep[1];
  ep->expected = size;
  ep->pending = true;
  ep->code = 0;
  uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 |
                   (uint64_t)index << 32 | (uint64_t)size << 48;
  bool input = type & 0x80;
  enqueue(&ep->ring, setup, 8,
          (2u << 10) | 64 | (size ? (input ? 3u : 2u) << 16 : 0));
  if (size)
    enqueue(&ep->ring, page_physical(data), size,
            (3u << 10) | (input ? 1u << 16 : 0));
  ep->awaited = enqueue(&ep->ring, 0, 0,
                        (4u << 10) | 32 | (!size || !input ? 1u << 16 : 0));
  if (!complete(device, 1))
    return false;
  if (type == 2 && request == 1 && !value) {
    unsigned dci = (index & 15) * 2 + ((index & 0x80) ? 1 : 0);
    if (dci < 2 || dci >= 32)
      return false;
    struct endpoint *target = &device->ep[dci];
    unsigned state = (*(uint32_t *)(device->output + dci * context_bytes)) & 7;
    if (state == 2) {
      if (!command(0, (14u << 10) | (dci << 16) | (slot << 24)))
        return false;
      ring_init(&target->ring);
      if (!command(page_physical(target->ring.trb) | 1,
                   (16u << 10) | (dci << 16) | (slot << 24)))
        return false;
      target->pending = false;
    }
  }
  return true;
}
int xhci_bulk(unsigned slot, unsigned endpoint, void *data, unsigned size,
              bool input) {
  struct device *device = find(slot);
  unsigned dci = endpoint * 2 + (input ? 1 : 0);
  if (!running || !device || !endpoint || dci >= 32 || !size ||
      size > sizeof(bounce))
    return -1;
  struct endpoint *ep = &device->ep[dci];
  if (!ep->configured)
    return -1;
  if (!input)
    memcpy(bounce, data, size);
  ep->expected = size;
  ep->pending = true;
  ep->code = 0;
  ep->awaited =
      enqueue(&ep->ring, page_physical(bounce), size, (1u << 10) | 32 | 4);
  if (!complete(device, dci))
    return -1;
  if (input)
    memcpy(data, bounce, ep->actual);
  return ep->actual;
}
static uint32_t *input_context(struct device *device, unsigned index) {
  return (void *)(device->input + (index + 1) * context_bytes);
}
static void endpoint_context(struct device *device, unsigned dci, unsigned type,
                             unsigned packet, unsigned interval) {
  struct endpoint *ep = &device->ep[dci];
  ring_init(&ep->ring);
  ep->configured = true;
  uint32_t *ctx = input_context(device, dci);
  ctx[0] = interval << 16;
  ctx[1] = (3u << 1) | (type << 3) | (packet << 16);
  uint64_t ptr = page_physical(ep->ring.trb) | 1;
  ctx[2] = ptr;
  ctx[3] = ptr >> 32;
  ctx[4] = packet | ((type == 7 ? packet : 0) << 16);
}
static bool descriptor(struct device *device, unsigned kind, unsigned size) {
  memset(device->descriptor, 0, sizeof(device->descriptor));
  return xhci_control(device->slot, 0x80, 6, kind << 8, 0, device->descriptor,
                      size);
}
static bool enumerate(unsigned port, unsigned speed, unsigned route,
                      unsigned depth, unsigned tt_slot, unsigned tt_port);
static bool hub_child_start(struct device *hub, unsigned child) {
  unsigned slot = hub->slot;
  unsigned route = hub->route | (child << (hub->depth * 4));
  for (unsigned i = 0; i < SLOTS; i++)
    if (devices[i].used && devices[i].port == hub->port &&
        devices[i].route == route && devices[i].depth == hub->depth + 1)
      return false;
  if (!xhci_control(slot, 0x23, 3, 4, child, NULL, 0))
    return false;
  for (unsigned tries = 0; tries < 20; tries++) {
    sleep_ticks(1);
    if (!xhci_control(slot, 0xa3, 0, 0, child, hub->descriptor, 4))
      return false;
    if ((hub->descriptor[0] & 3) != 3 || (hub->descriptor[0] & 16))
      continue;
    unsigned speed = hub->speed >= 4 ? 4
                     : (hub->descriptor[1] & 4) ? 3
                     : (hub->descriptor[1] & 2) ? 2 : 1;
    (void)xhci_control(slot, 0x23, 1, 20, child, NULL, 0);
    unsigned tt_slot = hub->tt_slot, tt_port = hub->tt_port;
    if (hub->speed == 3 && speed <= 2) {
      tt_slot = slot;
      tt_port = child;
    }
    return enumerate(hub->port, speed, route, hub->depth + 1, tt_slot, tt_port);
  }
  return false;
}
static bool enumerate(unsigned port, unsigned speed, unsigned route,
                      unsigned depth, unsigned tt_slot, unsigned tt_port) {
  struct device *device = NULL;
  for (unsigned i = 0; i < SLOTS; i++)
    if (!devices[i].used) {
      device = &devices[i];
      break;
    }
  if (!device || !command(0, 9u << 10))
    return false;
  unsigned slot = command_slot;
  memset(device, 0, sizeof(*device));
  device->used = true;
  device->slot = slot;
  device->port = port;
  device->speed = speed;
  device->route = route;
  device->depth = depth;
  device->tt_slot = tt_slot;
  device->tt_port = tt_port;
  device->packet = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
  dcbaa[slot] = page_physical(device->output);
  uint32_t *flags = (void *)device->input;
  flags[1] = 3;
  uint32_t *ctx = input_context(device, 0);
  ctx[0] = route | (speed << 20) | (1u << 27);
  ctx[1] = port << 16;
  ctx[2] = tt_slot | (tt_port << 8);
  endpoint_context(device, 1, 4, device->packet, 0);
  if (!command(page_physical(device->input), (11u << 10) | (slot << 24)))
    goto failed;
  if (!descriptor(device, 1, 8))
    goto failed;
  if (speed >= 4 && device->descriptor[7] != 9)
    goto failed;
  unsigned packet = speed >= 4 ? 512 : device->descriptor[7];
  if (packet != 8 && packet != 16 && packet != 32 && packet != 64 &&
      packet != 512)
    goto failed;
  if (packet != device->packet) {
    memset(device->input, 0, sizeof(device->input));
    flags[1] = 2;
    ctx = input_context(device, 1);
    memcpy(ctx, device->output + context_bytes, context_bytes);
    ctx[1] = (ctx[1] & 65535u) | (packet << 16);
    if (!command(page_physical(device->input), (13u << 10) | (slot << 24)))
      goto failed;
    device->packet = packet;
  }
  if (!descriptor(device, 1, 18))
    goto failed;
  unsigned device_class = device->descriptor[4],
           device_protocol = device->descriptor[6];
  if (!descriptor(device, 2, 9))
    goto failed;
  unsigned total = device->descriptor[2] | (unsigned)device->descriptor[3] << 8;
  if (total < 9 || total > 512 || !descriptor(device, 2, total))
    goto failed;
  unsigned configuration = device->descriptor[5], interface = 0, kind = 0,
           protocol = 0;
  unsigned in = 0, out = 0, in_packet = 0, out_packet = 0, max_dci = 1;
  memset(device->input, 0, sizeof(device->input));
  flags[1] = 1;
  memcpy(input_context(device, 0), device->output, context_bytes);
  for (unsigned at = 0; at + 2 <= total;) {
    uint8_t *d = device->descriptor + at;
    if (d[0] < 2 || d[0] > total - at)
      goto failed;
    if (d[1] == 4 && d[0] >= 9) {
      if (in || out)
        break;
      interface = d[2];
      kind = 0;
      protocol = d[7];
      if (!d[3] && d[5] == 3 && d[6] == 1 && (protocol == 1 || protocol == 2))
        kind = 3;
      if (!d[3] && d[5] == 8 && d[6] == 6 && protocol == 0x50)
        kind = 8;
      if (!d[3] && (d[5] == 9 || device_class == 9))
        kind = 9;
    } else if (d[1] == 5 && d[0] >= 7 && kind) {
      unsigned number = d[2] & 15, packet = (d[4] | (unsigned)d[5] << 8) & 2047;
      bool input = d[2] & 0x80;
      unsigned type = d[3] & 3, dci = number * 2 + (input ? 1 : 0),
               interval = 0;
      if (!number || !packet || packet > 1024)
        goto failed;
      if ((kind == 8 && type == 2) ||
          ((kind == 3 || kind == 9) && type == 3 && input && packet <= 16)) {
        if (type == 3) {
          if (speed >= 3)
            interval = d[6] ? d[6] - 1 : 0;
          else {
            unsigned frames = 1;
            interval = 3;
            while (frames < d[6] && interval < 10) {
              frames *= 2;
              interval++;
            }
          }
          if (interval > 15)
            goto failed;
        }
        endpoint_context(device, dci, type == 2 ? (input ? 6 : 2) : 7, packet,
                         interval);
        flags[1] |= 1u << dci;
        if (dci > max_dci)
          max_dci = dci;
        if (input) {
          in = number;
          in_packet = packet;
        } else {
          out = number;
          out_packet = packet;
        }
      }
    }
    at += d[0];
  }
  if (!configuration || !in || (kind == 8 && !out))
    goto failed;
  ctx = input_context(device, 0);
  ctx[0] = (ctx[0] & ~(31u << 27)) | (max_dci << 27);
  if (!command(page_physical(device->input), (12u << 10) | (slot << 24)) ||
      !xhci_control(slot, 0, 9, configuration, 0, NULL, 0))
    goto failed;
  if (kind == 9) {
    if (depth >= 5 ||
        !xhci_control(slot, 0xa0, 6, (speed >= 4 ? 0x2a : 0x29) << 8, 0,
                      device->descriptor, speed >= 4 ? 12 : 9))
      goto failed;
    unsigned children = device->descriptor[2],
             power_delay = device->descriptor[5];
    if (!children || children > 15)
      goto failed;
    memset(device->input, 0, sizeof(device->input));
    flags[1] = 1;
    ctx = input_context(device, 0);
    memcpy(ctx, device->output, context_bytes);
    ctx[0] |= 1u << 26;
    if (speed == 3 && device_protocol == 2)
      ctx[0] |= 1u << 25;
    ctx[1] = (ctx[1] & 0x00ffffff) | (children << 24);
    if (!command(page_physical(device->input), (13u << 10) | (slot << 24)))
      goto failed;
    if (speed >= 4 && !xhci_control(slot, 0x20, 12, depth, 0, NULL, 0))
      goto failed;
    for (unsigned child = 1; child <= children; child++)
      (void)xhci_control(slot, 0x23, 3, 8, child, NULL, 0);
    sleep_ticks((power_delay + 4) / 5 + 2);
    device->hub_ports = children;
    console_write("xHCI USB hub ready\n");
    for (unsigned child = 1; child <= children; child++) {
      if (!xhci_control(slot, 0xa3, 0, 0, child, device->descriptor, 4) ||
          !(device->descriptor[0] & 1))
        continue;
      device->hub_connected |= 1u << child;
      (void)xhci_control(slot, 0x23, 1, 16, child, NULL, 0);
      (void)hub_child_start(device, child);
    }
  } else if (kind == 8) {
    if (!usb_storage_attach(128 + slot, device->packet, interface, in, out,
                            in_packet, out_packet))
      goto failed;
  } else {
    if (!xhci_control(slot, 0x21, 11, 0, interface, NULL, 0))
      goto failed;
    device->hid = protocol;
    device->hid_ep = in * 2 + 1;
    device->hid_packet = in_packet;
    arm_hid(device);
    console_write(protocol == 1 ? "xHCI USB keyboard ready\n"
                                : "xHCI USB mouse ready\n");
  }
  return true;
failed:
  if (command(0, (10u << 10) | (slot << 24))) {
    dcbaa[slot] = 0;
    device->used = false;
  }
  return false;
}
static bool start_port(unsigned port, uint32_t status) {
  unsigned offset = 0x400 + (port - 1) * 16;
  if (!(status & 1)) return false;
  if (!(status & 2)) {
    write_reg(op, offset, (status & 0x0e00c200u) | (1u << 9) | 16);
    if (!wait_bits(op, offset, 16, 0, 100)) return false;
    status = read_reg(op, offset);
  }
  return (status & 3) == 3 &&
         enumerate(port, (status >> 10) & 15, 0, 0, 0, 0);
}
static void detach_branch(unsigned port, unsigned route, unsigned depth) {
  unsigned route_mask = (1u << (depth * 4)) - 1;
  for (int level = 5; level >= (int)depth; level--)
    for (unsigned i = 0; i < SLOTS; i++) {
      struct device *device = &devices[i];
      if (!device->used || device->port != port ||
          device->depth != (unsigned)level ||
          (device->route & route_mask) != route)
        continue;
      device->hid_pending = false;
      device->hub_ports = 0;
      usb_storage_detach(128 + device->slot);
      if (command(0, (10u << 10) | (device->slot << 24))) {
        dcbaa[device->slot] = 0;
        device->used = false;
      }
    }
}
static void service_hubs(void) {
  static uint32_t last_poll;
  uint32_t now = clock_ticks();
  if ((uint32_t)(now - last_poll) < 25) return;
  last_poll = now;
  for (unsigned i = 0; i < SLOTS; i++) {
    struct device *hub = &devices[i];
    if (!hub->used || !hub->hub_ports) continue;
    for (unsigned child = 1; child <= hub->hub_ports; child++) {
      if (!xhci_control(hub->slot, 0xa3, 0, 0, child, hub->descriptor, 4))
        break;
      unsigned bit = 1u << child;
      bool connected = hub->descriptor[0] & 1;
      bool was_connected = (hub->hub_connected & bit) != 0;
      bool changed = hub->descriptor[2] & 1;
      if (!changed && connected == was_connected) continue;
      if (changed) (void)xhci_control(hub->slot, 0x23, 1, 16, child, NULL, 0);
      unsigned route = hub->route | (child << (hub->depth * 4));
      if (was_connected) detach_branch(hub->port, route, hub->depth + 1);
      hub->hub_connected &= ~bit;
      if (connected) {
        hub->hub_connected |= bit;
        (void)hub_child_start(hub, child);
      }
    }
  }
}
void xhci_service_ports(void) {
  if (!running || polling) return;
  for (unsigned port = 1; port <= ports; port++) {
    unsigned index = port - 1, offset = 0x400 + index * 16;
    uint32_t status = read_reg(op, offset);
    bool connected = status & 1;
    if (connected != port_present[index] || (status & (1u << 17))) {
      if (status & (1u << 17))
        write_reg(op, offset, (status & 0x0e00c200u) | (1u << 17));
      if (port_present[index]) detach_branch(port, 0, 0);
      port_present[index] = connected;
      port_pending[index] = connected;
      port_changed[index] = clock_ticks();
      continue;
    }
    if (port_pending[index] &&
        (uint32_t)(clock_ticks() - port_changed[index]) >= 10) {
      port_pending[index] = false;
      bool occupied = false;
      for (unsigned i = 0; i < SLOTS; i++)
        occupied |= devices[i].used && devices[i].port == port;
      if (!occupied) (void)start_port(port, status);
    }
  }
  service_hubs();
}
void xhci_init(void) {
  const struct pci_device *pci = pci_find_class(12, 3, 0x30);
  uint64_t base = pci_memory_bar(pci, 0);
  if (!base || !platform_mmio(base, 0x10000))
    return;
  pci_enable(pci, 6);
  cap = page_pointer(base);
  unsigned length = read_reg(cap, 0) & 255;
  uint32_t hcs = read_reg(cap, 4), hcc = read_reg(cap, 16);
  ports = hcs >> 24;
  unsigned dboff = read_reg(cap, 20) & ~3u, rtoff = read_reg(cap, 24) & ~31u;
  if (length < 32 || !ports || ports > 64 ||
      !platform_mmio(base, length + 0x400 + ports * 16) ||
      !platform_mmio(base + dboff, 1024) || !platform_mmio(base + rtoff, 0x40))
    return;
  op = page_pointer(base + length);
  runtime = page_pointer(base + rtoff);
  doorbells = page_pointer(base + dboff);
  context_bytes = hcc & 4 ? 64 : 32;
  unsigned ext = (hcc >> 16) * 4;
  for (unsigned i = 0; ext && i < 64; i++) {
    if (!platform_mmio(base + ext, 8))
      return;
    volatile uint32_t *legacy = page_pointer(base + ext);
    uint32_t value = legacy[0];
    if ((value & 255) == 1) {
      legacy[0] = value | (1u << 24);
      if (!wait_bits(legacy, 0, 1u << 16, 0, 200))
        return;
      legacy[1] = 0;
    }
    unsigned next = (value >> 8) & 255;
    if (!next)
      break;
    ext += next * 4;
  }
  write_reg(op, 0, read_reg(op, 0) & ~1u);
  if (!wait_bits(op, 4, 1, 1, 100))
    return;
  write_reg(op, 0, 2);
  if (!wait_bits(op, 0, 2, 0, 100) || !wait_bits(op, 4, 1u << 11, 0, 100) ||
      !(read_reg(op, 8) & 1))
    return;
  uint32_t params = read_reg(cap, 8);
  unsigned pads = ((params >> 27) & 31) | (((params >> 21) & 31) << 5);
  if (pads >= 1024)
    return;
  for (unsigned i = 0; i < pads; i++)
    scratch_list[i] = page_physical(scratch[i]);
  if (pads)
    dcbaa[0] = page_physical(scratch_list);
  ring_init(&commands);
  event_cycle = 1;
  segment.base = page_physical(events);
  segment.size = 256;
  write64(op, 0x30, page_physical(dcbaa));
  write64(op, 0x18, page_physical(commands.trb) | 1);
  write_reg(op, 0x38, (hcs & 255) < SLOTS ? hcs & 255 : SLOTS);
  write_reg(runtime, 0x28, 1);
  write64(runtime, 0x30, page_physical(&segment));
  write64(runtime, 0x38, page_physical(events));
  write_reg(runtime, 0x20, 0);
  write_reg(op, 0, 1);
  if (!wait_bits(op, 4, 1, 0, 100))
    return;
  running = true;
  console_write("xHCI controller ready\n");
  for (unsigned port = 1; port <= ports; port++) {
    unsigned offset = 0x400 + (port - 1) * 16;
    uint32_t status = read_reg(op, offset);
    if ((hcc & 8) && !(status & (1u << 9))) {
      write_reg(op, offset, (status & 0x0e00c200u) | (1u << 9));
      status = read_reg(op, offset);
    }
    port_present[port - 1] = status & 1;
    if (status & (1u << 17))
      write_reg(op, offset, (status & 0x0e00c200u) | (1u << 17));
    if (status & 1) (void)start_port(port, status);
  }
}
