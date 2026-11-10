#include "system.h"
#include "firewall.h"
#define RULES 64
#define FLOW_SETS 256
#define FLOW_WAYS 4
struct rule {
  uint32_t source, source_mask, destination, destination_mask;
  uint16_t sport_low, sport_high, dport_low, dport_high;
  uint8_t direction, protocol, allow, reserved;
};
static struct policy {
  uint32_t magic, version, generation, enabled, inbound, outbound, count;
  struct rule rules[RULES];
  uint32_t checksum;
} policy = {.magic=0x41564657,.version=1,.outbound=1};
static struct flow {
  uint32_t source, destination, touched, syn_next, remote_next;
  uint16_t sport, dport;
  uint8_t protocol, state;
  bool used;
} flows[FLOW_SETS][FLOW_WAYS];
static uint64_t accepted[2], denied[2], rule_hits[RULES];
struct packet {
  uint32_t source, destination, sequence, acknowledgement;
  uint16_t sport, dport;
  uint8_t protocol, flags;
};
static uint16_t u16(const uint8_t *p) { return (uint16_t)p[0]<<8|p[1]; }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint16_t sum(const uint8_t *p,unsigned n,uint32_t value) {
  for(;n>1;n-=2,p+=2)value+=u16(p);
  if(n)value+=(unsigned)*p<<8;
  while(value>>16)value=(value&65535)+(value>>16);
  return (uint16_t)~value;
}
static bool decode(const uint8_t *h,unsigned n,struct packet *p) {
  if(n<20 || h[0]>>4!=4)return false;
  unsigned ihl=(h[0]&15)*4,total=u16(h+2);
  if(ihl<20 || ihl>n || total<ihl || total>n || (u16(h+6)&0x3fff) || sum(h,ihl,0))return false;
  memset(p,0,sizeof(*p));p->source=u32(h+12);p->destination=u32(h+16);p->protocol=h[9];
  const uint8_t *t=h+ihl;unsigned length=total-ihl;
  if(p->protocol==6 || p->protocol==17) {
    if(length<(p->protocol==6?20u:8u))return false;
    if(p->protocol==6 && ((t[12]>>4)*4u<20 || (t[12]>>4)*4u>length))return false;
    if(p->protocol==17) {
      unsigned udp_length=u16(t+4);
      if(udp_length<8 || udp_length>length)return false;
      length=udp_length;
    }
    uint32_t pseudo=(p->source>>16)+(p->source&65535)+(p->destination>>16)+(p->destination&65535)+p->protocol+length;
    if((p->protocol==6 || u16(t+6)) && sum(t,length,pseudo))return false;
    p->sport=u16(t);p->dport=u16(t+2);
    if(p->protocol==6) {p->flags=t[13];p->sequence=u32(t+4);p->acknowledgement=u32(t+8);}
  } else if(p->protocol==1) {
    if(length<8 || sum(t,length,0))return false;
    p->flags=t[0];
    if(t[0]==8 || t[0]==0) {p->sport=u16(t+4);p->dport=u16(t+6);}
  }
  return true;
}
static unsigned flow_set(const struct packet *p) {
  uint32_t hash=p->source^p->destination^((uint32_t)p->sport+p->dport)^p->protocol;
  hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
  return hash&(FLOW_SETS-1);
}
static bool expired(const struct flow *f,uint32_t now) {
  unsigned timeout=f->protocol==6?(f->state==3?30000:3000):3000;
  return !f->used || (uint32_t)(now-f->touched)>=timeout;
}
static struct flow *find(const struct packet *p,uint32_t now,bool *reverse) {
  struct flow *set=flows[flow_set(p)];
  for(unsigned i=0;i<FLOW_WAYS;i++) {
    struct flow *f=&set[i];
    if(expired(f,now) || f->protocol!=p->protocol)continue;
    if(f->source==p->source && f->destination==p->destination && f->sport==p->sport && f->dport==p->dport) {*reverse=false;return f;}
    if(f->source==p->destination && f->destination==p->source &&
       ((p->protocol==1 && f->sport==p->sport && f->dport==p->dport) ||
        (f->sport==p->dport && f->dport==p->sport))) {*reverse=true;return f;}
  }
  return NULL;
}
static bool established(const struct flow *f,const struct packet *p,bool reverse) {
  if(!f)return false;
  if(p->protocol==17)return true;
  if(p->protocol==1)return reverse && p->flags==0;
  if(p->protocol!=6 || (p->flags&3)==3)return false;
  if(f->state==1)return reverse && (p->flags&0x17)==0x12 && p->acknowledgement==f->syn_next;
  if(f->state==2)return (!reverse && (p->flags&0x17)==0x10 && p->acknowledgement==f->remote_next) ||
    (reverse && (p->flags&0x17)==0x12 && p->acknowledgement==f->syn_next);
  return (f->state==3 || f->state==4) && !(p->flags&2) && (p->flags&0x15);
}
static void track(const struct packet *p,struct flow *f,bool reverse,uint32_t now) {
  if(!f) {
    if(p->protocol!=17 && !(p->protocol==1 && p->flags==8) &&
       !(p->protocol==6 && (p->flags&0x17)==2))return;
    struct flow *set=flows[flow_set(p)];f=&set[0];
    for(unsigned i=0;i<FLOW_WAYS;i++) {
      if(expired(&set[i],now)) {f=&set[i];break;}
      if((uint32_t)(now-set[i].touched)>(uint32_t)(now-f->touched))f=&set[i];
    }
    *f=(struct flow){.source=p->source,.destination=p->destination,.sport=p->sport,.dport=p->dport,
      .protocol=p->protocol,.state=1,.syn_next=p->sequence+1,.used=true};
  } else if(p->protocol==6) {
    if(f->state==1 && reverse && established(f,p,true)) {f->state=2;f->remote_next=p->sequence+1;}
    else if(f->state==2 && !reverse && established(f,p,false))f->state=3;
    if(p->flags&5)f->state=4;
  }
  f->touched=now;
}
bool firewall_packet(bool outbound,const uint8_t *header,unsigned length) {
  if(!policy.enabled)return true;
  struct packet p;unsigned direction=outbound?1:0;
  if(!decode(header,length,&p)) {denied[direction]++;return false;}
  bool reverse=false;uint32_t now=clock_ticks();struct flow *f=find(&p,now,&reverse);
  bool allow=outbound?policy.outbound:policy.inbound,matched=false;
  for(unsigned i=0;i<policy.count;i++) {
    const struct rule *r=&policy.rules[i];
    if(r->direction!=direction || (r->protocol && r->protocol!=p.protocol) ||
       (p.source&r->source_mask)!=r->source || (p.destination&r->destination_mask)!=r->destination ||
       p.sport<r->sport_low || p.sport>r->sport_high || p.dport<r->dport_low || p.dport>r->dport_high)continue;
    rule_hits[i]++;allow=r->allow;matched=true;break;
  }
  if(!matched && established(f,&p,reverse))allow=true;
  if(!matched && p.protocol==17 && ((outbound && p.sport==68 && p.dport==67) ||
      (!outbound && p.sport==67 && p.dport==68)))allow=true;
  if(allow) {accepted[direction]++;track(&p,f,reverse,now);} else denied[direction]++;
  return allow;
}
static bool valid(const struct policy *p) {
  if(p->magic!=policy.magic || p->version!=1 || p->enabled>1 || p->inbound>1 || p->outbound>1 || p->count>RULES ||
     p->checksum!=crc32(p,offsetof(struct policy,checksum)))return false;
  for(unsigned i=0;i<p->count;i++) {
    const struct rule *r=&p->rules[i];uint32_t a=~r->source_mask,b=~r->destination_mask;
    if(r->direction>1 || r->allow>1 || (r->protocol!=0 && r->protocol!=1 && r->protocol!=6 && r->protocol!=17) ||
       r->sport_low>r->sport_high || r->dport_low>r->dport_high || (a&(a+1)) || (b&(b+1)) ||
       (r->source&r->source_mask)!=r->source || (r->destination&r->destination_mask)!=r->destination)return false;
  }
  return true;
}
void firewall_init(void) {
  struct policy saved, newest;
  bool found=false;
  for(unsigned i=0;i<2;i++)
    if(fs_read(i?"firewall-b":"firewall-a",&saved,sizeof(saved))==sizeof(saved) && valid(&saved) &&
       (!found || (int32_t)(saved.generation-newest.generation)>0)) {newest=saved;found=true;}
  if(found)policy=newest;
  memset(flows,0,sizeof(flows));
  memset(rule_hits,0,sizeof(rule_hits));
}
static char *word(char **text) {
  while(**text==' ')(*text)++;
  char *start=*text;while(**text && **text!=' ')(*text)++;
  if(**text)*(*text)++=0;
  return start;
}
static bool integer(const char *s,unsigned max,unsigned *out) {
  bool ok=false;uint32_t n=parse_uint(s,&ok);
  if(!ok || n>max)return false;
  *out=n;return true;
}
static bool network(char *text,uint32_t *address,uint32_t *mask) {
  if(str_eq(text,"any")) {*address=*mask=0;return true;}
  char *slash=text;while(*slash && *slash!='/')slash++;
  unsigned bits=32;
  if(*slash) {*slash++=0;if(!integer(slash,32,&bits))return false;}
  if(!ip_parse(text,address))return false;
  *mask=bits?0xffffffffu<<(32-bits):0;*address&=*mask;return true;
}
static bool ports(char *text,uint16_t *low,uint16_t *high) {
  if(str_eq(text,"any")) {*low=0;*high=65535;return true;}
  char *dash=text;while(*dash && *dash!='-')dash++;
  bool range=*dash;if(range)*dash++=0;
  unsigned a,b;if(!integer(text,65535,&a))return false;
  b=a;if(range && !integer(dash,65535,&b))return false;
  if(a>b)return false;
  *low=a;*high=b;return true;
}
static void count(uint64_t n) {
  char digits[24];unsigned used=0;do {digits[used++]='0'+n%10;n/=10;}while(n);
  while(used)console_putc(digits[--used]);
}
static void address(uint32_t ip) {
  for(unsigned i=0;i<4;i++) {if(i)console_putc('.');count((ip>>(24-i*8))&255);}
}
static void cidr(uint32_t ip,uint32_t mask) {
  address(ip);console_putc('/');unsigned bits=0;while(mask&0x80000000u){bits++;mask<<=1;}count(bits);
}
void firewall_command(char *text) {
  char *cmd=word(&text);
  if(str_eq(cmd,"on") || str_eq(cmd,"off")) {
    policy.enabled=str_eq(cmd,"on");memset(flows,0,sizeof(flows));
  } else if(str_eq(cmd,"save")) {
    struct policy saved=policy;saved.generation++;saved.checksum=crc32(&saved,offsetof(struct policy,checksum));
    int disk=fs_mounted_disk();
    if(disk<0 || fs_write(saved.generation&1?"firewall-b":"firewall-a",&saved,sizeof(saved))<0 || !disk_flush(disk)) {
      console_write("Firewall policy write failed\n");return;
    }
    policy=saved;
  } else if(str_eq(cmd,"load"))firewall_init();
  else if(str_eq(cmd,"flush")) {policy.count=0;memset(flows,0,sizeof(flows));memset(rule_hits,0,sizeof(rule_hits));}
  else if(str_eq(cmd,"default")) {
    char *direction=word(&text),*action=word(&text);
    if((!str_eq(direction,"in") && !str_eq(direction,"out")) || (!str_eq(action,"allow") && !str_eq(action,"drop")))goto invalid;
    if(str_eq(direction,"in"))policy.inbound=str_eq(action,"allow");else policy.outbound=str_eq(action,"allow");
  } else if(str_eq(cmd,"delete")) {
    unsigned index;if(!integer(word(&text),RULES,&index) || index>=policy.count)goto invalid;
    for(unsigned i=index+1;i<policy.count;i++) {policy.rules[i-1]=policy.rules[i];rule_hits[i-1]=rule_hits[i];}
    policy.count--;memset(flows,0,sizeof(flows));
  } else if(str_eq(cmd,"add")) {
    if(policy.count==RULES)goto invalid;
    struct rule r={0};char *direction=word(&text),*action=word(&text),*protocol=word(&text);
    if((!str_eq(direction,"in") && !str_eq(direction,"out")) || (!str_eq(action,"allow") && !str_eq(action,"drop")))goto invalid;
    r.direction=str_eq(direction,"out");r.allow=str_eq(action,"allow");
    if(str_eq(protocol,"tcp"))r.protocol=6;else if(str_eq(protocol,"udp"))r.protocol=17;
    else if(str_eq(protocol,"icmp"))r.protocol=1;else if(!str_eq(protocol,"any"))goto invalid;
    if(!network(word(&text),&r.source,&r.source_mask) || !network(word(&text),&r.destination,&r.destination_mask) ||
       !ports(word(&text),&r.sport_low,&r.sport_high) || !ports(word(&text),&r.dport_low,&r.dport_high) || *word(&text))goto invalid;
    policy.rules[policy.count]=r;rule_hits[policy.count++]=0;
  } else if(str_eq(cmd,"connections")) {
    uint32_t now=clock_ticks();
    for(unsigned set=0;set<FLOW_SETS;set++)for(unsigned way=0;way<FLOW_WAYS;way++) {
      const struct flow *f=&flows[set][way];if(expired(f,now))continue;
      console_write(f->protocol==6?"tcp ":f->protocol==17?"udp ":"icmp ");
      address(f->source);console_putc(':');count(f->sport);console_write(" -> ");
      address(f->destination);console_putc(':');count(f->dport);
      console_write(" state=");count(f->state);console_write(" idle_ticks=");count(now-f->touched);console_putc('\n');
    }
    return;
  } else if(str_eq(cmd,"status") || str_eq(cmd,"list") || !*cmd) {
    console_write(policy.enabled?"Firewall on\n":"Firewall off\n");
    console_write(policy.inbound?"Default in: allow\n":"Default in: drop\n");
    console_write(policy.outbound?"Default out: allow\n":"Default out: drop\n");
    for(unsigned d=0;d<2;d++) {console_write(d?"out accepted=":"in accepted=");count(accepted[d]);console_write(" dropped=");count(denied[d]);console_putc('\n');}
    for(unsigned i=0;i<policy.count;i++) {
      const struct rule *r=&policy.rules[i];count(i);console_write(r->direction?" out ":" in ");console_write(r->allow?"allow ":"drop ");
      console_write(r->protocol==6?"tcp ":r->protocol==17?"udp ":r->protocol==1?"icmp ":"any ");
      cidr(r->source,r->source_mask);console_putc(' ');cidr(r->destination,r->destination_mask);console_putc(' ');
      count(r->sport_low);console_putc('-');count(r->sport_high);console_putc(' ');count(r->dport_low);console_putc('-');count(r->dport_high);
      console_write(" hits=");count(rule_hits[i]);console_putc('\n');
    }
    return;
  } else goto invalid;
  console_write("Firewall updated\n");return;
invalid:
  console_write("firewall on|off|status|list|connections|save|load|flush\n"
    "firewall default in|out allow|drop\n"
    "firewall add in|out allow|drop tcp|udp|icmp|any SRC/CIDR DST/CIDR SPORT[-END] DPORT[-END]\n"
    "Use any for an address or port range. First matching rule wins.\nfirewall delete INDEX\n");
}
