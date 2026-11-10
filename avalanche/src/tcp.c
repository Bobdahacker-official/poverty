#include "system.h"
#include "tcp.h"
#define SOCKETS 64
#define BACKLOG 16
#define RX_SIZE 131072u
#define TX_SIZE 65536u
#define MSS 1460u
#define SACKS 4
#define WS 2u
enum state {FREE,RESERVED,LISTEN,SYN_SENT,SYN_RECEIVED,ESTABLISHED,FIN_WAIT1,FIN_WAIT2,CLOSE_WAIT,LAST_ACK,TIME_WAIT,CLOSED};
struct range {uint32_t left,right;};
struct connection {
  enum state state;bool used,detached,reset,peer_closed,scale,sack,fin_pending,closing,reuse,read_shutdown,recovering;
  uint8_t peer_scale,hardware[6],retries,duplicate_acks;
  uint16_t local_port,remote_port;
  int parent,error;
  uint32_t local_ip,remote,snd_una,snd_nxt,rcv_nxt,peer_window,mss;
  uint32_t rx_head,rx_count,rx_pending,last_sack_left,last_sack_right,fin_sequence;
  uint32_t tx_head,tx_count,tx_sent,cwnd,ssthresh,rto,deadline,idle_at,probe_at;
  uint32_t fin_sent_sequence,recovery_point;
  struct range sacks[SACKS];unsigned sack_count;
  uint8_t pending[BACKLOG];unsigned pending_count,backlog;
  uint8_t received[RX_SIZE/8],rx[RX_SIZE],tx[TX_SIZE];
};
static struct connection connections[SOCKETS];
static uint16_t ephemeral=49151;
static bool polling;
static uint16_t be16(const uint8_t *p) {return (uint16_t)p[0]<<8|p[1];}
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t x) {p[0]=x>>8;p[1]=x;}
static void put32(uint8_t *p,uint32_t x) {p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static uint16_t checksum(const uint8_t *p,unsigned length,uint32_t sum) {
  while(length>1){sum+=be16(p);p+=2;length-=2;}if(length)sum+=(uint32_t)*p<<8;
  while(sum>>16)sum=(sum&65535)+(sum>>16);
  return (uint16_t)~sum;
}
static bool after(uint32_t a,uint32_t b) {return (int32_t)(a-b)>0;}
static bool due(uint32_t now,uint32_t at) {return (int32_t)(now-at)>=0;}
static struct connection *get(int id) {return id>=0 && id<SOCKETS && connections[id].used?&connections[id]:NULL;}
static uint32_t local_address(void) {struct network_info info;net_snapshot(&info);return info.ip;}
static void initialize(struct connection *c) {
  memset(c,0,sizeof(*c));c->used=true;c->state=RESERVED;c->parent=-1;c->mss=MSS;
  c->cwnd=10*MSS;c->ssthresh=TX_SIZE;c->rto=100;c->idle_at=clock_ticks();
}
bool net_tcp_hungup(int id) {struct connection *c=get(id);return !c || c->reset || c->state==CLOSED || (c->peer_closed && c->closing);}
int net_tcp_socket_error(int id,bool clear) {struct connection *c=get(id);if(!c)return 9;int error=c->error;if(clear)c->error=0;return error;}
int net_tcp_socket(void) {
  for(unsigned i=1;i<SOCKETS;i++)if(!connections[i].used) {initialize(&connections[i]);return i;}
  return -23;
}
void net_tcp_reuse(int id,bool reuse) {struct connection *c=get(id);if(c)c->reuse=reuse;}
static bool port_used(unsigned port,uint32_t address,int except) {
  for(unsigned i=0;i<SOCKETS;i++)if((int)i!=except && connections[i].used && connections[i].local_port==port &&
    (!address || !connections[i].local_ip || address==connections[i].local_ip) &&
    !(connections[except].reuse && connections[i].reuse && connections[i].state!=LISTEN && connections[i].state!=RESERVED))return true;
  return false;
}
static int choose_port(struct connection *c,int id) {
  for(unsigned i=0;i<16384;i++) {
    if(++ephemeral<49152)ephemeral=49152;
    if(!port_used(ephemeral,c->local_ip,id)) {c->local_port=ephemeral;return 0;}
  }
  return -98;
}
int net_tcp_bind(int id,uint32_t address,uint16_t port) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->state!=RESERVED || c->local_port)return -22;
  if(address && address!=local_address())return -99;
  if(port && port_used(port,address,id))return -98;
  c->local_ip=address;c->local_port=port;
  return port?0:choose_port(c,id);
}
int net_tcp_listen(int id,unsigned backlog) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->state!=RESERVED && c->state!=LISTEN)return -22;
  if(!c->local_port) {int r=choose_port(c,id);if(r)return r;}
  c->backlog=backlog?backlog:1;if(c->backlog>BACKLOG)c->backlog=BACKLOG;
  c->state=LISTEN;return 0;
}
static bool bit(const struct connection *c,unsigned at) {return (c->received[at/8]>>(at&7))&1;}
static void set_bit(struct connection *c,unsigned at,bool on) {
  if(on)c->received[at/8]|=1u<<(at&7);else c->received[at/8]&=~(1u<<(at&7));
}
static unsigned sack_blocks(struct connection *c,struct range out[4]) {
  if(!c->sack || !c->rx_pending)return 0;
  unsigned count=0,base=(c->rx_head+c->rx_count)%RX_SIZE,space=RX_SIZE-c->rx_count;
  uint32_t recent=c->last_sack_left-c->rcv_nxt;
  if(recent<space && bit(c,(base+recent)%RX_SIZE)) {
    unsigned first=recent,last=recent;
    while(first && bit(c,(base+first-1)%RX_SIZE))first--;
    while(last<space && bit(c,(base+last)%RX_SIZE))last++;
    out[count++]=(struct range){c->rcv_nxt+first,c->rcv_nxt+last};
  }
  for(unsigned offset=0;offset<space && count<4;) {
    if(!bit(c,(base+offset)%RX_SIZE)) {offset++;continue;}
    unsigned first=offset;while(offset<space && bit(c,(base+offset)%RX_SIZE))offset++;
    if(count && out[0].left==c->rcv_nxt+first)continue;
    out[count++]=(struct range){c->rcv_nxt+first,c->rcv_nxt+offset};
  }
  return count;
}
static void emit(struct connection *c,uint8_t flags,uint32_t sequence,const void *data,unsigned size) {
  uint8_t frame[1514];uint8_t *h=frame+14,*t=h+20;unsigned options=0;
  memcpy(frame,c->hardware,6);net_device_mac(frame+6);put16(frame+12,0x800);
  memset(h,0,20);memset(t,0,60);
  if(flags&2) {
    t[20]=2;t[21]=4;put16(t+22,MSS);options=4;
    if(c->state==SYN_SENT || c->sack) {t[20+options++]=4;t[20+options++]=2;}
    if(c->state==SYN_SENT || c->scale) {t[20+options++]=1;t[20+options++]=3;t[20+options++]=3;t[20+options++]=WS;}
  } else if(!size) {
    struct range blocks[4];unsigned n=sack_blocks(c,blocks);
    if(n) {
      t[20]=1;t[21]=1;t[22]=5;t[23]=2+8*n;options=4+8*n;
      for(unsigned i=0;i<n;i++) {put32(t+24+i*8,blocks[i].left);put32(t+28+i*8,blocks[i].right);}
    }
  }
  while(options&3)t[20+options++]=1;
  unsigned header=20+options,total=header+size;
  if(total>1480)return;
  h[0]=0x45;put16(h+2,20+total);put16(h+6,0x4000);h[8]=64;h[9]=6;
  uint32_t local=c->local_ip?c->local_ip:local_address();
  put32(h+12,local);put32(h+16,c->remote);put16(h+10,checksum(h,20,0));
  put16(t,c->local_port);put16(t+2,c->remote_port);put32(t+4,sequence);put32(t+8,c->rcv_nxt);
  t[12]=(header/4)<<4;t[13]=flags;
  uint32_t window=RX_SIZE-c->rx_count;
  if(!(flags&2) && c->scale)window>>=WS;
  if(window>65535)window=65535;
  put16(t+14,window);
  if(size)memcpy(t+header,data,size);
  uint32_t pseudo=(local>>16)+(local&65535)+(c->remote>>16)+(c->remote&65535)+6+total;
  put16(t+16,checksum(t,total,pseudo));net_frame_send(frame,34+total);
}
static void options(struct connection *c,const uint8_t *p,unsigned header,bool syn) {
  if(syn) {c->sack=false;c->scale=false;c->peer_scale=0;c->mss=536;}
  for(unsigned at=20;at<header;) {
    unsigned kind=p[at++];if(!kind)break;if(kind==1)continue;
    if(at>=header)break;
    unsigned length=p[at++];if(length<2 || length-2>header-at)break;
    if(syn && kind==2 && length==4) {unsigned m=be16(p+at);if(m>=64)c->mss=m<MSS?m:MSS;}
    if(syn && kind==3 && length==3) {c->scale=true;c->peer_scale=p[at]>14?14:p[at];}
    if(syn && kind==4 && length==2)c->sack=true;
    if(!syn && c->sack && kind==5 && length>=10 && !((length-2)&7)) {
      for(unsigned j=0;j<(length-2)/8;j++) {
        struct range r={be32(p+at+j*8),be32(p+at+j*8+4)};
        if(!after(r.right,r.left) || after(c->snd_una,r.left) || after(r.right,c->snd_nxt))continue;
        bool merged=false;
        for(unsigned i=0;i<c->sack_count;i++)if(!after(r.left,c->sacks[i].right) && !after(c->sacks[i].left,r.right)) {
          if(after(c->sacks[i].left,r.left))c->sacks[i].left=r.left;
          if(after(r.right,c->sacks[i].right))c->sacks[i].right=r.right;
          merged=true;break;
        }
        if(!merged && c->sack_count<SACKS)c->sacks[c->sack_count++]=r;
      }
    }
    at+=length-2;
  }
  if(syn)c->cwnd=10*c->mss;
}
static void transmit(struct connection *c,unsigned offset,unsigned size) {
  uint8_t data[MSS];for(unsigned i=0;i<size;i++)data[i]=c->tx[(c->tx_head+offset+i)%TX_SIZE];
  emit(c,0x18,c->snd_una+offset,data,size);
}
static void retransmit(struct connection *c) {
  if(!c->tx_sent)return;
  unsigned offset=0;
  for(unsigned pass=0;pass<SACKS;pass++) {
    bool moved=false;
    for(unsigned i=0;i<c->sack_count;i++) {
      uint32_t seq=c->snd_una+offset;
      if(!after(c->sacks[i].left,seq) && after(c->sacks[i].right,seq)) {offset=c->sacks[i].right-c->snd_una;moved=true;break;}
    }
    if(!moved)break;
  }
  if(offset>=c->tx_sent)offset=0;
  unsigned size=c->tx_sent-offset;if(size>c->mss)size=c->mss;
  for(unsigned i=0;i<c->sack_count;i++)if(after(c->sacks[i].left,c->snd_una+offset) &&
      c->sacks[i].left-(c->snd_una+offset)<size)size=c->sacks[i].left-(c->snd_una+offset);
  if(size)transmit(c,offset,size);
}
static void flush(struct connection *c) {
  if(c->state!=ESTABLISHED && c->state!=CLOSE_WAIT)return;
  uint32_t limit=c->cwnd<c->peer_window?c->cwnd:c->peer_window;
  for(unsigned budget=0;budget<4 && c->tx_sent<c->tx_count && c->tx_sent<limit;budget++) {
    unsigned size=c->tx_count-c->tx_sent;if(size>c->mss)size=c->mss;
    if(size>limit-c->tx_sent)size=limit-c->tx_sent;
    if(!c->tx_sent)c->deadline=clock_ticks()+c->rto;
    transmit(c,c->tx_sent,size);c->tx_sent+=size;c->snd_nxt=c->snd_una+c->tx_sent;
  }
  if(c->closing && !c->tx_count) {
    c->fin_sent_sequence=c->snd_nxt++;c->state=c->peer_closed?LAST_ACK:FIN_WAIT1;
    c->deadline=clock_ticks()+100;c->retries=0;emit(c,0x11,c->fin_sent_sequence,NULL,0);
  }
}
static void acknowledged(struct connection *c,uint32_t ack,unsigned raw_window,bool duplicate) {
  uint32_t previous_window=c->peer_window;
  if(after(ack,c->snd_nxt))return;
  c->peer_window=(uint32_t)raw_window<<(c->scale?c->peer_scale:0);
  if(after(ack,c->snd_una)) {
    unsigned bytes=ack-c->snd_una;if(bytes>c->tx_sent)bytes=c->tx_sent;
    c->tx_head=(c->tx_head+bytes)%TX_SIZE;c->tx_count-=bytes;c->tx_sent-=bytes;c->snd_una+=bytes;
    c->duplicate_acks=0;c->retries=0;c->rto=100;
    if(bytes) {
      if(c->recovering) {
        if(!after(c->recovery_point,ack)) {c->recovering=false;c->cwnd=c->ssthresh;}
        else c->cwnd=c->ssthresh+3*c->mss;
      }
      else if(c->cwnd<c->ssthresh)c->cwnd+=bytes;
      else {unsigned add=c->mss*c->mss/c->cwnd;c->cwnd+=add?add:1;}
      if(c->cwnd>TX_SIZE)c->cwnd=TX_SIZE;
    }
    unsigned keep=0;for(unsigned i=0;i<c->sack_count;i++)if(after(c->sacks[i].right,c->snd_una)) {
      if(after(c->snd_una,c->sacks[i].left))c->sacks[i].left=c->snd_una;
      c->sacks[keep++]=c->sacks[i];
    }
    c->sack_count=keep;c->deadline=clock_ticks()+c->rto;
    if(bytes && c->recovering && after(c->recovery_point,ack))retransmit(c);
    if((c->state==FIN_WAIT1 || c->state==LAST_ACK) && ack==c->fin_sent_sequence+1) {
      c->snd_una=ack;
      if(c->state==LAST_ACK) {c->state=CLOSED;if(c->detached)c->used=false;}
      else if(c->peer_closed) {c->state=TIME_WAIT;c->deadline=clock_ticks()+3000;}
      else {c->state=FIN_WAIT2;c->deadline=clock_ticks()+3000;}
    }
  } else if(duplicate && previous_window==c->peer_window && ack==c->snd_una && c->tx_sent) {
    if(c->duplicate_acks<255)c->duplicate_acks++;
    if(c->duplicate_acks==3) {
      c->ssthresh=c->cwnd/2;if(c->ssthresh<2*c->mss)c->ssthresh=2*c->mss;
      c->recovering=true;c->recovery_point=c->snd_nxt;
      c->cwnd=c->ssthresh+3*c->mss;retransmit(c);c->deadline=clock_ticks()+c->rto;
    } else if(c->duplicate_acks>3 && c->recovering) {
      c->cwnd+=c->mss;if(c->cwnd>TX_SIZE)c->cwnd=TX_SIZE;
    }
  }
}
static uint32_t initial_sequence(void) {
  uint32_t sequence;if(!secure_random(&sequence,sizeof(sequence)))sequence=clock_ticks()*1103515245u^0x9e3779b9u;
  return sequence;
}
static void pending_clean(struct connection *listener) {
  unsigned keep=0;for(unsigned i=0;i<listener->pending_count;i++) {
    struct connection *c=get(listener->pending[i]);
    if(c && !c->reset && c->state!=CLOSED)listener->pending[keep++]=listener->pending[i];
    else if(c)c->used=false;
  }
  listener->pending_count=keep;
}
unsigned net_tcp_pending(int id) {
  struct connection *c=get(id);if(!c || c->state!=LISTEN)return 0;
  pending_clean(c);unsigned n=0;
  for(unsigned i=0;i<c->pending_count;i++)if(connections[c->pending[i]].state==ESTABLISHED)n++;
  return n;
}
int net_tcp_accept(int id) {
  struct connection *c=get(id);if(!c || c->state!=LISTEN)return -22;
  pending_clean(c);
  for(unsigned i=0;i<c->pending_count;i++) {
    int accepted=c->pending[i];if(connections[accepted].state!=ESTABLISHED)continue;
    for(unsigned j=i+1;j<c->pending_count;j++)c->pending[j-1]=c->pending[j];
    c->pending_count--;connections[accepted].parent=-1;return accepted;
  }
  return -11;
}
void tcp_input(uint32_t source,const uint8_t *p,uint32_t length,const uint8_t *hardware) {
  if(length<20 || length>65515)return;
  unsigned header=(p[12]>>4)*4;if(header<20 || header>length)return;
  uint32_t local=local_address(),pseudo=(source>>16)+(source&65535)+(local>>16)+(local&65535)+6+length;
  if(checksum(p,length,pseudo))return;
  uint16_t sport=be16(p),dport=be16(p+2);uint8_t flags=p[13];
  uint32_t sequence=be32(p+4),ack=be32(p+8);unsigned bytes=length-header;
  struct connection *c=NULL,*listener=NULL;
  for(unsigned i=0;i<SOCKETS;i++) {
    struct connection *s=&connections[i];if(!s->used || s->local_port!=dport)continue;
    if(s->state==LISTEN && (!s->local_ip || s->local_ip==local))listener=s;
    else if(s->state!=RESERVED && s->state!=LISTEN && s->remote==source && s->remote_port==sport) {c=s;break;}
  }
  if(!c) {
    if(listener && (flags&0x17)==2 && !bytes) {
      pending_clean(listener);if(listener->pending_count>=listener->backlog)return;
      int id=net_tcp_socket();if(id<0)return;c=&connections[id];
      c->local_ip=local;c->remote=source;c->local_port=dport;c->remote_port=sport;
      memcpy(c->hardware,hardware,6);options(c,p,header,true);c->rcv_nxt=sequence+1;
      c->snd_una=initial_sequence();c->snd_nxt=c->snd_una+1;c->peer_window=be16(p+14);
      c->reuse=listener->reuse;c->state=SYN_RECEIVED;c->parent=listener-connections;c->deadline=clock_ticks()+100;
      listener->pending[listener->pending_count++]=id;emit(c,0x12,c->snd_una,NULL,0);return;
    }
    if(!(flags&4)) {
      /* Only the transport fields are needed for a stateless reset. */
      static struct connection reset_reply;
      reset_reply.local_ip=local;reset_reply.remote=source;reset_reply.local_port=dport;reset_reply.remote_port=sport;
      memcpy(reset_reply.hardware,hardware,6);reset_reply.rcv_nxt=sequence+bytes+!!(flags&2)+!!(flags&1);
      emit(&reset_reply,flags&16?4:0x14,flags&16?ack:0,NULL,0);
    }
    return;
  }
  c->idle_at=clock_ticks();
  if(c->state==TIME_WAIT && (flags&4)) return;
  if((flags&3)==3) return;
  if(flags&4) {
    if((c->state==SYN_SENT && (flags&16) && ack==c->snd_nxt) || (c->state!=SYN_SENT && sequence==c->rcv_nxt)) {
      c->error=c->state==SYN_SENT?111:104;
      c->reset=true;c->state=CLOSED;if(c->detached)c->used=false;
    } else if(c->state!=SYN_SENT)emit(c,16,c->snd_nxt,NULL,0);
    return;
  }
  if(c->state==SYN_SENT) {
    if((flags&0x12)!=0x12 || ack!=c->snd_nxt)return;
    options(c,p,header,true);c->snd_una=ack;c->rcv_nxt=sequence+1;c->peer_window=be16(p+14);
    c->state=ESTABLISHED;c->retries=0;emit(c,16,c->snd_nxt,NULL,0);return;
  }
  if(c->state==SYN_RECEIVED) {
    if((flags&0x17)==2 && sequence+1==c->rcv_nxt) {emit(c,0x12,c->snd_una,NULL,0);return;}
    if(!(flags&16) || ack!=c->snd_nxt || sequence!=c->rcv_nxt)return;
    c->snd_una=ack;c->state=ESTABLISHED;c->retries=0;
  }
  if(c->state==TIME_WAIT) {if(flags&1) {emit(c,16,c->snd_nxt,NULL,0);c->deadline=clock_ticks()+3000;}return;}
  if(c->state==CLOSED || !(flags&16))return;
  if(after(ack,c->snd_nxt)) {emit(c,16,c->snd_nxt,NULL,0);return;}
  int32_t relative=sequence-c->rcv_nxt;
  unsigned segment=bytes+!!(flags&1)+!!(flags&2),window=RX_SIZE-c->rx_count;
  if(!c->scale && window>65535)window=65535;
  if((relative>=0 && ((uint32_t)relative>window || ((uint32_t)relative==window && segment))) ||
     (relative<0 && (uint32_t)-relative>segment)) {emit(c,16,c->snd_nxt,NULL,0);return;}
  options(c,p,header,false);acknowledged(c,ack,be16(p+14),!bytes && !(flags&3));
  if(c->state==CLOSED)return;
  if(flags&2) {emit(c,16,c->snd_nxt,NULL,0);return;}
  uint32_t fin=sequence+bytes;
  if((flags&1) && !c->peer_closed && !after(c->rcv_nxt,fin) && fin-c->rcv_nxt<window) {
    if(c->fin_pending && c->fin_sequence!=fin) {emit(c,16,c->snd_nxt,NULL,0);return;}
    c->fin_pending=true;c->fin_sequence=fin;
  }
  if(bytes && !c->peer_closed) {
    int32_t delta=sequence-c->rcv_nxt;
    if(delta<0) {
      unsigned trim=(unsigned)-delta;if(trim>bytes)trim=bytes;
      p+=trim;bytes-=trim;sequence+=trim;delta=sequence-c->rcv_nxt;
    }
    unsigned space=RX_SIZE-c->rx_count;
    if(!c->scale && space>65535)space=65535;
    if(delta>=0 && (uint32_t)delta<space) {
      if(bytes>space-(unsigned)delta)bytes=space-delta;
      unsigned base=(c->rx_head+c->rx_count)%RX_SIZE;
      for(unsigned i=0;i<bytes;i++) {
        unsigned at=(base+(unsigned)delta+i)%RX_SIZE;
        if(!bit(c,at)) {c->rx[at]=p[header+i];set_bit(c,at,true);c->rx_pending++;}
      }
      c->last_sack_left=sequence;c->last_sack_right=sequence+bytes;
      while(c->rx_count<RX_SIZE && !(c->fin_pending && c->rcv_nxt==c->fin_sequence) && bit(c,(c->rx_head+c->rx_count)%RX_SIZE)) {
        set_bit(c,(c->rx_head+c->rx_count)%RX_SIZE,false);c->rx_count++;c->rx_pending--;c->rcv_nxt++;
      }
    }
  }
  if(c->fin_pending && c->fin_sequence==c->rcv_nxt) {
    c->fin_pending=false;c->rcv_nxt++;c->peer_closed=true;
    c->rx_pending=0;memset(c->received,0,sizeof(c->received));
    if(c->state==ESTABLISHED)c->state=CLOSE_WAIT;
    else if(c->state==FIN_WAIT2) {c->state=TIME_WAIT;c->deadline=clock_ticks()+3000;}
  }
  if(c->read_shutdown && c->rx_count) {c->rx_head=(c->rx_head+c->rx_count)%RX_SIZE;c->rx_count=0;}
  if(length>header || (flags&1))emit(c,16,c->snd_nxt,NULL,0);
  flush(c);
}
void tcp_poll(void) {
  if(polling)return;
  polling=true;uint32_t now=clock_ticks();
  for(unsigned i=0;i<SOCKETS;i++) {
    struct connection *c=&connections[i];if(!c->used)continue;
    if(c->state==LISTEN) {pending_clean(c);continue;}
    if(c->state==SYN_SENT || c->state==SYN_RECEIVED) {
      if(due(now,c->deadline)) {
        if(c->retries++>=5) {c->error=110;c->reset=true;c->state=CLOSED;if(c->detached)c->used=false;continue;}
        emit(c,c->state==SYN_SENT?2:0x12,c->snd_una,NULL,0);
        c->deadline=now+(100u<<(c->retries>4?4:c->retries));
      }
    } else if(c->state==TIME_WAIT || c->state==FIN_WAIT2) {
      if(due(now,c->deadline)) {c->state=CLOSED;if(c->detached)c->used=false;}
    } else if(c->state==FIN_WAIT1 || c->state==LAST_ACK) {
      if(due(now,c->deadline)) {
        if(c->retries++>=5) {c->state=CLOSED;if(c->detached)c->used=false;continue;}
        emit(c,0x11,c->fin_sent_sequence,NULL,0);c->deadline=now+(100u<<c->retries);
      }
    } else if(c->state==ESTABLISHED || c->state==CLOSE_WAIT) {
      if(c->tx_sent && due(now,c->deadline)) {
        if(c->retries++>=7) {c->error=110;c->reset=true;c->state=CLOSED;if(c->detached)c->used=false;continue;}
        c->ssthresh=c->cwnd/2;if(c->ssthresh<2*c->mss)c->ssthresh=2*c->mss;c->cwnd=c->mss;
        c->rto=c->rto<1600?c->rto*2:3200;c->sack_count=0;c->recovering=false;c->duplicate_acks=0;retransmit(c);c->deadline=now+c->rto;
      }
      if(!c->peer_window && c->tx_count && due(now,c->probe_at)) {emit(c,16,c->snd_una-1,NULL,0);c->probe_at=now+100;}
      flush(c);
    }
  }
  polling=false;
}
int net_tcp_socket_connect(int id,uint32_t destination,uint16_t port,bool nonblock) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->state!=RESERVED || !local_address() || !destination || !port)return -22;
  if(c->local_ip && c->local_ip!=local_address())return -99;
  c->local_ip=local_address();if(!c->local_port) {int r=choose_port(c,id);if(r)return r;}
  if(!net_resolve(destination,c->hardware))return -113;
  c->remote=destination;c->remote_port=port;c->snd_una=initial_sequence();c->snd_nxt=c->snd_una+1;
  c->state=SYN_SENT;c->deadline=clock_ticks()+100;emit(c,2,c->snd_una,NULL,0);
  if(nonblock)return -115;
  uint32_t started=clock_ticks();
  while(c->state==SYN_SENT && (uint32_t)(clock_ticks()-started)<1500) {net_poll();cpu_pause();}
  if(c->state==ESTABLISHED)return 0;
  if(c->reset)return -(c->error?c->error:111);
  c->error=110;c->reset=true;c->state=CLOSED;return -110;
}
int net_tcp_socket_send(int id,const void *data,uint32_t size,bool nonblock) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->reset || (c->state!=ESTABLISHED && c->state!=CLOSE_WAIT) || c->closing)return -32;
  if(!size)return 0;
  uint32_t started=clock_ticks();
  while(c->tx_count==TX_SIZE && !nonblock && !c->reset && (uint32_t)(clock_ticks()-started)<500) {net_poll();cpu_pause();}
  if(c->reset)return -(c->error?c->error:104);
  unsigned count=TX_SIZE-c->tx_count;if(!count)return -11;if(count>size)count=size;
  for(unsigned i=0;i<count;i++)c->tx[(c->tx_head+c->tx_count+i)%TX_SIZE]=((const uint8_t *)data)[i];
  c->tx_count+=count;flush(c);return count;
}
int net_tcp_socket_recv(int id,void *data,uint32_t size,uint32_t timeout) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->read_shutdown)return 0;
  if(!size)return 0;
  uint32_t start=clock_ticks();
  while(!c->rx_count && !c->peer_closed && !c->reset && (uint32_t)(clock_ticks()-start)<timeout) {net_poll();cpu_pause();}
  if(c->reset)return -(c->error?c->error:104);
  if(!c->rx_count)return c->peer_closed?0:-11;
  unsigned count=c->rx_count;if(count>size)count=size;
  for(unsigned i=0;i<count;i++)((uint8_t *)data)[i]=c->rx[(c->rx_head+i)%RX_SIZE];
  c->rx_head=(c->rx_head+count)%RX_SIZE;c->rx_count-=count;emit(c,16,c->snd_nxt,NULL,0);return count;
}
int net_tcp_shutdown(int id,unsigned how) {
  struct connection *c=get(id);if(!c)return -9;
  if(how>2)return -22;
  if(c->state==RESERVED || c->state==LISTEN || c->state==CLOSED || c->state==SYN_SENT || c->state==SYN_RECEIVED)return -107;
  if(how==0 || how==2) {c->read_shutdown=true;c->rx_head=(c->rx_head+c->rx_count)%RX_SIZE;c->rx_count=0;}
  if(how==1 || how==2) {c->closing=true;flush(c);}
  return 0;
}
int net_tcp_socket_close(int id) {
  struct connection *c=get(id);if(!c)return -9;
  if(c->state==LISTEN) {
    for(unsigned i=0;i<c->pending_count;i++) {struct connection *child=get(c->pending[i]);if(child) {emit(child,0x14,child->snd_nxt,NULL,0);child->used=false;}}
    c->used=false;return 0;
  }
  c->detached=true;
  if(c->state==ESTABLISHED || c->state==CLOSE_WAIT) {c->closing=true;flush(c);}
  else if(c->state==RESERVED || c->state==CLOSED || c->state==SYN_SENT) {
    if(c->state==SYN_SENT)emit(c,4,c->snd_nxt,NULL,0);
    c->used=false;
  }
  return 0;
}
void net_tcp_socket_snapshot(int id,struct tcp_snapshot *out) {
  memset(out,0,sizeof(*out));struct connection *c=get(id);if(!c)return;
  *out=(struct tcp_snapshot){c->state!=CLOSED,c->state==ESTABLISHED || c->state==CLOSE_WAIT,c->peer_closed || c->read_shutdown,c->reset,c->rx_count,c->remote,c->local_port,c->remote_port};
}
void tcp_reset_all(void) {
  for(unsigned i=0;i<SOCKETS;i++)if(connections[i].used && !((connections[i].state==LISTEN || connections[i].state==RESERVED) && !connections[i].local_ip)) {
    connections[i].error=100;connections[i].reset=true;connections[i].state=CLOSED;
    if(connections[i].detached)connections[i].used=false;
  }
}
void net_tcp_snapshot(struct tcp_snapshot *out) {net_tcp_socket_snapshot(0,out);}
int net_tcp_connect(uint32_t destination,uint16_t port) {
  if(connections[0].used && connections[0].state!=CLOSED)return -1;
  initialize(&connections[0]);int result=net_tcp_socket_connect(0,destination,port,false);
  if(result) {connections[0].used=false;return result==-110?-4:-3;}return 0;
}
int net_tcp_send(const void *data,uint32_t size) {
  unsigned done=0;while(done<size) {
    int n=net_tcp_socket_send(0,(const uint8_t *)data+done,size-done,false);if(n<0)return -2;done+=n;
    net_poll();
  }
  uint32_t started=clock_ticks();
  while(connections[0].tx_count && !connections[0].reset && (uint32_t)(clock_ticks()-started)<1500) {net_poll();cpu_pause();}
  return connections[0].tx_count || connections[0].reset?-3:(int)done;
}
int net_tcp_recv(void *data,uint32_t size,uint32_t timeout) {
  int n=net_tcp_socket_recv(0,data,size,timeout);return n==-11?-2:n<0?-3:n;
}
int net_tcp_close(void) {
  if(!connections[0].used)return 0;
  net_tcp_socket_close(0);uint32_t started=clock_ticks();
  while(connections[0].used && connections[0].state!=TIME_WAIT && (uint32_t)(clock_ticks()-started)<500) {net_poll();cpu_pause();}
  connections[0].used=false;return 0;
}

void tcp_diagnostics(void) {
  for(unsigned i=0;i<SOCKETS;i++) {
    struct connection *c=get(i);if(!c)continue;
    console_write("TCP ");print_uint(i);console_write(" state ");print_uint(c->state);
    console_write(" local ");print_uint(c->local_port);console_write(" peer ");ip_print(c->remote);console_putc(':');print_uint(c->remote_port);
    console_write(" RX ");print_uint(c->rx_count);console_write(" TX ");print_uint(c->tx_count);console_write(" flight ");print_uint(c->tx_sent);
    console_write(" cwnd ");print_uint(c->cwnd);console_write(" peer-window ");print_uint(c->peer_window);
    console_write(c->sack?" SACK yes":" SACK no");console_write(" window-scale ");print_uint(c->scale?c->peer_scale:0);
    console_write(" backlog ");print_uint(c->pending_count);console_putc('\n');
  }
}
