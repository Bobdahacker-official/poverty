#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/tcp.c"
static uint32_t tick=10;
static uint8_t frames[512][1514];static unsigned sizes[512],sent;
uint32_t clock_ticks(void) {return tick;}
void net_snapshot(struct network_info *info) {memset(info,0,sizeof(*info));info->ip=0x0a00020f;info->present=true;}
void net_device_mac(uint8_t out[6]) {const uint8_t a[6]={2,0,0,0,0,1};memcpy(out,a,6);}
bool net_resolve(uint32_t destination,uint8_t *out) {(void)destination;const uint8_t a[6]={2,0,0,0,0,2};memcpy(out,a,6);return true;}
bool secure_random(void *out,unsigned size) {assert(size==4);uint32_t v=0x12340000+sent;memcpy(out,&v,4);return true;}
bool net_frame_send(const void *p,uint32_t n) {
  assert(sent<512 && n<=1514);memcpy(frames[sent],p,n);sizes[sent++]=n;
  const uint8_t *ip=(const uint8_t *)p+14;unsigned total=be16(ip+2);
  assert(!checksum(ip,20,0));uint32_t pseudo=(be32(ip+12)>>16)+(be32(ip+12)&65535)+(be32(ip+16)>>16)+(be32(ip+16)&65535)+6+total-20;
  assert(!checksum(ip+20,total-20,pseudo));return true;
}
void net_poll(void) {tick++;tcp_poll();}
static void packet(uint16_t port,uint32_t sequence,uint32_t ack,uint8_t flags,unsigned window,const void *data,unsigned n,const uint8_t *opts,unsigned options_size) {
  uint8_t p[1600]={0},hardware[6]={2,0,0,0,0,2};unsigned header=20+options_size,total=header+n;
  put16(p,port);put16(p+2,9000);put32(p+4,sequence);put32(p+8,ack);p[12]=(header/4)<<4;p[13]=flags;put16(p+14,window);
  if(options_size)memcpy(p+20,opts,options_size);
  if(n)memcpy(p+header,data,n);
  uint32_t pseudo=0x0a00+0x0202+0x0a00+0x020f+6+total;put16(p+16,checksum(p,total,pseudo));
  tcp_input(0x0a000202,p,total,hardware);
}
static const uint8_t syn_options[12]={2,4,5,180,4,2,1,3,3,7,1,1};
static const uint8_t *last(void) {assert(sent);return frames[sent-1]+34;}
static bool has_option(const uint8_t *p,unsigned kind,unsigned value) {
  unsigned size=(p[12]>>4)*4;
  for(unsigned i=20;i<size;) {unsigned k=p[i++];if(!k)break;if(k==1)continue;unsigned n=p[i++];if(n<2 || n-2>size-i)return false;if(k==kind && (kind!=3 || p[i]==value))return true;i+=n-2;}
  return false;
}
int main(void) {
  int listener=net_tcp_socket();assert(listener>0);assert(!net_tcp_bind(listener,0,9000));assert(!net_tcp_listen(listener,2));
  packet(40000,1000,0,2,60000,NULL,0,syn_options,12);
  assert((last()[13]&0x12)==0x12 && has_option(last(),4,0) && has_option(last(),3,WS));
  uint32_t initial=be32(last()+4);packet(40000,1001,initial+1,16,60000,NULL,0,NULL,0);
  assert(net_tcp_pending(listener)==1);int child=net_tcp_accept(listener);assert(child>0 && !net_tcp_pending(listener));
  packet(40000,1006,initial+1,24,60000,"world",5,NULL,0);
  assert(be32(last()+8)==1001 && has_option(last(),5,0));
  const uint8_t *ack=last();assert(be32(ack+24)==1006 && be32(ack+28)==1011);
  packet(40000,1001,initial+1,24,60000,"hello",5,NULL,0);
  char out[16]={0};assert(net_tcp_socket_recv(child,out,10,0)==10 && !memcmp(out,"helloworld",10));
  puts("PASS SYN/SYN-ACK, accept, negotiated SACK/window scaling and out-of-order receive");
  uint8_t data[20000];for(unsigned i=0;i<sizeof(data);i++)data[i]=i;
  packet(40000,1011,initial+1,16,2,NULL,0,NULL,0);
  unsigned before=sent;assert(net_tcp_socket_send(child,data,sizeof(data),true)==sizeof(data));
  assert(sent==before+1 && sizes[sent-1]-54==256); /* Peer shift 7 turns window 2 into 256 bytes. */
  packet(40000,1011,initial+257,16,60000,NULL,0,NULL,0);
  assert(sent>=before+5); /* Multiple segments in flight before another cumulative ACK. */
  uint8_t selective[12]={1,1,5,10};uint32_t hole=initial+257;
  put32(selective+4,hole+MSS);put32(selective+8,hole+3*MSS);
  unsigned retransmissions=sent;
  for(unsigned i=0;i<3;i++)packet(40000,1011,hole,16,60000,NULL,0,selective,12);
  bool repaired=false;
  for(unsigned i=retransmissions;i<sent;i++)if(be32(frames[i]+38)==hole && sizes[i]>54)repaired=true;
  assert(repaired);
  puts("PASS peer SACK scoreboard and fast retransmission of a missing range");
  for(unsigned round=0;round<12;round++) {
    uint32_t highest=initial+257;
    for(unsigned i=before;i<sent;i++) {const uint8_t *t=frames[i]+34;unsigned payload=sizes[i]-34-(t[12]>>4)*4;if(payload && after(be32(t+4)+payload,highest))highest=be32(t+4)+payload;}
    packet(40000,1011,highest,16,60000,NULL,0,NULL,0);
    if(highest==initial+1+sizeof(data))break;
  }
  assert(!connections[child].tx_count && connections[child].snd_una==initial+1+sizeof(data));
  assert(!net_tcp_shutdown(child,1));assert(last()[13]&1);uint32_t fin=be32(last()+4);
  assert(net_tcp_socket_send(child,"x",1,true)==-32);
  packet(40000,1011,fin+1,24,60000,"tail",4,NULL,0);
  assert(net_tcp_socket_recv(child,out,4,0)==4 && !memcmp(out,"tail",4));
  packet(40000,1015,fin+1,17,60000,NULL,0,NULL,0);assert(be32(last()+8)==1016);
  packet(40000,1016,0,4,60000,NULL,0,NULL,0);assert(connections[child].state==TIME_WAIT);
  assert(!net_tcp_socket_close(child));
  tick+=3001;tcp_poll();assert(!connections[child].used);
  puts("PASS scaled send window, sliding-window delivery, write half-close, FIN handshake and TIME_WAIT protection/reclamation");
  packet(41000,2000,0,2,60000,NULL,0,syn_options,12);
  packet(41001,3000,0,2,60000,NULL,0,syn_options,12);before=sent;
  packet(41002,4000,0,2,60000,NULL,0,syn_options,12);assert(sent==before);
  assert(connections[listener].pending_count==2);assert(!net_tcp_socket_close(listener));
  puts("PASS bounded half-open backlog and listener cleanup");
  return 0;
}
