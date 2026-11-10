#include "system.h"
#include "net_service.h"
#include "sandbox.h"
#define PROFILES 16
struct profile {char name[32];uint32_t automatic,ip,mask,gateway,dns,secondary;};
static struct configuration {
  uint32_t magic,version,generation,count,active;
  struct profile profiles[PROFILES];uint32_t checksum;
} config={.magic=0x41564e50,.version=1,.count=1,.profiles={{.name="Automatic",.automatic=1}}};
static bool valid(const struct configuration *p) {
  if(p->magic!=config.magic || p->version!=1 || !p->count || p->count>PROFILES || p->active>=p->count || p->checksum!=crc32(p,offsetof(struct configuration,checksum)))return false;
  for(unsigned i=0;i<p->count;i++) {
    const struct profile *s=&p->profiles[i];uint32_t inverse=~s->mask;
    if(!s->name[0] || !__builtin_memchr(s->name,0,sizeof(s->name)) || s->automatic>1 ||
       (!s->automatic && (!s->ip || !s->mask || (inverse&(inverse+1)) || (s->gateway && (s->gateway&s->mask)!=(s->ip&s->mask)))))return false;
    for(unsigned j=0;j<i;j++)if(str_eq(s->name,p->profiles[j].name))return false;
  }
  return true;
}
static void activate(void) {
  const struct profile *p=&config.profiles[config.active];
  if(p->automatic)net_dhcp_begin();
  else {net_config(p->ip,p->mask,p->gateway,p->dns);net_dns_servers(p->dns,p->secondary);}
}
void net_service_init(void) {
  struct configuration saved,newest;bool found=false;
  for(unsigned i=0;i<2;i++)
    if(fs_read(i?"netprofiles-b":"netprofiles-a",&saved,sizeof(saved))==sizeof(saved) && valid(&saved) &&
      (!found || (int32_t)(saved.generation-newest.generation)>0)) {newest=saved;found=true;}
  if(found)config=newest;
  activate();
}
static char *word(char **p) {while(**p==' ')(*p)++;char *s=*p;while(**p && **p!=' ')(*p)++;if(**p)*(*p)++=0;return s;}
void net_service_command(char *line) {
  if(!sandbox_has(process_current(),AV_CAP_ADMIN)) {console_write("Permission denied\n");return;}
  char *cmd=word(&line);
  if(!*cmd || str_eq(cmd,"list")) {
    for(unsigned i=0;i<config.count;i++) {console_write(i==config.active?"* ":"  ");console_write(config.profiles[i].name);console_write(config.profiles[i].automatic?" DHCP\n":" static\n");}
    return;
  }
  if(str_eq(cmd,"save")) {
    struct configuration next=config;next.generation++;next.checksum=crc32(&next,offsetof(struct configuration,checksum));
    int disk=fs_mounted_disk();
    if(disk<0 || fs_write(next.generation&1?"netprofiles-b":"netprofiles-a",&next,sizeof(next))<0 || !disk_flush(disk)) {console_write("Connection profile write failed\n");return;}
    config=next;console_write("Connection profiles saved\n");return;
  }
  if(str_eq(cmd,"load")) {net_service_init();return;}
  char *name=word(&line);unsigned index;
  for(index=0;index<config.count;index++)if(str_eq(name,config.profiles[index].name))break;
  if(str_eq(cmd,"activate")) {if(index==config.count)goto invalid;config.active=index;activate();return;}
  if(str_eq(cmd,"delete")) {
    if(index==config.count || config.count==1)goto invalid;
    for(unsigned i=index+1;i<config.count;i++)config.profiles[i-1]=config.profiles[i];
    config.count--;if(config.active>index)config.active--;
    else if(config.active==index) {config.active=0;activate();}
    return;
  }
  if(str_eq(cmd,"set")) {
    if(!*name || strlen(name)>=32 || index==PROFILES)goto invalid;
    struct profile p={0};memcpy(p.name,name,strlen(name)+1);
    char *mode=word(&line);
    if(str_eq(mode,"dhcp")) {p.automatic=1;if(*word(&line))goto invalid;}
    else if(str_eq(mode,"static")) {
      if(!ip_parse(word(&line),&p.ip) || !ip_parse(word(&line),&p.mask) || !ip_parse(word(&line),&p.gateway) || !ip_parse(word(&line),&p.dns))goto invalid;
      char *secondary=word(&line);if(*secondary && !ip_parse(secondary,&p.secondary))goto invalid;
      uint32_t inverse=~p.mask;
      if(!p.ip || !p.mask || (inverse&(inverse+1)) || (p.gateway && (p.gateway&p.mask)!=(p.ip&p.mask)))goto invalid;
    } else goto invalid;
    config.profiles[index]=p;if(index==config.count)config.count++;
    if(index==config.active)activate();
    return;
  }
invalid:console_write("netprofile list|save|load|set NAME dhcp|set NAME static IP MASK GATEWAY DNS [SECONDARY_DNS]|activate NAME|delete NAME\n");
}
