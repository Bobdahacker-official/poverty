#include "system.h"
#include "firewall.h"
#include "net_service.h"
#include "tcp.h"
#include "ip_fragment.h"
#include "virtio.h"
#include "usb.h"
/* Poll-driven Ethernet. DMA memory is identity mapped and supervisor-only. */
static enum { NONE, RTL8139, E1000, VIRTIO, PCNET, RTL8169, RGE, USB_ECM } nic, wired_nic;
static uint8_t wired_mac[6];
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
static uint32_t dns_secondary;
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
  return ((uintptr_t)p[0] << 24) | ((uintptr_t)p[1] << 16) |
         ((uintptr_t)p[2] << 8) | p[3];
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
static bool ethernet_send_raw(const void *frame, uint32_t n) {
  if (!link_ready || n > 1514 || n < 14)
    return false;
  if (nic == USB_ECM) {
    bool ok = usb_ethernet_send(frame, n);
    if (ok) tx_count++;
    else drop_count++;
    return ok;
  }
  if (nic == RGE) {
    bool ok=rge_send(frame,n);if(ok)tx_count++;else drop_count++;return ok;
  }
  if (nic == RTL8169) {
    bool ok = rtl8169_send(frame, n);
    if (ok) tx_count++;
    return ok;
  }
  if (nic == PCNET) {
    bool ok = pcnet_send(frame, n);
    if (ok)
      tx_count++;
    return ok;
  }
  if (nic == VIRTIO) {
    bool ok = virtio_net_send(frame, n);
    if (ok)
      tx_count++;
    return ok;
  }
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
    io_out32(rtl_port + 0x20 + index * 4, page_physical(tx_buffers[index]));
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
static bool ethernet_send(const void *frame,uint32_t size) {
  if(size>=34 && be16((const uint8_t *)frame+12)==0x800 &&
      !firewall_packet(true,(const uint8_t *)frame+14,size-14))return false;
  return ethernet_send_raw(frame,size);
}
bool net_frame_send(const void *frame,uint32_t size) {return ethernet_send(frame,size);}
void net_device_mac(uint8_t result[6]) {memcpy(result,mac,6);}
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
  io_out32(rtl_port + 0x30, page_physical(rtl_rx));
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
  uint64_t base = pci_memory_bar(d, 0);
  if (!base || !platform_mmio(base, 0x20000))
    return false;
  e1000 = page_pointer(base);
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
    low = (uintptr_t)mac[0] | ((uintptr_t)mac[1] << 8) |
          ((uintptr_t)mac[2] << 16) | ((uintptr_t)mac[3] << 24);
    high = mac[4] | ((uintptr_t)mac[5] << 8) | 0x80000000u;
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
    rx_ring[i].address = page_physical(rx_buffers[i]);
    rx_ring[i].status = 0;
  }
  for (unsigned i = 0; i < 8; i++) {
    tx_ring[i].address = page_physical(e_tx[i]);
    tx_ring[i].status = 1;
  }
  wr(0x2800, page_physical(rx_ring));
  wr(0x2804, 0);
  wr(0x2808, sizeof(rx_ring));
  wr(0x2810, 0);
  wr(0x2818, 31);
  wr(0x3800, page_physical(tx_ring));
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
  if (virtio_net_init(mac)) {
    nic = VIRTIO;
    link_ready = true;
    return;
  }
  if (pcnet_init(mac)) {
    nic = PCNET;
    link_ready = true;
    return;
  }
  if (rge_init(mac)) { nic = RGE; link_ready = true; return; }
  if (rtl8169_init(mac)) { nic = RTL8169; link_ready = true; return; }
  const struct pci_device *d = pci_find(0x10ec, 0x8139);
  if (d && rtl_init(d))
    link_ready = true;
  else {
    const uint16_t models[] = {0x100e, 0x100f, 0x1010, 0x1011, 0x1012,
                               0x1015, 0x1016, 0x1017, 0x1026};
    for (unsigned i = 0; i < sizeof(models) / sizeof(models[0]) && !link_ready;
         i++) {
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
  if(!ip)return false;
  uint32_t next;
  if (!route_lookup(destination, &next) || !next)
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
bool net_resolve(uint32_t destination,uint8_t *result) {return resolve(destination,result);}
static bool ipv4_send_to(uint32_t dest,uint8_t protocol,const void *payload,uint32_t size,const uint8_t target[6]) {
  if(size>65515)return false;
  uint16_t identification=++ip_id;
  if(size>1480) {
    static uint8_t complete[65535];
    memset(complete,0,20);complete[0]=0x45;put16(complete+2,20+size);complete[8]=64;complete[9]=protocol;
    put32(complete+12,ip);put32(complete+16,dest);put16(complete+10,checksum(complete,20));
    memcpy(complete+20,payload,size);
    if(!firewall_packet(true,complete,20+size))return false;
  }
  for(unsigned offset=0;offset<size || (!size && !offset);) {
    unsigned count=size-offset;if(count>1480)count=1480;
    uint8_t frame[1514];memcpy(frame,target,6);memcpy(frame+6,mac,6);put16(frame+12,0x800);
    uint8_t *h=frame+14;memset(h,0,20);h[0]=0x45;put16(h+2,20+count);put16(h+4,identification);
    put16(h+6,size<=1480?0x4000:(offset/8)|(offset+count<size?0x2000:0));h[8]=64;h[9]=protocol;
    put32(h+12,ip);put32(h+16,dest);put16(h+10,checksum(h,20));memcpy(h+20,(const uint8_t *)payload+offset,count);
    if(!(size>1480?ethernet_send_raw(frame,34+count):ethernet_send(frame,34+count)))return false;
    if(!size)break;
    offset+=count;
  }
  return true;
}
static bool ipv4_send(uint32_t dest,uint8_t protocol,const void *payload,uint32_t size) {
  uint8_t target[6];if(!resolve(dest,target))return false;return ipv4_send_to(dest,protocol,payload,size,target);
}
struct datagram {
  bool used;
  uint32_t source, size;
  uint16_t src_port, dst_port;
  uint8_t data[65507];
};
static struct datagram udp[8];
static unsigned udp_next;

static uint16_t ping_sequence;
static uint32_t ping_target, ping_reply_tick;
static bool ping_waiting, ping_received;
static bool dhcp_receive(uint32_t, uint16_t, uint16_t, const uint8_t *, unsigned);
static bool dhcp_listening(void);
static void receive_packet(const uint8_t *,uint32_t,bool);
static void receive_reassembled(const uint8_t *p,uint32_t n) {receive_packet(p,n,false);}
static void receive(const uint8_t *p,uint32_t n) {receive_packet(p,n,true);}
static void receive_packet(const uint8_t *p, uint32_t n,bool physical) {
  if (n < 14)
    return;
  if (physical) rx_count++;
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
      total > n - 14 || checksum(h, ihl) || (be16(h + 6) & 0x8000))
    return;
  if (be16(h + 6) & 0x3fff) {ip4_reassemble(p,n,receive_reassembled);return;}
  uint32_t source = be32(h + 12), dest = be32(h + 16);
  bool dhcp_reply = dhcp_listening() && h[9] == 17 && total >= ihl+8 && be16(h+ihl) == 67 && be16(h+ihl+2) == 68;
  if (!dhcp_reply && dest != ip && dest != 0xffffffffu && (!ip || dest != (ip | ~mask)))
    return;
  if (!firewall_packet(false, h, total)) { drop_count++; return; }
  const uint8_t *payload = h + ihl;
  uint32_t size = total - ihl;
  if (h[9] == 1) {
    if (size < 8 || checksum(payload, size))
      return;
    if (payload[0] == 8 && payload[1] == 0 && ip && dest == ip && size<=65515) {
      static uint8_t reply[65515];memcpy(reply,payload,size);reply[0]=0;put16(reply+2,0);put16(reply+2,checksum(reply,size));
      ipv4_send_to(source,1,reply,size,p+6);
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
      tcp_input(source, payload, size, p + 6);
    return;
  }
  if (h[9] != 17 || size < 8)
    return;
  uint32_t len = be16(payload + 4);
  if (len < 8 || len > size || len - 8 > 65507)
    return;
  if (be16(payload + 6)) {
    uint32_t sum=(source>>16)+(source&65535)+(dest>>16)+(dest&65535)+17+len;
    for(unsigned i=0;i+1<len;i+=2)sum+=be16(payload+i);
    if(len&1)sum+=(uint32_t)payload[len-1]<<8;
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    if((uint16_t)~sum)return;
  }
  if (dhcp_receive(source, be16(payload), be16(payload + 2), payload + 8, len - 8)) return;
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
  uint8_t usb_mac[6];
  bool usb_present = usb_ethernet_mac(usb_mac);
  if ((nic == USB_ECM && !usb_present) || (nic == NONE && usb_present))
    net_use_usb_ethernet(usb_present);
  if (!link_ready)
    return;
  if (nic == USB_ECM) {
    usb_ethernet_poll(receive);
    net_service_poll();
    tcp_poll();
    return;
  }
  if (nic == RGE) { rge_poll(receive); net_service_poll(); tcp_poll(); return; }
  if (nic == RTL8169) { rtl8169_poll(receive); net_service_poll(); tcp_poll(); return; }
  if (nic == PCNET) {
    pcnet_poll(receive);
    net_service_poll(); tcp_poll();
    return;
  }
  if (nic == VIRTIO) {
    virtio_net_poll(receive);
    net_service_poll(); tcp_poll();
    return;
  }
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
  net_service_poll(); tcp_poll();
}
int net_udp_send(uint32_t dest, uint16_t src, uint16_t port, const void *data,
                 uint32_t n) {
  if (!link_ready || n > 65507 || !src || !port || !dest)
    return -1;
  uint8_t small[1480];
  unsigned pages=(n+8+4095)/4096;
  uint8_t *p=n+8<=sizeof(small)?small:pages_alloc(pages);
  if(!p)return -12;
  put16(p, src);
  put16(p + 2, port);
  put16(p + 4, n + 8);
  put16(p + 6, 0);
  memcpy(p + 8, data, n);
  int result=ipv4_send(dest,17,p,n+8)?(int)n:-2;
  if(p!=small)for(unsigned i=0;i<pages;i++)page_free(p+i*4096);
  return result;
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
static void apply_config(uint32_t address, uint32_t netmask, uint32_t router,
                uint32_t dns) {
  if (ip != address || mask != netmask || gateway != router) {tcp_reset_all(); memset(udp,0,sizeof(udp));}
  ip = address;
  mask = netmask;
  gateway = router;
  dns_server = dns;
  dns_secondary = 0;
  route_config(address, netmask, router);
  memset(arp, 0, sizeof(arp));
}
void net_config(uint32_t address, uint32_t netmask, uint32_t router, uint32_t dns) {
  net_dhcp_stop(); apply_config(address, netmask, router, dns);
}
bool net_usb_ethernet_selected(void) { return nic == USB_ECM; }
int net_use_usb_ethernet(bool enable) {
  if (enable == (nic == USB_ECM)) return 0;
  uint8_t address[6];
  if (enable && !usb_ethernet_mac(address)) return -19;
  net_config(0, 0, 0, 0);
  if (enable) {
    wired_nic = nic;
    memcpy(wired_mac, mac, sizeof(mac));
    nic = USB_ECM;
    memcpy(mac, address, sizeof(mac));
  } else {
    nic = wired_nic;
    memcpy(mac, wired_mac, sizeof(mac));
  }
  link_ready = nic != NONE;
  if (link_ready) net_dhcp_begin();
  return 0;
}
void net_dns_servers(uint32_t primary, uint32_t secondary) {
  dns_server = primary; dns_secondary = secondary;
}
void net_info(void) {
  console_write("NIC: ");
  console_write(nic == VIRTIO    ? "VirtIO legacy net"
                : nic == RTL8139 ? "RTL8139"
                : nic == E1000   ? "Intel E1000"
                : nic == PCNET   ? "AMD PCnet"
                : nic == RGE ? "Realtek RTL8125/8126/8127"
                : nic == RTL8169 ? "Realtek RTL8169/8168"
                : nic == USB_ECM ? "USB CDC Ethernet"
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
void net_snapshot(struct network_info *out) {
  *out = (struct network_info){ip,         mask,     gateway,
                               dns_server, rx_count, tx_count,
                               drop_count, nic != 0, {0}};
  const char *name = nic == VIRTIO    ? "VirtIO"
                     : nic == RTL8139 ? "RTL8139"
                     : nic == E1000   ? "Intel E1000"
                     : nic == PCNET   ? "AMD PCnet"
                : nic == RGE ? "Realtek RTL8125/8126/8127"
                : nic == RTL8169 ? "Realtek RTL8169/8168"
                : nic == USB_ECM ? "USB CDC Ethernet"
                                      : "No Ethernet adapter";
  memcpy(out->adapter, name, strlen(name) + 1);
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
enum dhcp_state {DHCP_OFF,DHCP_SELECTING,DHCP_REQUESTING,DHCP_BOUND,DHCP_RENEWING,DHCP_REBINDING,DHCP_RETRY};
static enum dhcp_state dhcp_state;
static bool dhcp_listening(void) {return dhcp_state==DHCP_SELECTING || dhcp_state==DHCP_REQUESTING;}
struct lease {
  uint32_t address,mask,router,dns,secondary,server,seconds,renew,rebind;
  uint8_t type;
  bool has_mask,has_router,has_dns;
};
static struct lease dhcp_offer;
static uint32_t dhcp_next,dhcp_expires,dhcp_renew_at,dhcp_rebind_at,dhcp_attempts,dhcp_acquired;
static bool dhcp_infinite,dhcp_polling;
static bool due(uint32_t now,uint32_t deadline) {return (int32_t)(now-deadline)>=0;}
static uint32_t transaction_id(void) {
  uint32_t value;
  if(!secure_random(&value,sizeof(value)))value=clock_ticks()^be32(mac)^((uint32_t)mac[4]<<24)^((uint32_t)mac[5]<<8);
  return value?value:1;
}
static void dhcp_packet(uint8_t *p,uint8_t type,uint32_t *size) {
  memset(p,0,320);p[0]=1;p[1]=1;p[2]=6;put32(p+4,dhcp_xid);
  bool renewal=dhcp_state==DHCP_RENEWING || dhcp_state==DHCP_REBINDING;
  if(renewal)put32(p+12,ip);else put16(p+10,0x8000);
  uint32_t elapsed=(clock_ticks()-dhcp_acquired)/100;put16(p+8,elapsed>65535?65535:elapsed);
  memcpy(p+28,mac,6);put32(p+236,0x63825363);
  unsigned i=240;p[i++]=53;p[i++]=1;p[i++]=type;
  p[i++]=61;p[i++]=7;p[i++]=1;memcpy(p+i,mac,6);i+=6;
  if(type==3 && !renewal) {
    p[i++]=50;p[i++]=4;put32(p+i,dhcp_offer.address);i+=4;
    p[i++]=54;p[i++]=4;put32(p+i,dhcp_offer.server);i+=4;
  }
  p[i++]=55;p[i++]=6;p[i++]=1;p[i++]=3;p[i++]=6;p[i++]=51;p[i++]=58;p[i++]=59;
  p[i++]=57;p[i++]=2;put16(p+i,1472);i+=2;
  p[i++]=255;*size=i;
}
static bool dhcp_parse(const uint8_t *p,unsigned n,struct lease *l) {
  if(n<240 || p[0]!=2 || p[1]!=1 || p[2]!=6 || be32(p+4)!=dhcp_xid ||
     !same_mac(p+28,mac) || be32(p+236)!=0x63825363)return false;
  memset(l,0,sizeof(*l));l->address=be32(p+16);
  for(unsigned i=240;i<n;) {
    unsigned code=p[i++];if(code==255)break;if(!code)continue;
    if(i>=n)return false;
    unsigned len=p[i++];if(len>n-i)return false;
    if(code==53 && len==1)l->type=p[i];
    else if(code==1 && len==4) {l->mask=be32(p+i);l->has_mask=true;}
    else if(code==3 && len>=4 && !(len&3)) {l->router=be32(p+i);l->has_router=true;}
    else if(code==6 && len>=4 && !(len&3)) {l->dns=be32(p+i);if(len>=8)l->secondary=be32(p+i+4);l->has_dns=true;}
    else if(code==54 && len==4)l->server=be32(p+i);
    else if(code==51 && len==4)l->seconds=be32(p+i);
    else if(code==58 && len==4)l->renew=be32(p+i);
    else if(code==59 && len==4)l->rebind=be32(p+i);
    i+=len;
  }
  return l->type!=0;
}
void net_dhcp_stop(void) {dhcp_state=DHCP_OFF;}
void net_dhcp_begin(void) {
  if(!link_ready) {dhcp_state=DHCP_OFF;return;}
  apply_config(0,0,0,0);
  memset(&dhcp_offer,0,sizeof(dhcp_offer));
  dhcp_state=DHCP_SELECTING;dhcp_attempts=0;dhcp_xid=transaction_id();
  dhcp_acquired=dhcp_next=clock_ticks();dhcp_infinite=false;
}
static bool dhcp_receive(uint32_t source,uint16_t sport,uint16_t dport,const uint8_t *p,unsigned length) {
  (void)source;
  if(sport!=67 || dport!=68 || dhcp_state==DHCP_OFF)return false;
  struct lease lease;
  if(!dhcp_parse(p,length,&lease))return true;
  uint32_t now=clock_ticks();
  if(dhcp_state==DHCP_SELECTING && lease.type==2 && lease.address && lease.server) {
    dhcp_offer=lease;dhcp_state=DHCP_REQUESTING;dhcp_attempts=0;dhcp_next=now;return true;
  }
  if(dhcp_state!=DHCP_REQUESTING && dhcp_state!=DHCP_RENEWING && dhcp_state!=DHCP_REBINDING)return true;
  if(!lease.server || (dhcp_state!=DHCP_REBINDING && lease.server!=dhcp_offer.server))return true;
  if(lease.type==6) {apply_config(0,0,0,0);dhcp_state=DHCP_RETRY;dhcp_next=now+100;return true;}
  if(lease.type!=5 || !lease.seconds)return true;
  if(!lease.address)lease.address=dhcp_offer.address;
  if(lease.address!=dhcp_offer.address)return true;
  if(!lease.has_mask)lease.mask=dhcp_offer.mask;
  if(!lease.has_router)lease.router=dhcp_offer.router;
  if(!lease.has_dns) {lease.dns=dhcp_offer.dns;lease.secondary=dhcp_offer.secondary;}
  uint32_t inverse=~lease.mask;
  if(!lease.address || !lease.mask || (inverse&(inverse+1)) ||
     (lease.router && (lease.router&lease.mask)!=(lease.address&lease.mask)))return true;
  apply_config(lease.address,lease.mask,lease.router,lease.dns);dns_secondary=lease.secondary;
  dhcp_offer=lease;dhcp_acquired=now;dhcp_infinite=lease.seconds==0xffffffffu;
  uint32_t lifetime=lease.seconds>21474836u?2147483600u:lease.seconds*100u;
  uint32_t renew=lease.renew && lease.renew<lease.seconds && lease.renew<=21474836u?lease.renew*100u:lifetime/2;
  uint32_t rebind=lease.rebind && lease.rebind<lease.seconds && lease.rebind<=21474836u?lease.rebind*100u:(uint64_t)lifetime*7/8;
  if(!renew || renew>=rebind || rebind>=lifetime) {renew=lifetime/2;rebind=(uint64_t)lifetime*7/8;}
  dhcp_expires=now+lifetime;dhcp_renew_at=now+renew;dhcp_rebind_at=now+rebind;
  dhcp_state=DHCP_BOUND;dhcp_attempts=0;return true;
}
void net_service_poll(void) {
  if(dhcp_polling || dhcp_state==DHCP_OFF || !link_ready)return;
  dhcp_polling=true;uint32_t now=clock_ticks();
  if((dhcp_state==DHCP_BOUND || dhcp_state==DHCP_RENEWING || dhcp_state==DHCP_REBINDING) && !dhcp_infinite) {
    if(due(now,dhcp_expires)) {net_dhcp_begin();goto done;}
    if(dhcp_state==DHCP_BOUND && due(now,dhcp_renew_at)) {
      dhcp_state=DHCP_RENEWING;dhcp_xid=transaction_id();dhcp_next=now;dhcp_attempts=0;
    }
    if(dhcp_state==DHCP_RENEWING && due(now,dhcp_rebind_at)) {dhcp_state=DHCP_REBINDING;dhcp_next=now;dhcp_attempts=0;}
  }
  if(dhcp_state==DHCP_BOUND)goto done;
  if(dhcp_state==DHCP_RETRY) {if(due(now,dhcp_next))net_dhcp_begin();goto done;}
  if(!due(now,dhcp_next))goto done;
  if(dhcp_attempts>=5 && (dhcp_state==DHCP_SELECTING || dhcp_state==DHCP_REQUESTING)) {
    dhcp_state=DHCP_RETRY;dhcp_next=now+3000;goto done;
  }
  uint8_t packet[320];uint32_t length;
  dhcp_packet(packet,dhcp_state==DHCP_SELECTING?1:3,&length);
  unsigned delay=100u<<(dhcp_attempts<4?dhcp_attempts:4);
  if(dhcp_attempts<31)dhcp_attempts++;
  dhcp_next=now+delay;
  net_udp_send(dhcp_state==DHCP_RENEWING?dhcp_offer.server:0xffffffffu,68,67,packet,length);
done:dhcp_polling=false;
}
int net_dhcp(void) {
  if(!link_ready)return -19;
  if(dhcp_state==DHCP_BOUND) {
    dhcp_state=DHCP_RENEWING;dhcp_xid=transaction_id();dhcp_next=clock_ticks();dhcp_attempts=0;
  } else if(dhcp_state==DHCP_OFF || dhcp_state==DHCP_RETRY)net_dhcp_begin();
  uint32_t started=clock_ticks();
  while((uint32_t)(clock_ticks()-started)<1200) {
    net_poll();if(dhcp_state==DHCP_BOUND)return 0;cpu_pause();
  }
  return -110;
}
void net_diagnostics(void) {
  net_info();
  console_write("Ethernet link: ");
  if(nic==RGE)console_write(rge_link()?"up\n":"down\n");
  else if(nic==RTL8169)console_write(rtl8169_link()?"up\n":"down\n");
  else if(nic==E1000)console_write(rd(8)&2?"up\n":"down\n");
  else console_write(link_ready?"driver initialized\n":"no supported driver\n");
  const char *states[]={"off/static","discovering","requesting","bound","renewing","rebinding","retry wait"};
  console_write("DHCP state: ");console_write(states[dhcp_state]);console_putc('\n');
  if(dhcp_state==DHCP_BOUND || dhcp_state==DHCP_RENEWING || dhcp_state==DHCP_REBINDING) {
    console_write("Lease server: ");ip_print(dhcp_offer.server);console_putc('\n');
    console_write("Lease age seconds: ");print_uint((clock_ticks()-dhcp_acquired)/100);console_putc('\n');
    console_write("Lease duration seconds: ");print_uint(dhcp_offer.seconds);console_putc('\n');
  }
  for(unsigned i=0;i<pci_count_devices();i++) {
    const struct pci_device *d=pci_device_at(i);
    if(d && d->class_id==2) {
      console_write("Network PCI ");print_hex(d->vendor);console_putc(':');print_hex(d->device);
      console_write(" revision ");print_hex(pci_read(d,8)&255);console_write(" class ");print_hex(d->subclass);console_putc('\n');
    }
  }
  rtl8169_diagnostics();
  rge_diagnostics();
  tcp_diagnostics();
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
  if (!ip)
    return -1;
  uint8_t query[512] = {0}, reply[1472];
  uint32_t nonce; if(!secure_random(&nonce,sizeof(nonce))) nonce=clock_ticks() ^ be32(mac);
  uint16_t id=nonce, port=49152+((nonce>>16)%16000);
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
  uint32_t resolvers[2] = {dns_server, dns_secondary};
  for (unsigned resolver = 0; resolver < 2; resolver++) {
    uint32_t server = resolvers[resolver];
    if (!server || (resolver && server == resolvers[0]))
      continue;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      if (net_udp_send(server, port, 53, query, pos) < 0)
        return -3;
      uint32_t start = clock_ticks();
      while (clock_ticks() - start < 200) {
        uint32_t source;
        uint16_t srcport;
        int result =
            net_udp_recv(port, reply, sizeof(reply), &source, &srcport);
        if (result < 12 || source != server || srcport != 53)
          continue;
        unsigned n = (unsigned)result;
        if (be16(reply) != id || !(be16(reply+2)&0x8000)) continue;
        if((be16(reply+2)&15)==3) return -2;
        if ((be16(reply+2)&0x7a0f)!=0) continue;
        unsigned off = 12, questions = be16(reply + 4),
                 answers = be16(reply + 6);
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
  }
  return -4;
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
      total += (uintptr_t)r;
    }
  }
  net_tcp_close();
  console_putc('\n');
}
