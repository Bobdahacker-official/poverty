#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/net.c"
static uint32_t tick=100;
static bool permit=true,server_enabled,offers=true;
static unsigned ack_mode=1,discoveries,renewals,rebindings,firewall_checks;
static uint8_t queued[16][1514],captured[16][1514],allocation[65536];
static unsigned queue_sizes[16],capture_sizes[16],qhead,qcount,capture_count;
static const uint32_t offered_ip=0xc0a8585a,server_ip=0xc0a85801;
uint32_t clock_ticks(void) {return tick;}
void devices_poll(void) {}
bool secure_random(void *p,unsigned n) {assert(n==4);uint32_t v=tick^0x52f43018;memcpy(p,&v,4);return true;}
void *pages_alloc(unsigned n) {assert(n<=16);return allocation;}
bool page_free(void *p) {(void)p;return true;}
bool firewall_packet(bool outbound,const uint8_t *h,unsigned n) {(void)outbound;assert(n>=20 && be16(h+2)==n);firewall_checks++;return permit;}
void tcp_poll(void) {}
void tcp_reset_all(void) {}
void tcp_input(uint32_t a,const uint8_t *b,uint32_t c,const uint8_t *d) {(void)a;(void)b;(void)c;(void)d;assert(0);}
bool pcnet_send(const void *p,unsigned n) {(void)p;(void)n;assert(0);return false;}
void pcnet_poll(void (*receive)(const uint8_t *,uint32_t)) {(void)receive;assert(0);}
bool rtl8169_send(const void *p,unsigned n) {(void)p;(void)n;assert(0);return false;}
void rtl8169_poll(void (*receive)(const uint8_t *,uint32_t)) {(void)receive;assert(0);}
bool rge_send(const void *p,unsigned n) {(void)p;(void)n;assert(0);return false;}
void rge_poll(void (*receive)(const uint8_t *,uint32_t)) {(void)receive;assert(0);}
static uint8_t *queue(unsigned size) {
  assert(qcount<16);unsigned at=(qhead+qcount++)%16;queue_sizes[at]=size;memset(queued[at],0,size);return queued[at];
}
static void option(uint8_t *p,unsigned *at,unsigned key,uint32_t value) {p[(*at)++]=key;p[(*at)++]=4;put32(p+*at,value);*at+=4;}
static void dhcp_reply(unsigned type,uint32_t xid) {
  uint8_t packet[320]={2,1,6};put32(packet+4,xid);put32(packet+16,offered_ip);memcpy(packet+28,mac,6);put32(packet+236,0x63825363);
  unsigned at=240;packet[at++]=53;packet[at++]=1;packet[at++]=type;
  option(packet,&at,54,server_ip);option(packet,&at,1,0xffffff00);option(packet,&at,3,server_ip);
  packet[at++]=6;packet[at++]=8;put32(packet+at,0xc0a85835);put32(packet+at+4,0x09090909);at+=8;
  option(packet,&at,51,8);option(packet,&at,58,2);option(packet,&at,59,5);packet[at++]=255;
  uint8_t *frame=queue(42+at);memcpy(frame,mac,6);frame[6]=2;frame[11]=2;put16(frame+12,0x800);
  uint8_t *h=frame+14;h[0]=0x45;put16(h+2,28+at);h[8]=64;h[9]=17;put32(h+12,server_ip);put32(h+16,0xffffffff);put16(h+10,checksum(h,20));
  put16(h+20,67);put16(h+22,68);put16(h+24,8+at);memcpy(h+28,packet,at);
}
bool virtio_net_send(const void *buffer,unsigned size) {
  const uint8_t *frame=buffer;
  if(!server_enabled) {assert(capture_count<16);memcpy(captured[capture_count],frame,size);capture_sizes[capture_count++]=size;return true;}
  if(be16(frame+12)==0x806) {
    if(be32(frame+38)!=server_ip)return true;
    uint8_t *reply=queue(42);memcpy(reply,mac,6);reply[6]=2;reply[11]=2;put16(reply+12,0x806);
    put16(reply+14,1);put16(reply+16,0x800);reply[18]=6;reply[19]=4;put16(reply+20,2);
    memcpy(reply+22,reply+6,6);put32(reply+28,server_ip);memcpy(reply+32,mac,6);put32(reply+38,ip);return true;
  }
  if(be16(frame+12)!=0x800 || frame[23]!=17 || be16(frame+36)!=67)return true;
  const uint8_t *packet=frame+42;unsigned n=be16(frame+16)-28,type=0;
  for(unsigned at=240;at<n;) {unsigned code=packet[at++];if(code==255)break;if(!code)continue;unsigned len=packet[at++];assert(len<=n-at);if(code==53)type=packet[at];at+=len;}
  if(type==1) {discoveries++;if(offers)dhcp_reply(2,be32(packet+4));}
  if(type==3) {
    if(be32(packet+12)) {assert(be32(packet+12)==offered_ip);if(be32(frame+30)==0xffffffff)rebindings++;else renewals++;}
    if(ack_mode)dhcp_reply(ack_mode==1?5:6,be32(packet+4));
  }
  return true;
}
void virtio_net_poll(void (*callback)(const uint8_t *,uint32_t)) {
  unsigned budget=qcount;
  while(budget-- && qcount) {unsigned at=qhead;qhead=(qhead+1)%16;qcount--;callback(queued[at],queue_sizes[at]);}
}
int main(void) {
  nic=VIRTIO;link_ready=true;mac[0]=2;mac[5]=1;
  net_config(0xc0a80102,0xffffff00,0,0);
  uint8_t data[4000],out[4000];for(unsigned i=0;i<sizeof(data);i++)data[i]=i*17;
  assert(net_udp_send(0xffffffff,1234,9000,data,sizeof(data))==sizeof(data));assert(capture_count==3 && firewall_checks==1);
  unsigned id=be16(captured[0]+18),offset=0;
  for(unsigned i=0;i<3;i++) {
    const uint8_t *h=captured[i]+14;assert(be16(h+4)==id && (be16(h+6)&8191)*8==offset);
    assert(!!(be16(h+6)&0x2000)==(i<2));offset+=be16(h+2)-20;
  }
  assert(offset==4008);receive(captured[2],capture_sizes[2]);receive(captured[0],capture_sizes[0]);receive(captured[1],capture_sizes[1]);
  uint32_t sender;uint16_t port;assert(net_udp_recv(9000,out,sizeof(out),&sender,&port)==sizeof(out));assert(sender==0xc0a80102 && port==1234 && !memcmp(data,out,sizeof(out)));
  permit=false;capture_count=0;assert(net_udp_send(0xffffffff,1234,9000,data,sizeof(data))<0 && !capture_count);permit=true;
  puts("PASS outgoing fragmentation, shared datagram IDs, complete UDP reassembly and firewall enforcement");
  server_enabled=true;net_dhcp_begin();assert(!ip);
  for(unsigned i=0;i<8;i++) {net_poll();tick++;}
  assert(dhcp_state==DHCP_BOUND && ip==offered_ip && dns_server==0xc0a85835 && dns_secondary==0x09090909);
  uint32_t hop;assert(route_lookup(0x08080808,&hop) && hop==server_ip);
  tick=dhcp_acquired+200;for(unsigned i=0;i<5;i++)net_poll();assert(renewals && dhcp_state==DHCP_BOUND);
  ack_mode=0;tick=dhcp_acquired+200;net_poll();tick=dhcp_acquired+500;net_poll();assert(rebindings && dhcp_state==DHCP_REBINDING);
  offers=false;tick=dhcp_acquired+800;net_poll();assert(!ip && !dns_server && dhcp_state==DHCP_SELECTING);
  offers=true;ack_mode=1;for(unsigned i=0;i<8;i++)net_poll();assert(dhcp_state==DHCP_BOUND);
  ack_mode=2;tick=dhcp_acquired+200;for(unsigned i=0;i<5;i++)net_poll();assert(!ip && dhcp_state==DHCP_RETRY);
  puts("PASS DHCP discovery, ACK, secondary DNS, T1 renewal, T2 rebinding, expiry and NAK invalidation");
  return 0;
}
