#include "system.h"
/* Poll-driven Ethernet. DMA memory is identity mapped and supervisor-only. */
static enum { NONE, RTL8139, E1000 } nic;
static uint16_t rtl_port;
static volatile uint32_t *e1000;
static uint8_t mac[6];
static bool link_ready;
static uint8_t rtl_rx[8192 + 16 + 1536] __attribute__((aligned(4096)));
static uint8_t tx_buffers[4][2048] __attribute__((aligned(16)));
static uint32_t rtl_offset, tx_index;
struct rx_desc {
  uint64_t address;
  uint16_t length, checksum;
  uint8_t status, errors;
  uint16_t special;
} __attribute__((packed));
struct tx_desc {
  uint64_t address;
  uint16_t length;
  uint8_t cso, cmd, status, css;
  uint16_t special;
} __attribute__((packed));
static volatile struct rx_desc rx_ring[32] __attribute__((aligned(128)));
static volatile struct tx_desc tx_ring[8] __attribute__((aligned(128)));
static uint8_t rx_buffers[32][2048] __attribute__((aligned(16)));
static uint8_t e_tx[8][2048] __attribute__((aligned(16)));
static uint32_t rx_index;
static uint32_t ip, mask, gateway, dns_server, rx_count, tx_count, drop_count;
static uint16_t ip_id;
static uint32_t rd(uint32_t r) { return e1000[r / 4]; }
static void wr(uint32_t r, uint32_t v) {
  e1000[r / 4] = v;
  (void)e1000[2];
}
static uint16_t be16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] << 8) | p[1];
}
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n) {
  p[0] = n >> 8;
  p[1] = n;
}
static void put32(uint8_t *p, uint32_t n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static uint16_t checksum(const uint8_t *p, uint32_t n) {
  uint32_t sum = 0;
  while (n > 1) {
    sum += be16(p);
    p += 2;
    n -= 2;
  }
  if (n)
    sum += (uint32_t)*p << 8;
  while (sum >> 16)
    sum = (sum & 65535) + (sum >> 16);
  return (uint16_t)~sum;
}
static bool same_mac(const uint8_t *a, const uint8_t *b) {
  for (unsigned i = 0; i < 6; i++)
    if (a[i] != b[i])
      return false;
  return true;
}
static bool ethernet_send(const void *frame, uint32_t n) {
  if (!link_ready || n > 1514 || n < 14)
    return false;
  uint32_t size = n < 60 ? 60 : n;
  if (nic == RTL8139) {
    unsigned index = tx_index++ % 4;
    uint32_t start = clock_ticks();
    while (!(io_in32(rtl_port + 0x10 + index * 4) & (1 << 13))) {
      if (clock_ticks() - start > 100)
        return false;
      cpu_pause();
    }
    memset(tx_buffers[index], 0, size);
    memcpy(tx_buffers[index], frame, n);
    __asm__ volatile("" ::: "memory");
    io_out32(rtl_port + 0x20 + index * 4, (uint32_t)tx_buffers[index]);
    io_out32(rtl_port + 0x10 + index * 4, size);
  } else {
    unsigned index = tx_index % 8;
    uint32_t start = clock_ticks();
    while (!(tx_ring[index].status & 1)) {
      if (clock_ticks() - start > 100)
        return false;
      cpu_pause();
    }
    memset(e_tx[index], 0, size);
    memcpy(e_tx[index], frame, n);
    tx_ring[index].length = size;
    tx_ring[index].cmd = 0x0b;
    tx_ring[index].status = 0;
    __asm__ volatile("" ::: "memory");
    tx_index = (index + 1) % 8;
    wr(0x3818, tx_index);
  }
  tx_count++;
  return true;
}
static bool rtl_init(const struct pci_device *d) {
  if (!(d->bar[0] & 1))
    return false;
  rtl_port = (uint16_t)(d->bar[0] & ~3u);
  pci_enable(d, 5);
  io_out8(rtl_port + 0x52, 0);
  io_out8(rtl_port + 0x37, 0x10);
  for (unsigned i = 0; i < 1000000; i++) {
    if (!(io_in8(rtl_port + 0x37) & 0x10))
      break;
    if (i == 999999)
      return false;
  }
  for (unsigned i = 0; i < 6; i++)
    mac[i] = io_in8(rtl_port + i);
  io_out32(rtl_port + 0x30, (uint32_t)rtl_rx);
  io_out16(rtl_port + 0x3c, 0);
  io_out16(
      rtl_port + 0x3e,
      0xffff); /* APM + AB; WRAP allows contiguous reads across ring end. */
  io_out32(rtl_port + 0x44, (7 << 8) | (1 << 7) | 0x0a);
  io_out32(rtl_port + 0x40, 0x03000700);
  io_out8(rtl_port + 0x37, 0x0c);
  nic = RTL8139;
  return true;
}
static bool e1000_init(const struct pci_device *d) {
  if (d->bar[0] & 1)
    return false;
  if ((d->bar[0] & 6) == 4 && d->bar[1])
    return false;
  if (!platform_mmio(d->bar[0] & ~15u, 0x20000))
    return false;
  e1000 = (void *)(d->bar[0] & ~15u);
  if (!e1000)
    return false;
  pci_enable(d, 6);
  wr(0xd8, 0xffffffff);
  wr(0, rd(0) | (1 << 26));
  for (unsigned i = 0; i < 1000000; i++) {
    if (!(rd(0) & (1 << 26)))
      break;
    if (i == 999999)
      return false;
  }
  wr(0xd8, 0xffffffff);
  wr(0, rd(0) | (1 << 6));
  uint32_t low = rd(0x5400), high = rd(0x5404);
  if (!(high & 0x80000000u)) {
    for (unsigned word = 0; word < 3; word++) {
      wr(0x14, 1 | (word << 8));
      uint32_t v = 0;
      unsigned tries;
      for (tries = 0; tries < 100000; tries++) {
        v = rd(0x14);
        if (v & 0x10)
          break;
      }
      if (tries == 100000)
        return false;
      mac[word * 2] = v >> 16;
      mac[word * 2 + 1] = v >> 24;
    }
    low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
          ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    high = mac[4] | ((uint32_t)mac[5] << 8) | 0x80000000u;
    wr(0x5400, low);
    wr(0x5404, high);
  } else {
    for (unsigned i = 0; i < 4; i++)
      mac[i] = low >> (i * 8);
    mac[4] = high;
    mac[5] = high >> 8;
  }
  for (unsigned i = 0; i < 128; i++)
    wr(0x5200 + i * 4, 0);
  for (unsigned i = 0; i < 32; i++) {
    rx_ring[i].address = (uint32_t)rx_buffers[i];
    rx_ring[i].status = 0;
  }
  for (unsigned i = 0; i < 8; i++) {
    tx_ring[i].address = (uint32_t)e_tx[i];
    tx_ring[i].status = 1;
  }
  wr(0x2800, (uint32_t)rx_ring);
  wr(0x2804, 0);
  wr(0x2808, sizeof(rx_ring));
  wr(0x2810, 0);
  wr(0x2818, 31);
  wr(0x3800, (uint32_t)tx_ring);
  wr(0x3804, 0);
  wr(0x3808, sizeof(tx_ring));
  wr(0x3810, 0);
  wr(0x3818, 0);
  wr(0x410, 0x0060200a);
  wr(0x400, (1 << 1) | (1 << 3) | (15 << 4) | (64 << 12));
  wr(0x100, (1 << 1) | (1 << 15) | (1 << 26));
  nic = E1000;
  return true;
}
void net_init(void) {
  const struct pci_device *d = pci_find(0x10ec, 0x8139);
  if (d && rtl_init(d))
    link_ready = true;
  else {
    const uint16_t models[] = {0x100e, 0x100f};
    for (unsigned i = 0; i < 2 && !link_ready; i++) {
      d = pci_find(0x8086, models[i]);
      if (d && e1000_init(d))
        link_ready = true;
    }
  }
}
struct arp_entry {
  uint32_t ip, seen;
  uint8_t mac[6];
};
static struct arp_entry arp[8];
static unsigned arp_next;
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
static void arp_store(uint32_t address, const uint8_t *hw) {
  if (!address || (address >> 24) >= 224)
    return;
  for (unsigned i = 0; i < 8; i++)
    if (arp[i].ip == address) {
      memcpy(arp[i].mac, hw, 6);
      arp[i].seen = clock_ticks();
      return;
    }
  struct arp_entry *a = &arp[arp_next++ % 8];
  a->ip = address;
  memcpy(a->mac, hw, 6);
  a->seen = clock_ticks();
}
static void arp_send(uint16_t op, const uint8_t *target, uint32_t address) {
  uint8_t p[42] = {0};
  memcpy(p, target, 6);
  memcpy(p + 6, mac, 6);
  put16(p + 12, 0x806);
  put16(p + 14, 1);
  put16(p + 16, 0x800);
  p[18] = 6;
  p[19] = 4;
  put16(p + 20, op);
  memcpy(p + 22, mac, 6);
  put32(p + 28, ip);
  if (op == 2)
    memcpy(p + 32, target, 6);
  put32(p + 38, address);
  ethernet_send(p, sizeof(p));
}
static bool resolve(uint32_t destination, uint8_t *result) {
  if (destination == 0xffffffffu || (ip && destination == (ip | ~mask))) {
    memcpy(result, broadcast, 6);
    return true;
  }
  uint32_t next = ((destination & mask) == (ip & mask)) ? destination : gateway;
  if (!next)
    return false;
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    for (unsigned i = 0; i < 8; i++)
      if (arp[i].ip == next && clock_ticks() - arp[i].seen < 6000) {
        memcpy(result, arp[i].mac, 6);
        return true;
      }
    arp_send(1, broadcast, next);
    uint32_t start = clock_ticks();
    while (clock_ticks() - start < 35) {
      net_poll();
      for (unsigned i = 0; i < 8; i++)
        if (arp[i].ip == next && clock_ticks() - arp[i].seen < 6000) {
          memcpy(result, arp[i].mac, 6);
          return true;
        }
      cpu_pause();
    }
  }
  return false;
}
static bool ipv4_send(uint32_t dest, uint8_t protocol, const void *payload,
                      uint32_t size) {
  if (size > 1480)
    return false;
  uint8_t p[1514], target[6];
  if (!resolve(dest, target))
    return false;
  memcpy(p, target, 6);
  memcpy(p + 6, mac, 6);
  put16(p + 12, 0x800);
  uint8_t *h = p + 14;
  memset(h, 0, 20);
  h[0] = 0x45;
  put16(h + 2, (uint16_t)(20 + size));
  put16(h + 4, ++ip_id);
  put16(h + 6, 0x4000);
  h[8] = 64;
  h[9] = protocol;
  put32(h + 12, ip);
  put32(h + 16, dest);
  put16(h + 10, checksum(h, 20));
  memcpy(h + 20, payload, size);
  return ethernet_send(p, 34 + size);
}
struct datagram {
  bool used;
  uint32_t source, size;
  uint16_t src_port, dst_port;
  uint8_t data[1472];
};
static struct datagram udp[8];
static unsigned udp_next;
static void tcp_receive(uint32_t, const uint8_t *, uint32_t);
static uint16_t ping_sequence;
static uint32_t ping_target, ping_reply_tick;
static bool ping_waiting, ping_received;
static void receive(const uint8_t *p, uint32_t n) {
  if (n < 14)
    return;
  rx_count++;
  if (!same_mac(p, mac) && !same_mac(p, broadcast))
    return;
  uint16_t type = be16(p + 12);
  if (type == 0x806) {
    if (n < 42 || be16(p + 14) != 1 || be16(p + 16) != 0x800 || p[18] != 6 ||
        p[19] != 4)
      return;
    uint16_t op = be16(p + 20);
    if (op != 1 && op != 2)
      return;
    if (be32(p + 38) != ip)
      return;
    arp_store(be32(p + 28), p + 22);
    if (op == 1 && ip)
      arp_send(2, p + 22, be32(p + 28));
    return;
  }
  if (type != 0x800 || n < 34)
    return;
  const uint8_t *h = p + 14;
  uint32_t ihl = (h[0] & 15) * 4, total = be16(h + 2);
  if ((h[0] >> 4) != 4 || ihl < 20 || ihl > 60 || total < ihl ||
      total > n - 14 || checksum(h, ihl) || ((be16(h + 6) & 0x3fff) != 0))
    return;
  uint32_t source = be32(h + 12), dest = be32(h + 16);
  if (dest != ip && dest != 0xffffffffu && (!ip || dest != (ip | ~mask)))
    return;
  const uint8_t *payload = h + ihl;
  uint32_t size = total - ihl;
  if (h[9] == 1) {
    if (size < 8 || checksum(payload, size))
      return;
    if (payload[0] == 8 && payload[1] == 0 && ip && dest == ip &&
        size <= 1480) { /* Reply directly to the Ethernet sender; never block in
                           receive. */
      uint8_t reply[1514];
      memcpy(reply, p + 6, 6);
      memcpy(reply + 6, mac, 6);
      put16(reply + 12, 0x800);
      uint8_t *rh = reply + 14;
      memcpy(rh, h, 20);
      rh[0] = 0x45;
      put16(rh + 2, 20 + size);
      put16(rh + 6, 0);
      rh[8] = 64;
      put32(rh + 12, ip);
      put32(rh + 16, source);
      put16(rh + 10, 0);
      put16(rh + 10, checksum(rh, 20));
      memcpy(rh + 20, payload, size);
      rh[20] = 0;
      put16(rh + 22, 0);
      put16(rh + 22, checksum(rh + 20, size));
      ethernet_send(reply, 34 + size);
    } else if (payload[0] == 0 && payload[1] == 0 && ping_waiting &&
               source == ping_target && be16(payload + 4) == 0xa11a &&
               be16(payload + 6) == ping_sequence) {
      ping_received = true;
      ping_reply_tick = clock_ticks();
    }
    return;
  }
  if (h[9] == 6) {
    if (dest == ip)
      tcp_receive(source, payload, size);
    return;
  }
  if (h[9] != 17 || size < 8)
    return;
  uint32_t len = be16(payload + 4);
  if (len < 8 || len > size || len - 8 > 1472)
    return;
  if (be16(payload + 6)) {
    uint8_t pseudo[1492];
    put32(pseudo, source);
    put32(pseudo + 4, dest);
    pseudo[8] = 0;
    pseudo[9] = 17;
    put16(pseudo + 10, len);
    memcpy(pseudo + 12, payload, len);
    if (checksum(pseudo, len + 12))
      return;
  }
  unsigned slot = 8;
  for (unsigned i = 0; i < 8; i++)
    if (!udp[i].used) {
      slot = i;
      break;
    }
  if (slot == 8) {
    slot = udp_next++ % 8;
    drop_count++;
  }
  struct datagram *d = &udp[slot];
  d->used = true;
  d->source = source;
  d->src_port = be16(payload);
  d->dst_port = be16(payload + 2);
  d->size = len - 8;
  memcpy(d->data, payload + 8, d->size);
}
void net_poll(void) {
  devices_poll();
  if (!link_ready)
    return;
  if (nic == RTL8139) {
    for (unsigned frames = 0; frames < 32 && !(io_in8(rtl_port + 0x37) & 1);
         frames++) {
      uint8_t *p = rtl_rx + rtl_offset;
      uint16_t status = (uint16_t)p[0] | ((uint16_t)p[1] << 8),
               n = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
      if (!(status & 1) || n < 4 || n > 1518) {
        drop_count++;
        io_out8(rtl_port + 0x37, 0);
        rtl_offset = 0;
        io_out16(rtl_port + 0x38, 0xfff0);
        io_out8(rtl_port + 0x37, 0x0c);
        break;
      }
      receive(p + 4, n - 4);
      rtl_offset = (rtl_offset + n + 4 + 3) & ~3u;
      rtl_offset %= 8192;
      io_out16(rtl_port + 0x38, (uint16_t)(rtl_offset - 16));
    }
    io_out16(rtl_port + 0x3e, io_in16(rtl_port + 0x3e));
  } else {
    for (unsigned frames = 0; frames < 32 && (rx_ring[rx_index].status & 1);
         frames++) {
      unsigned i = rx_index;
      __asm__ volatile("" ::: "memory");
      uint16_t n = rx_ring[i].length;
      if ((rx_ring[i].status & 2) && !rx_ring[i].errors && n <= 1514)
        receive(rx_buffers[i], n);
      else
        drop_count++;
      rx_ring[i].status = 0;
      __asm__ volatile("" ::: "memory");
      wr(0x2818, i);
      rx_index = (i + 1) % 32;
    }
    (void)rd(0xc0);
  }
}
int net_udp_send(uint32_t dest, uint16_t src, uint16_t port, const void *data,
                 uint32_t n) {
  if (!link_ready || n > 1472 || !src || !port || !dest)
    return -1;
  uint8_t p[1480];
  put16(p, src);
  put16(p + 2, port);
  put16(p + 4, n + 8);
  put16(p + 6, 0);
  memcpy(p + 8, data, n);
  return ipv4_send(dest, 17, p, n + 8) ? (int)n : -2;
}
int net_udp_recv(uint16_t port, void *data, uint32_t cap, uint32_t *source,
                 uint16_t *srcport) {
  net_poll();
  for (unsigned i = 0; i < 8; i++)
    if (udp[i].used && udp[i].dst_port == port) {
      struct datagram *d = &udp[i];
      if (d->size > cap)
        return -2;
      memcpy(data, d->data, d->size);
      if (source)
        *source = d->source;
      if (srcport)
        *srcport = d->src_port;
      d->used = false;
      return (int)d->size;
    }
  return -1;
}
bool ip_parse(const char *s, uint32_t *result) {
  uint32_t n = 0;
  for (unsigned k = 0; k < 4; k++) {
    if (*s < '0' || *s > '9')
      return false;
    unsigned part = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      part = part * 10 + (unsigned)(*s++ - '0');
      if (part > 255 || ++digits > 3)
        return false;
    }
    n = (n << 8) | part;
    if (k < 3) {
      if (*s++ != '.')
        return false;
    } else if (*s)
      return false;
  }
  *result = n;
  return true;
}
void ip_print(uint32_t a) {
  for (int i = 3; i >= 0; i--) {
    print_uint((a >> (i * 8)) & 255);
    if (i)
      console_putc('.');
  }
}
void net_config(uint32_t address, uint32_t netmask, uint32_t router,
                uint32_t dns) {
  ip = address;
  mask = netmask;
  gateway = router;
  dns_server = dns;
  memset(arp, 0, sizeof(arp));
}
void net_info(void) {
  console_write("NIC: ");
  console_write(nic == RTL8139 ? "RTL8139"
                : nic == E1000 ? "Intel E1000"
                               : "none supported");
  console_write(" MAC ");
  for (unsigned i = 0; i < 6; i++) {
    console_putc("0123456789abcdef"[mac[i] >> 4]);
    console_putc("0123456789abcdef"[mac[i] & 15]);
    if (i < 5)
      console_putc(':');
  }
  console_write("\nIPv4 ");
  ip_print(ip);
  console_write(" mask ");
  ip_print(mask);
  console_write(" gateway ");
  ip_print(gateway);
  console_write(" DNS ");
  ip_print(dns_server);
  console_write("\nRX ");
  print_uint(rx_count);
  console_write(" TX ");
  print_uint(tx_count);
  console_write(" dropped ");
  print_uint(drop_count);
  console_putc('\n');
}
int net_ping(uint32_t dest) {
  if (!ip || !link_ready)
    return -1;
  uint8_t p[40] = {8, 0};
  put16(p + 4, 0xa11a);
  put16(p + 6, ++ping_sequence);
  for (unsigned i = 8; i < sizeof(p); i++)
    p[i] = (uint8_t)i;
  put16(p + 2, checksum(p, sizeof(p)));
  ping_target = dest;
  ping_waiting = true;
  ping_received = false;
  uint32_t start = clock_ticks();
  if (!ipv4_send(dest, 1, p, sizeof(p))) {
    ping_waiting = false;
    return -2;
  }
  while (clock_ticks() - start < 300) {
    net_poll();
    if (ping_received) {
      ping_waiting = false;
      return (int)((ping_reply_tick - start) * 10);
    }
    cpu_pause();
  }
  ping_waiting = false;
  return -3;
}
static uint32_t dhcp_xid;
static void dhcp_packet(uint8_t *p, uint8_t type, uint32_t offered,
                        uint32_t server, uint32_t *size) {
  memset(p, 0, 320);
  p[0] = 1;
  p[1] = 1;
  p[2] = 6;
  put32(p + 4, dhcp_xid);
  put16(p + 10, 0x8000);
  memcpy(p + 28, mac, 6);
  put32(p + 236, 0x63825363);
  unsigned i = 240;
  p[i++] = 53;
  p[i++] = 1;
  p[i++] = type;
  p[i++] = 61;
  p[i++] = 7;
  p[i++] = 1;
  memcpy(p + i, mac, 6);
  i += 6;
  if (type == 3) {
    p[i++] = 50;
    p[i++] = 4;
    put32(p + i, offered);
    i += 4;
    p[i++] = 54;
    p[i++] = 4;
    put32(p + i, server);
    i += 4;
  }
  p[i++] = 55;
  p[i++] = 4;
  p[i++] = 1;
  p[i++] = 3;
  p[i++] = 6;
  p[i++] = 51;
  p[i++] = 255;
  *size = i;
}
struct lease {
  uint32_t address, mask, router, dns, server;
  uint8_t type;
};
static bool dhcp_parse(const uint8_t *p, unsigned n, struct lease *l) {
  if (n < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6 ||
      be32(p + 4) != dhcp_xid || !same_mac(p + 28, mac) ||
      be32(p + 236) != 0x63825363)
    return false;
  memset(l, 0, sizeof(*l));
  l->address = be32(p + 16);
  for (unsigned i = 240; i < n;) {
    unsigned code = p[i++];
    if (code == 255)
      break;
    if (!code)
      continue;
    if (i >= n)
      return false;
    unsigned len = p[i++];
    if (len > n - i)
      return false;
    if (code == 53 && len == 1)
      l->type = p[i];
    else if (code == 1 && len == 4)
      l->mask = be32(p + i);
    else if (code == 3 && len >= 4)
      l->router = be32(p + i);
    else if (code == 6 && len >= 4)
      l->dns = be32(p + i);
    else if (code == 54 && len == 4)
      l->server = be32(p + i);
    i += len;
  }
  return true;
}
int net_dhcp(void) {
  if (!link_ready)
    return -1;
  uint32_t old_ip = ip, old_mask = mask, old_gw = gateway, old_dns = dns_server;
  ip = 0;
  mask = 0;
  gateway = 0;
  dhcp_xid = 0x41564c00u ^ clock_ticks() ^ ((uint32_t)mac[4] << 8) ^ mac[5];
  uint8_t p[320], reply[1472];
  struct lease offer = {0};
  bool offered = false;
  uint32_t size;
  for (unsigned attempt = 0; attempt < 3 && !offered; attempt++) {
    dhcp_packet(p, 1, 0, 0, &size);
    net_udp_send(0xffffffffu, 68, 67, p, size);
    uint32_t start = clock_ticks();
    while (clock_ticks() - start < 200) {
      uint32_t source;
      uint16_t port;
      int n = net_udp_recv(68, reply, sizeof(reply), &source, &port);
      if (n >= 0 && port == 67 && dhcp_parse(reply, (unsigned)n, &offer) &&
          offer.type == 2 && offer.address && offer.server) {
        offered = true;
        break;
      }
      cpu_pause();
    }
  }
  if (offered)
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      dhcp_packet(p, 3, offer.address, offer.server, &size);
      net_udp_send(0xffffffffu, 68, 67, p, size);
      uint32_t start = clock_ticks();
      while (clock_ticks() - start < 200) {
        uint32_t source;
        uint16_t port;
        int n = net_udp_recv(68, reply, sizeof(reply), &source, &port);
        struct lease ack;
        if (n >= 0 && port == 67 && dhcp_parse(reply, (unsigned)n, &ack)) {
          if (ack.type == 6)
            break;
          if (ack.type == 5 && ack.address == offer.address &&
              ack.server == offer.server && ack.mask) {
            net_config(ack.address, ack.mask, ack.router, ack.dns);
            return 0;
          }
        }
        cpu_pause();
      }
    }
  net_config(old_ip, old_mask, old_gw, old_dns);
  return -2;
}
static bool skip_name(const uint8_t *p, unsigned n, unsigned *off) {
  unsigned steps = 0;
  while (*off < n && steps++ < 128) {
    uint8_t len = p[(*off)++];
    if (!len)
      return true;
    if ((len & 0xc0) == 0xc0) {
      if (*off >= n)
        return false;
      unsigned target = ((unsigned)(len & 63) << 8) | p[(*off)++];
      return target < n;
    }
    if (len > 63 || len > n - *off)
      return false;
    *off += len;
  }
  return false;
}
int net_dns(const char *name, uint32_t *answer) {
  if (ip_parse(name, answer))
    return 0;
  if (!ip || !dns_server)
    return -1;
  uint8_t query[512] = {0}, reply[1472];
  uint16_t id = (uint16_t)(clock_ticks() ^ 0xa51a), port = 49152 + (id % 16000);
  put16(query, id);
  put16(query + 2, 0x100);
  put16(query + 4, 1);
  unsigned pos = 12;
  const char *s = name;
  if (!*s)
    return -2;
  while (*s) {
    const char *begin = s;
    while (*s && *s != '.')
      s++;
    unsigned len = (unsigned)(s - begin);
    if (!len || len > 63 || pos + len + 6 > sizeof(query))
      return -2;
    query[pos++] = len;
    memcpy(query + pos, begin, len);
    pos += len;
    if (*s)
      s++;
  }
  query[pos++] = 0;
  put16(query + pos, 1);
  put16(query + pos + 2, 1);
  pos += 4;
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    if (net_udp_send(dns_server, port, 53, query, pos) < 0)
      return -3;
    uint32_t start = clock_ticks();
    while (clock_ticks() - start < 150) {
      uint32_t source;
      uint16_t srcport;
      int result = net_udp_recv(port, reply, sizeof(reply), &source, &srcport);
      if (result < 12 || source != dns_server || srcport != 53)
        continue;
      unsigned n = (unsigned)result;
      if (be16(reply) != id || (be16(reply + 2) & 0x820f) != 0x8000)
        continue;
      unsigned off = 12, questions = be16(reply + 4), answers = be16(reply + 6);
      if (questions != 1 || answers > 128)
        continue;
      if (!skip_name(reply, n, &off) || n - off < 4)
        continue;
      off += 4;
      for (unsigned i = 0; i < answers; i++) {
        if (!skip_name(reply, n, &off) || n - off < 10)
          break;
        uint16_t type = be16(reply + off), cls = be16(reply + off + 2),
                 len = be16(reply + off + 8);
        off += 10;
        if (len > n - off)
          break;
        if (type == 1 && cls == 1 && len == 4) {
          *answer = be32(reply + off);
          return 0;
        }
        off += len;
      }
    }
  }
  return -4;
}

/* A single active-open TCP stream. Stop-and-wait transmission bounds memory;
 * in-order receive, advertised windows, checksums and retransmission are used.
 * No listen/backlog API, SACK, window scaling or IP fragmentation is provided.
 */
#define TCP_RX 8192u
static struct {
  bool active, established, peer_closed, reset;
  uint32_t remote, snd_una, snd_nxt, rcv_nxt;
  uint16_t local_port, remote_port, peer_window, mss;
  uint8_t hardware[6], buffer[TCP_RX];
  uint32_t head, count;
} tcp;
static uint16_t tcp_port = 50000;
static void tcp_emit(uint8_t flags, uint32_t seq, const void *data,
                     uint32_t size) {
  uint8_t p[594], pseudo[572];
  unsigned options = (flags & 2) ? 4 : 0, hlen = 20 + options,
           tlen = hlen + size;
  if (tlen > 560)
    return;
  memcpy(p, tcp.hardware, 6);
  memcpy(p + 6, mac, 6);
  put16(p + 12, 0x800);
  uint8_t *h = p + 14;
  memset(h, 0, 20);
  h[0] = 0x45;
  put16(h + 2, 20 + tlen);
  put16(h + 4, ++ip_id);
  put16(h + 6, 0x4000);
  h[8] = 64;
  h[9] = 6;
  put32(h + 12, ip);
  put32(h + 16, tcp.remote);
  put16(h + 10, checksum(h, 20));
  uint8_t *t = h + 20;
  memset(t, 0, hlen);
  put16(t, tcp.local_port);
  put16(t + 2, tcp.remote_port);
  put32(t + 4, seq);
  put32(t + 8, tcp.rcv_nxt);
  t[12] = (uint8_t)((hlen / 4) << 4);
  t[13] = flags;
  put16(t + 14, TCP_RX - tcp.count);
  if (options) {
    t[20] = 2;
    t[21] = 4;
    put16(t + 22, 536);
  }
  if (size)
    memcpy(t + hlen, data, size);
  put32(pseudo, ip);
  put32(pseudo + 4, tcp.remote);
  pseudo[8] = 0;
  pseudo[9] = 6;
  put16(pseudo + 10, tlen);
  memcpy(pseudo + 12, t, tlen);
  put16(t + 16, checksum(pseudo, tlen + 12));
  ethernet_send(p, 34 + tlen);
}
static void tcp_receive(uint32_t source, const uint8_t *p, uint32_t n) {
  if (!tcp.active || source != tcp.remote || n < 20 || n > 1480 ||
      be16(p) != tcp.remote_port || be16(p + 2) != tcp.local_port)
    return;
  unsigned hlen = (p[12] >> 4) * 4;
  if (hlen < 20 || hlen > n)
    return;
  uint8_t pseudo[1492];
  put32(pseudo, source);
  put32(pseudo + 4, ip);
  pseudo[8] = 0;
  pseudo[9] = 6;
  put16(pseudo + 10, n);
  memcpy(pseudo + 12, p, n);
  if (checksum(pseudo, n + 12))
    return;
  uint32_t seq = be32(p + 4), ack = be32(p + 8);
  uint8_t flags = p[13];
  if (flags & 4) {
    if ((tcp.established && seq == tcp.rcv_nxt) ||
        (!tcp.established && (flags & 16) && ack == tcp.snd_nxt))
      tcp.reset = true;
    return;
  }
  if (!tcp.established) {
    if ((flags & 0x12) != 0x12 || ack != tcp.snd_nxt)
      return;
    tcp.snd_una = ack;
    tcp.rcv_nxt = seq + 1;
    tcp.peer_window = be16(p + 14);
    tcp.mss = 536;
    for (unsigned off = 20; off < hlen;) {
      unsigned kind = p[off++];
      if (!kind)
        break;
      if (kind == 1)
        continue;
      if (off >= hlen)
        break;
      unsigned len = p[off++];
      if (len < 2 || len - 2 > hlen - off)
        break;
      if (kind == 2 && len == 4) {
        uint16_t m = be16(p + off);
        if (m && m < tcp.mss)
          tcp.mss = m;
      }
      off += len - 2;
    }
    tcp.established = true;
    tcp_emit(16, tcp.snd_nxt, NULL, 0);
    return;
  }
  if (!(flags & 16))
    return;
  if ((int32_t)(ack - tcp.snd_una) >= 0 && (int32_t)(tcp.snd_nxt - ack) >= 0) {
    tcp.snd_una = ack;
    tcp.peer_window = be16(p + 14);
  }
  if (flags & 2) {
    tcp_emit(16, tcp.snd_nxt, NULL, 0);
    return;
  }
  unsigned bytes = n - hlen;
  if (bytes || flags & 1) {
    if (seq != tcp.rcv_nxt || bytes > TCP_RX - tcp.count) {
      tcp_emit(16, tcp.snd_nxt, NULL, 0);
      return;
    }
    for (unsigned i = 0; i < bytes; i++)
      tcp.buffer[(tcp.head + tcp.count + i) % TCP_RX] = p[hlen + i];
    tcp.count += bytes;
    tcp.rcv_nxt += bytes;
    if (flags & 1) {
      tcp.rcv_nxt++;
      tcp.peer_closed = true;
    }
    tcp_emit(16, tcp.snd_nxt, NULL, 0);
  }
}
int net_tcp_connect(uint32_t destination, uint16_t port) {
  if (!link_ready || !ip || !destination || !port || tcp.active)
    return -1;
  memset(&tcp, 0, sizeof(tcp));
  if (!resolve(destination, tcp.hardware))
    return -2;
  tcp.active = true;
  tcp.remote = destination;
  tcp.remote_port = port;
  tcp.local_port = ++tcp_port;
  if (tcp_port < 49152)
    tcp.local_port = tcp_port = 49152;
  uint32_t seq = clock_ticks() * 1103515245u ^ ((uint32_t)mac[4] << 16) ^
                 ((uint32_t)mac[5] << 8);
  tcp.snd_una = seq;
  tcp.snd_nxt = seq + 1;
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    tcp_emit(2, seq, NULL, 0);
    uint32_t start = clock_ticks();
    while (clock_ticks() - start < (100u << attempt)) {
      net_poll();
      if (tcp.reset) {
        tcp.active = false;
        return -3;
      }
      if (tcp.established)
        return 0;
      cpu_pause();
    }
  }
  tcp.active = false;
  return -4;
}
int net_tcp_send(const void *data, uint32_t size) {
  if (!tcp.active || !tcp.established || tcp.reset || size > 65536)
    return -1;
  const uint8_t *p = data;
  uint32_t sent = 0;
  while (sent < size) {
    uint32_t window_start = clock_ticks();
    while (!tcp.peer_window && !tcp.reset &&
           clock_ticks() - window_start < 300) {
      net_poll();
      cpu_pause();
    }
    if (!tcp.peer_window || tcp.reset)
      return -4;
    uint32_t n = size - sent;
    if (n > tcp.mss)
      n = tcp.mss;
    if (n > tcp.peer_window)
      n = tcp.peer_window;
    uint32_t seq = tcp.snd_nxt;
    tcp.snd_nxt += n;
    bool acknowledged = false;
    for (unsigned attempt = 0; attempt < 4 && !acknowledged; attempt++) {
      tcp_emit(0x18, seq, p + sent, n);
      uint32_t start = clock_ticks();
      while (clock_ticks() - start < (100u << attempt)) {
        net_poll();
        if (tcp.reset) {
          tcp.active = false;
          return -2;
        }
        if (tcp.snd_una == tcp.snd_nxt) {
          acknowledged = true;
          break;
        }
        cpu_pause();
      }
    }
    if (!acknowledged) {
      tcp_emit(4, tcp.snd_nxt, NULL, 0);
      tcp.active = false;
      return -3;
    }
    sent += n;
  }
  return (int)sent;
}
int net_tcp_recv(void *data, uint32_t capacity, uint32_t timeout) {
  if (!tcp.active || !tcp.established || !capacity || timeout > 1000)
    return -1;
  uint32_t start = clock_ticks();
  while (!tcp.count && !tcp.peer_closed && !tcp.reset) {
    net_poll();
    if (clock_ticks() - start >= timeout)
      return -2;
    cpu_pause();
  }
  if (tcp.reset)
    return -3;
  uint32_t n = tcp.count;
  if (n > capacity)
    n = capacity;
  for (uint32_t i = 0; i < n; i++)
    ((uint8_t *)data)[i] = tcp.buffer[(tcp.head + i) % TCP_RX];
  tcp.head = (tcp.head + n) % TCP_RX;
  tcp.count -= n;
  if (n)
    tcp_emit(16, tcp.snd_nxt, NULL, 0);
  return (int)n;
}
int net_tcp_close(void) {
  if (!tcp.active)
    return 0;
  if (tcp.reset) {
    tcp.active = false;
    return -1;
  }
  uint32_t seq = tcp.snd_nxt++;
  bool acknowledged = false;
  for (unsigned attempt = 0; attempt < 3 && !acknowledged; attempt++) {
    tcp_emit(0x11, seq, NULL, 0);
    uint32_t start = clock_ticks();
    while (clock_ticks() - start < 100) {
      net_poll();
      if (tcp.reset)
        break;
      if (tcp.snd_una == tcp.snd_nxt) {
        acknowledged = true;
        break;
      }
      cpu_pause();
    }
  }
  uint32_t start = clock_ticks();
  while (acknowledged && !tcp.peer_closed && !tcp.reset &&
         clock_ticks() - start < 200) {
    net_poll();
    cpu_pause();
  }
  if (!acknowledged || !tcp.peer_closed)
    tcp_emit(4, tcp.snd_nxt, NULL, 0);
  tcp.active = false;
  return acknowledged ? 0 : -1;
}
void net_http(const char *host, const char *path) {
  if (!*path)
    path = "/";
  if (*path != '/') {
    console_write("HTTP path must begin with /.\n");
    return;
  }
  uint32_t address;
  if (net_dns(host, &address) < 0) {
    console_write("DNS resolution failed.\n");
    return;
  }
  if (net_tcp_connect(address, 80) < 0) {
    console_write("TCP connection failed.\n");
    return;
  }
  char request[512];
  size_t h = strlen(host), p = strlen(path);
  if (h + p + 64 >= sizeof(request)) {
    console_write("HTTP request too long.\n");
    net_tcp_close();
    return;
  }
  size_t n = 0;
  const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host,
                         "\r\nConnection: close\r\n\r\n"};
  for (unsigned k = 0; k < 5; k++) {
    size_t size = strlen(parts[k]);
    memcpy(request + n, parts[k], size);
    n += size;
  }
  if (net_tcp_send(request, n) < 0)
    console_write("HTTP send failed.\n");
  else {
    uint8_t data[512];
    uint32_t total = 0;
    while (total < 65536) {
      int r = net_tcp_recv(data, sizeof(data), 500);
      if (r <= 0) {
        if (r < 0)
          console_write("\nHTTP receive stopped.\n");
        break;
      }
      for (int i = 0; i < r; i++)
        console_putc((char)data[i]);
      total += (uint32_t)r;
    }
  }
  net_tcp_close();
  console_putc('\n');
}
