#include "system.h"
#include "net_service.h"
#include "sandbox.h"
#define ROUTES 64
struct route {uint32_t network, mask, gateway, metric;};
static struct route manual[ROUTES];
static unsigned route_count;
static uint32_t connected_ip,connected_mask,default_gateway;
static unsigned prefix(uint32_t mask) {unsigned n=0;while(mask&0x80000000u){n++;mask<<=1;}return n;}
void route_config(uint32_t address,uint32_t mask,uint32_t gateway) {
  connected_ip=address;connected_mask=mask;default_gateway=gateway;
}
bool route_lookup(uint32_t destination,uint32_t *next) {
  bool found=false;unsigned bits=0;uint32_t metric=0xffffffffu;
  if(connected_ip && connected_mask && (destination&connected_mask)==(connected_ip&connected_mask)) {
    *next=destination;found=true;bits=prefix(connected_mask);metric=0;
  }
  if(!found && default_gateway) {*next=default_gateway;found=true;bits=0;metric=1000;}
  for(unsigned i=0;i<route_count;i++) {
    const struct route *r=&manual[i];if((destination&r->mask)!=r->network)continue;
    if(r->gateway && (!connected_ip || (r->gateway&connected_mask)!=(connected_ip&connected_mask)))continue;
    unsigned n=prefix(r->mask);
    if(!found || n>bits || (n==bits && r->metric<metric)) {
      *next=r->gateway?r->gateway:destination;found=true;bits=n;metric=r->metric;
    }
  }
  return found;
}
static struct saved_routes {
  uint32_t magic,version,generation,count;
  struct route entries[ROUTES];uint32_t checksum;
} route_store={.magic=0x41565254,.version=1};
static bool valid_store(const struct saved_routes *p) {
  if(p->magic!=route_store.magic || p->version!=1 || p->count>ROUTES || p->checksum!=crc32(p,offsetof(struct saved_routes,checksum)))return false;
  for(unsigned i=0;i<p->count;i++) {uint32_t inverse=~p->entries[i].mask;
    if((inverse&(inverse+1)) || (p->entries[i].network&p->entries[i].mask)!=p->entries[i].network)return false;
  }
  return true;
}
void route_init(void) {
  struct saved_routes saved,newest;bool found=false;
  for(unsigned i=0;i<2;i++)if(fs_read(i?"routes-b":"routes-a",&saved,sizeof(saved))==sizeof(saved) && valid_store(&saved) &&
    (!found || (int32_t)(saved.generation-newest.generation)>0)) {newest=saved;found=true;}
  if(found) {route_store=newest;route_count=newest.count;memcpy(manual,newest.entries,sizeof(manual));}
}
static int route_save(void) {
  struct saved_routes next=route_store;next.generation++;next.count=route_count;memcpy(next.entries,manual,sizeof(manual));
  next.checksum=crc32(&next,offsetof(struct saved_routes,checksum));int disk=fs_mounted_disk();
  if(disk<0 || fs_write(next.generation&1?"routes-b":"routes-a",&next,sizeof(next))<0 || !disk_flush(disk))return -5;
  route_store=next;return 0;
}
static char *word(char **p) {while(**p==' ')(*p)++;char *s=*p;while(**p&&**p!=' ')(*p)++;if(**p)*(*p)++=0;return s;}
static bool network(char *s,uint32_t *ip,uint32_t *mask) {
  char *slash=s;while(*slash && *slash!='/')slash++;
  if(!*slash)return false;
  *slash++=0;
  bool ok;unsigned bits=parse_uint(slash,&ok);if(!ok || bits>32 || !ip_parse(s,ip))return false;
  *mask=bits?0xffffffffu<<(32-bits):0;*ip&=*mask;return true;
}
void route_command(char *line) {
  if(!sandbox_has(process_current(),AV_CAP_ADMIN)) {console_write("Permission denied\n");return;}
  char *cmd=word(&line);
  if(!*cmd || str_eq(cmd,"list")) {
    if(connected_ip) {ip_print(connected_ip&connected_mask);console_putc('/');print_uint(prefix(connected_mask));console_write(" connected\n");}
    if(default_gateway) {console_write("0.0.0.0/0 via ");ip_print(default_gateway);console_write(" metric 1000 DHCP/static\n");}
    for(unsigned i=0;i<route_count;i++) {print_uint(i);console_putc(' ');ip_print(manual[i].network);console_putc('/');print_uint(prefix(manual[i].mask));console_write(" via ");ip_print(manual[i].gateway);console_write(" metric ");print_uint(manual[i].metric);console_putc('\n');}
    return;
  }
  if(str_eq(cmd,"save")) {console_write(route_save()?"Route write failed\n":"Routes saved\n");return;}
  if(str_eq(cmd,"load")) {route_init();return;}
  if(str_eq(cmd,"add")) {
    struct route r;bool ok;
    if(route_count==ROUTES || !network(word(&line),&r.network,&r.mask) || !ip_parse(word(&line),&r.gateway))goto invalid;
    r.metric=parse_uint(word(&line),&ok);if(!ok || *word(&line))goto invalid;
    if(r.gateway && (!connected_ip || (r.gateway&connected_mask)!=(connected_ip&connected_mask)))goto invalid;
    for(unsigned i=0;i<route_count;i++)if(manual[i].network==r.network && manual[i].mask==r.mask && manual[i].metric==r.metric) {manual[i]=r;return;}
    manual[route_count++]=r;
  } else if(str_eq(cmd,"delete")) {
    bool ok;unsigned n=parse_uint(word(&line),&ok);if(!ok || n>=route_count)goto invalid;
    for(unsigned i=n+1;i<route_count;i++)manual[i-1]=manual[i];
    route_count--;
  } else if(str_eq(cmd,"flush"))route_count=0;
  else if(str_eq(cmd,"get")) {
    uint32_t ip,next;if(!ip_parse(word(&line),&ip))goto invalid;
    if(route_lookup(ip,&next)) {console_write("Next hop: ");ip_print(next);console_putc('\n');}
    else console_write("No route to destination\n");
  } else goto invalid;
  return;
invalid:console_write("route list|save|load|add NETWORK/PREFIX GATEWAY METRIC|delete INDEX|flush|get ADDRESS\nGateway 0.0.0.0 selects an on-link route.\n");
}
