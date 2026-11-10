#include "abi.h"
#include "sandbox.h"
#include "process.h"
#include "package.h"
#define PROFILE_MAX 64
struct profile { uint8_t hash[32]; uint32_t capabilities; };
static struct policy {
  uint32_t magic, version, generation, defaults, count;
  struct profile profiles[PROFILE_MAX];
  uint32_t checksum;
} policy={.magic=0x41565342,.version=1,.defaults=AV_CAP_ALL};
bool sandbox_has(const struct process *p,uint32_t need) {
  return !p || (p->capabilities&need)==need;
}
static bool valid(const struct policy *p) {
  if(p->magic!=policy.magic || p->version!=1 || p->count>PROFILE_MAX ||
     (p->defaults&~AV_CAP_ALL) || p->checksum!=crc32(p,offsetof(struct policy,checksum)))return false;
  for(unsigned i=0;i<p->count;i++) {
    if(p->profiles[i].capabilities&~AV_CAP_ALL)return false;
    for(unsigned j=0;j<i;j++)if(!__builtin_memcmp(p->profiles[i].hash,p->profiles[j].hash,32))return false;
  }
  return true;
}
void sandbox_init(void) {
  struct policy saved,newest;bool found=false;
  bool present=fs_size("sandbox-a")>=0 || fs_size("sandbox-b")>=0;
  for(unsigned i=0;i<2;i++)
    if(fs_read(i?"sandbox-b":"sandbox-a",&saved,sizeof(saved))==sizeof(saved) && valid(&saved) &&
       (!found || (int32_t)(saved.generation-newest.generation)>0)) {newest=saved;found=true;}
  if(found)policy=newest;
  else if(present) { policy.defaults=0;policy.count=0; }
}
static int commit(struct policy *next) {
  if(!sandbox_has(process_current(),AV_CAP_ADMIN))return -13;
  next->generation=policy.generation+1;
  next->checksum=crc32(next,offsetof(struct policy,checksum));
  int disk=fs_mounted_disk();
  if(disk<0)return -30;
  int n=fs_write(next->generation&1?"sandbox-b":"sandbox-a",next,sizeof(*next));
  if(n<0 || !disk_flush(disk))return -5;
  policy=*next;return 0;
}
int sandbox_profile(const uint8_t hash[32],uint32_t capabilities) {
  if(capabilities&~AV_CAP_ALL)return -22;
  struct policy next=policy;unsigned i;
  for(i=0;i<next.count;i++)if(!__builtin_memcmp(hash,next.profiles[i].hash,32))break;
  if(i==PROFILE_MAX)return -28;
  if(i==next.count)next.count++;
  memcpy(next.profiles[i].hash,hash,32);next.profiles[i].capabilities=capabilities;
  return commit(&next);
}
uint32_t sandbox_image(const void *image,unsigned size,uint8_t hash[32]) {
  sha256(image,size,hash);
  for(unsigned i=0;i<policy.count;i++)
    if(!__builtin_memcmp(hash,policy.profiles[i].hash,32))return policy.profiles[i].capabilities;
  return policy.defaults;
}
void sandbox_root(const uint8_t hash[32],char out[80]) {
  const char digits[]="0123456789abcdef";
  memcpy(out,"/appdata/",9);
  for(unsigned i=0;i<32;i++) {out[9+2*i]=digits[hash[i]>>4];out[10+2*i]=digits[hash[i]&15];}
  out[73]=0;
}
bool sandbox_write_allowed(const char *path) {
  const struct process *p=process_current();
  if(!p)return true;
  if(!sandbox_has(p,AV_CAP_WRITE))return false;
  if(sandbox_has(p,AV_CAP_ADMIN))return true;
  char canonical[256];unsigned used=0;
  const char *part=path;
  while(*part) {
    while(*part=='/')part++;
    if(!*part)break;
    const char *start=part;while(*part && *part!='/')part++;
    unsigned n=part-start;
    if((n==1 && start[0]=='.') || (n==2 && start[0]=='.' && start[1]=='.'))return false;
    if(used+n+1>=sizeof(canonical))return false;
    canonical[used++]='/';memcpy(canonical+used,start,n);used+=n;
  }
  canonical[used]=0;
  char root[80];sandbox_root(p->executable_sha256,root);
  return used>74 && !__builtin_memcmp(canonical,root,73) && canonical[73]=='/';
}
bool sandbox_native(struct process *p,unsigned call) {
  uint32_t need=0;
  switch(call) {
  case AV_FILE_READ:case AV_FILE_LIST:case AV_FILE_SIZE:case AV_FILE_ENTRY:case AV_DISK_HEALTH: need=AV_CAP_READ;break;
  case AV_FILE_WRITE:case AV_FILE_REMOVE:case AV_FILE_MKDIR:case AV_FILE_RENAME:need=AV_CAP_WRITE;break;
  case AV_FILE_COPY:need=AV_CAP_READ|AV_CAP_WRITE;break;
  case AV_SOCKET:case AV_UDP_SEND:case AV_UDP_RECV:case AV_TCP_CONNECT:case AV_TCP_SEND:case AV_TCP_RECV:case AV_TCP_CLOSE:case AV_DNS:case AV_PING:need=AV_CAP_NETWORK;break;
  case AV_SPAWN:need=AV_CAP_EXEC;break;
  case AV_TASK_INFO:case AV_KILL:need=AV_CAP_PROCESSES;break;
  case AV_PACKAGE:case AV_POWER:case AV_DHCP:case AV_NET_CONFIG:case AV_FIREWALL:need=AV_CAP_ADMIN;break;
  case AV_GUI_OPEN:case AV_GUI_DRAW:case AV_GUI_EVENT:case AV_GUI_CLOSE:need=AV_CAP_GUI;break;
  case AV_AUDIO_PLAY:need=AV_CAP_AUDIO|AV_CAP_READ;break;
  case AV_AUDIO_STATUS:case AV_AUDIO_STOP:need=AV_CAP_AUDIO;break;
  case AV_CLIPBOARD_READ:need=AV_CAP_CLIPBOARD_READ;break;
  case AV_CLIPBOARD_WRITE:need=AV_CAP_CLIPBOARD_WRITE;break;
  case AV_EXIT:case AV_WRITE:case AV_READLINE:case AV_TICKS:case AV_SLEEP:case AV_ARGS:case AV_YIELD:case AV_WAIT:case AV_GETPID:case AV_STATUS:case AV_REPORT:case AV_CLEAR:case AV_COLOR:case AV_RANDOM:case AV_SANDBOX_INFO:case AV_SANDBOX_LIMIT:break;
  default:return p->capabilities==AV_CAP_ALL;
  }
  return sandbox_has(p,need);
}
bool sandbox_linux(struct process *p,unsigned call,bool x64) {
  if(p->capabilities==AV_CAP_ALL)return true;
  uint32_t need=0;
  if(x64) {
    switch(call) {
    case 4096:case 4097:case 4098:case 4099:need=AV_CAP_GUI;break;
    case 41:case 42:case 43:case 48:case 49:case 50:case 288:case 44:case 45:case 51:case 52:case 54:case 55:case 4100:need=AV_CAP_NETWORK;break;
    case 56:case 57:case 59:need=AV_CAP_EXEC;break;
    case 62:case 234:need=AV_CAP_PROCESSES;break;
    case 4:case 6:case 21:case 79:case 89:case 217:case 262:need=AV_CAP_READ;break;
    case 82:case 83:case 84:case 87:case 267:need=AV_CAP_WRITE;break;
    case 0:case 1:case 2:case 3:case 5:case 8:case 9:case 10:case 11:case 12:case 13:case 14:case 16:case 19:case 20:case 24:case 32:case 33:case 35:case 39:case 60:case 61:case 63:case 72:case 74:case 75:case 96:case 97:case 102:case 104:case 107:case 108:case 110:case 158:case 186:case 200:case 201:case 202:case 218:case 228:case 230:case 231:case 257:case 302:case 318:break;
    default:return false;
    }
  } else {
    switch(call) {
    case 102:need=AV_CAP_NETWORK;break;
    case 2:case 11:case 120:need=AV_CAP_EXEC;break;
    case 37:need=AV_CAP_PROCESSES;break;
    case 106:case 107:case 183:case 195:case 196:case 220:need=AV_CAP_READ;break;
    case 10:case 85:need=AV_CAP_WRITE;break;
    case 1:case 3:case 4:case 5:case 6:case 7:case 19:case 20:case 24:case 41:case 45:case 47:case 49:case 50:case 54:case 55:case 63:case 64:case 90:case 91:case 108:case 122:case 125:case 140:case 158:case 162:case 192:case 197:case 199:case 200:case 201:case 202:case 221:case 224:case 243:case 252:case 258:case 265:case 295:break;
    default:return false;
    }
  }
  return sandbox_has(p,need);
}
static char *word(char **p) {while(**p==' ')(*p)++;char *s=*p;while(**p && **p!=' ')(*p)++;if(**p)*(*p)++=0;return s;}
static bool mask(const char *s,uint32_t *out) {bool ok;*out=parse_uint(s,&ok);return ok && !(*out&~AV_CAP_ALL);}
static bool digest(const char *s,uint8_t hash[32]) {
  if(strlen(s)!=64)return false;
  for(unsigned i=0;i<64;i++) {
    unsigned n=s[i]>='0'&&s[i]<='9'?s[i]-'0':s[i]>='a'&&s[i]<='f'?s[i]-'a'+10:16;
    if(n==16)return false;
    if(!(i&1))hash[i/2]=n<<4;else hash[i/2]|=n;
  }
  return true;
}
void sandbox_command(char *line) {
  if(!sandbox_has(process_current(),AV_CAP_ADMIN)) {console_write("Permission denied\n");return;}
  char *cmd=word(&line);int result=0;uint32_t value;
  if(!*cmd || str_eq(cmd,"list")) {
    console_write("Default capabilities: ");print_uint(policy.defaults);console_putc('\n');
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<policy.count;i++) {
      for(unsigned j=0;j<32;j++){console_putc(hex[policy.profiles[i].hash[j]>>4]);console_putc(hex[policy.profiles[i].hash[j]&15]);}
      console_putc(' ');print_uint(policy.profiles[i].capabilities);console_putc('\n');
    }
    return;
  } else if(str_eq(cmd,"default")) {
    if(!mask(word(&line),&value))goto invalid;
    struct policy next=policy;next.defaults=value;result=commit(&next);
  } else if(str_eq(cmd,"profile")) {
    uint8_t hash[32];if(!digest(word(&line),hash) || !mask(word(&line),&value))goto invalid;
    result=sandbox_profile(hash,value);
  } else if(str_eq(cmd,"delete")) {
    uint8_t hash[32];if(!digest(word(&line),hash))goto invalid;
    struct policy next=policy;unsigned i;
    for(i=0;i<next.count;i++)if(!__builtin_memcmp(hash,next.profiles[i].hash,32))break;
    if(i==next.count)result=-2;
    else {for(unsigned j=i+1;j<next.count;j++)next.profiles[j-1]=next.profiles[j];next.count--;result=commit(&next);}
  } else if(str_eq(cmd,"reduce")) {
    bool ok;unsigned pid=parse_uint(word(&line),&ok);
    if(!ok || !pid || !mask(word(&line),&value))goto invalid;
    result=user_sandbox_limit(pid,value);
  } else if(str_eq(cmd,"ps")) {user_sandbox_report();return;}
  else if(str_eq(cmd,"load"))sandbox_init();
  else if(str_eq(cmd,"run")) {
    if(!mask(word(&line),&value))goto invalid;
    char *name=word(&line);if(!*name)goto invalid;
    result=user_spawn_sandbox(name,line,false,value);
    if(result>=0) {console_write("Started PID ");print_uint(result);console_putc('\n');return;}
  } else goto invalid;
  if(result<0){console_write("Sandbox operation failed: ");print_uint(-result);console_putc('\n');}
  else console_write("Sandbox policy updated\n");
  return;
invalid:
  console_write("sandbox list|ps|load|default MASK|profile SHA256 MASK|delete SHA256|reduce PID MASK|run MASK PROGRAM [ARGS]\n"
    "read=1 write=2 network=4 exec=8 gui=16 audio=32 clipboard-read=64 clipboard-write=128 processes=256 admin=512 exec-memory=1024\n");
}
