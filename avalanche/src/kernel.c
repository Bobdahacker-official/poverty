#include "avalanche.h"
#define PAGES 32768u
#define PAGE 4096u
static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
void *memset(void *d,int c,size_t n){uint8_t *p=d;while(n--)*p++=(uint8_t)c;return d;}
void *memcpy(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;return d;}
void *memmove(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t *b=s;if(a<b)return memcpy(d,s,n);while(n){--n;a[n]=b[n];}return d;}
size_t strlen(const char *s){size_t n=0;while(s[n])++n;return n;}
static bool eq(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void copy(char *d,const char *s,size_t n){if(!n)return;while(--n&&*s)*d++=*s++;*d=0;}
struct tag{uint32_t type,size;};
struct mmap_entry{uint64_t base,len;uint32_t type,reserved;}__attribute__((packed));
struct fbtag{uint32_t type,size;uint64_t addr;uint32_t pitch,width,height;uint8_t bpp,kind;uint16_t reserved;uint8_t rp,rs,gp,gs,bp,bs;}__attribute__((packed));
static uint8_t usable[PAGES/8],allocated[PAGES/8];
static uint32_t available,used;
extern char kernel_end;
static void mark(uint8_t *b,uint32_t i,bool on){if(on)b[i/8]|=1u<<(i%8);else b[i/8]&=~(1u<<(i%8));}
static bool bit(const uint8_t *b,uint32_t i){return b[i/8]&(1u<<(i%8));}
static void reserve(uint32_t start,uint32_t end){uint32_t a=start/PAGE,b=end/PAGE+(end%PAGE!=0);if(b>PAGES)b=PAGES;for(uint32_t i=a;i<b;++i)if(bit(usable,i)){mark(usable,i,false);--available;}}
void *page_alloc(void){for(uint32_t i=0;i<PAGES;++i)if(bit(usable,i)&&!bit(allocated,i)){mark(allocated,i,true);++used;void *p=(void *)(i*PAGE);memset(p,0,PAGE);return p;}return NULL;}
bool page_free(void *p){uint32_t a=(uint32_t)p,i=a/PAGE;if(a%PAGE||i>=PAGES||!bit(allocated,i))return false;mark(allocated,i,false);--used;return true;}
static volatile uint8_t *fb;static uint32_t pitch,fw,fh,fg,bg;static uint8_t bytespp,rpos,gpos,bpos,rbits,gbits,bbits;static uint32_t col,row,cols=80,rows=25;static bool serial;
/* Original 5x7 bitmap alphabet. Lowercase is displayed as uppercase. */
static const uint8_t glyphs[36][7]={
{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
static uint32_t rgb(uint8_t r,uint8_t g,uint8_t b){return ((uint32_t)(r>>(8-rbits))<<rpos)|((uint32_t)(g>>(8-gbits))<<gpos)|((uint32_t)(b>>(8-bbits))<<bpos);}
static void pixel(uint32_t x,uint32_t y,uint32_t c){volatile uint8_t *p=fb+y*pitch+x*bytespp;for(uint8_t i=0;i<bytespp;++i)p[i]=(uint8_t)(c>>(8*i));}
static uint8_t glyph(char c,unsigned y){if(c>='a'&&c<='z')c-=32;if(c>='A'&&c<='Z')return glyphs[c-'A'][y];if(c>='0'&&c<='9')return glyphs[c-'0'+26][y];switch(c){case '>':return y==1?16:y==2?8:y==3?4:y==4?8:y==5?16:0;case '-':return y==3?14:0;case '_':return y==6?31:0;case '.':return y==6?4:0;case ':':return y==2||y==5?4:0;case '/':return y<5?1u<<y:16;case '?':return y==0?14:y==1?17:y==2?2:y==3?4:y==5?4:0;case '=':return y==2||y==4?31:0;case '!':return y<4||y==6?4:0;case '[':return y==0||y==6?14:8;case ']':return y==0||y==6?14:2;case '(':return y==0||y==6?2:4;case ')':return y==0||y==6?8:4;case '+':return y==3?14:y==2||y==4?4:0;case ',':return y==5?4:y==6?8:0;case '\'':return y<2?4:0;default:return 0;}}
static void cell(uint32_t x,uint32_t y,char c){if(!fb)return;for(unsigned py=0;py<16;++py)for(unsigned px=0;px<12;++px){uint8_t bits=py/2<7?glyph(c,py/2):0;pixel(x*12+px,y*16+py,px/2<5&&(bits&(1u<<(4-px/2)))?fg:bg);}}
static void clear_screen(void){if(fb)for(uint32_t y=0;y<fh;++y)for(uint32_t x=0;x<fw;++x)pixel(x,y,bg);col=row=0;}
static void newline(void){col=0;++row;if(row>=rows){if(fb){memmove((void *)fb,(const void *)(fb+16*pitch),(rows-1)*16*pitch);for(uint32_t y=(rows-1)*16;y<rows*16;++y)for(uint32_t x=0;x<fw;++x)pixel(x,y,bg);}row=rows-1;}}
void console_putc(char c){if(serial){for(unsigned i=0;i<100000&&!(inb(0x3fd)&0x20);++i){}if(c=='\n')outb(0x3f8,'\r');outb(0x3f8,(uint8_t)c);}if(c=='\n'){newline();return;}if(c=='\r'){col=0;return;}if(c=='\b'){if(col)--col;else if(row){--row;col=cols-1;}cell(col,row,' ');return;}if(c<' ')return;cell(col,row,c);if(++col>=cols)newline();}
void console_write(const char *s){while(*s)console_putc(*s++);}
static void number(uint32_t n){char b[11];unsigned i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)console_putc(b[--i]);}
struct idt_gate {uint16_t low,selector;uint8_t zero,flags;uint16_t high;}__attribute__((packed));
static struct idt_gate idt[256];
extern void *exception_table[32];
static void idt_init(void){for(unsigned i=0;i<32;++i){uint32_t a=(uint32_t)exception_table[i];idt[i]=(struct idt_gate){a&65535,8,0,0x8e,a>>16};}struct {uint16_t size;uint32_t address;}__attribute__((packed)) pointer={sizeof(idt)-1,(uint32_t)idt};__asm__ volatile("lidt %0"::"m"(pointer));}
static void hex(uint32_t n){console_write("0x");for(int i=7;i>=0;--i)console_putc("0123456789ABCDEF"[(n>>(i*4))&15]);}
void exception_panic(uint32_t *frame){console_write("\nKERNEL EXCEPTION ");number(frame[8]);console_write(" error=");hex(frame[9]);console_write(" eip=");hex(frame[10]);console_write("\nCPU halted to preserve state.\n");for(;;)__asm__ volatile("cli; hlt");}
static const char keys[128]={[2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',[12]='-',[13]='=',[14]='\b',[15]='\t',[16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',[26]='[',[27]=']',[28]='\n',[30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',[39]=';',[40]='\'',[41]='`',[43]='\\',[44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',[51]=',',[52]='.',[53]='/',[57]=' '};
static char input(void){static bool left_shift,right_shift,caps,extended;for(;;){if(serial&&(inb(0x3fd)&1)){char c=inb(0x3f8);if(c=='\r')return '\n';if(c==127)return '\b';return c;}if(!(inb(0x64)&1)){__asm__ volatile("pause");continue;}uint8_t status=inb(0x64),s=inb(0x60);if(status&0x20)continue;if(s==0xe0){extended=true;continue;}if(extended){extended=false;continue;}if(s==42||s==170){left_shift=s==42;continue;}if(s==54||s==182){right_shift=s==54;continue;}if(s==58){caps=!caps;continue;}if(s&128)continue;char c=keys[s];bool shift=left_shift||right_shift;if(c>='a'&&c<='z'){if(shift!=caps)c-=32;}else if(shift){const char *a="1234567890-=[];',./`\\",*b="!@#$%^&*()_+{}:\"<>?~|";for(unsigned i=0;a[i];++i)if(c==a[i]){c=b[i];break;}}if(c)return c;}}
#define FILES 32
struct file{char name[32];void *data;uint32_t len;};static struct file files[FILES];
static struct file *lookup(const char *name){for(unsigned i=0;i<FILES;++i)if(files[i].data&&eq(files[i].name,name))return &files[i];return NULL;}
static bool filename(const char *s){size_t n=strlen(s);if(!n||n>=32)return false;while(*s){char c=*s++;if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;}return true;}
static char *token(char **p){while(**p==' ')++*p;char *s=*p;while(**p&&**p!=' ')++*p;if(**p)*(*p)++=0;return s;}
static void reboot(void){for(unsigned i=0;i<100000;++i)if(!(inb(0x64)&2))break;outb(0x64,0xfe);}
static void command(char *line){char *rest=line,*cmd=token(&rest);if(!*cmd)return;
if(eq(cmd,"help")){console_write("help version clear echo mem ls write cat rm reboot halt\nwrite NAME TEXT - create or replace a RAM file (4095 bytes max)\nRAM files are volatile. Serial: COM1, 115200 baud.\n");}
else if(eq(cmd,"version"))console_write("Avalanche Kernel 0.1.0 - x86 / Multiboot2 / BIOS + UEFI\nFreestanding C and assembly. No Linux code or runtime.\n");
else if(eq(cmd,"clear"))clear_screen();
else if(eq(cmd,"echo")){console_write(rest);console_putc('\n');}
else if(eq(cmd,"mem")){console_write("Managed physical RAM: ");number(available*4);console_write(" KiB; allocated: ");number(used*4);console_write(" KiB; free: ");number((available-used)*4);console_write(" KiB\nPhysical allocator manages usable pages below 128 MiB.\n");}
else if(eq(cmd,"ls")){unsigned count=0;for(unsigned i=0;i<FILES;++i)if(files[i].data){console_write(files[i].name);console_write("  ");number(files[i].len);console_write(" bytes\n");++count;}if(!count)console_write("RAM filesystem is empty.\n");}
else if(eq(cmd,"write")){char *name=token(&rest);if(!filename(name)){console_write("Use a filename of 1-31 letters, digits, dots, dashes or underscores.\n");return;}struct file *f=lookup(name);if(!f)for(unsigned i=0;i<FILES;++i)if(!files[i].data){f=&files[i];break;}if(!f){console_write("File table full.\n");return;}if(!f->data)f->data=page_alloc();if(!f->data){console_write("Out of memory.\n");return;}copy(f->name,name,sizeof f->name);copy(f->data,rest,PAGE);f->len=strlen(f->data);console_write("Saved in RAM.\n");}
else if(eq(cmd,"cat")||eq(cmd,"rm")){char *name=token(&rest);struct file *f=lookup(name);if(!f){console_write("No such file.\n");return;}if(eq(cmd,"cat")){console_write(f->data);console_putc('\n');}else{page_free(f->data);memset(f,0,sizeof *f);}}
else if(eq(cmd,"reboot")){console_write("Rebooting via keyboard controller...\n");reboot();console_write("Firmware did not respond. Use your machine's reset control.\n");}
else if(eq(cmd,"halt")){console_write("CPU halted. Reset the machine to continue.\n");for(;;)__asm__ volatile("cli; hlt");}
else console_write("Unknown command. Type help.\n");}
void kernel_main(uint32_t magic,uint32_t mbi){
/* Polling kernel: mask both PICs; interrupts remain disabled. */
outb(0x21,255);outb(0xa1,255);outb(0x3f9,0);outb(0x3fb,0x80);outb(0x3f8,1);outb(0x3f9,0);outb(0x3fb,3);outb(0x3fa,0xc7);outb(0x3fc,0x0b);serial=inb(0x3fd)!=0xff;idt_init();
if(magic!=0x36d76289){console_write("Avalanche: invalid Multiboot2 handoff.\n");for(;;)__asm__ volatile("hlt");}
uint32_t total=*(uint32_t *)mbi;if(total<16||total>0x100000||mbi>0xffffffffu-total){console_write("Invalid boot information.\n");for(;;)__asm__ volatile("hlt");}
uint32_t end=mbi+total;
for(struct tag *t=(void *)(mbi+8);(uint32_t)t<=end-8;){if(t->size<8||t->size>end-(uint32_t)t)break;if(t->type==0)break;
if(t->type==6&&t->size>=16){uint32_t step=*(uint32_t *)((uint8_t *)t+8);if(step>=24)for(uint32_t off=16;off<=t->size-24;){struct mmap_entry *e=(void *)((uint8_t *)t+off);if(e->type==1&&e->base<(uint64_t)PAGES*PAGE){uint64_t last=e->base+e->len;if(last<e->base||last>(uint64_t)PAGES*PAGE)last=(uint64_t)PAGES*PAGE;uint32_t a=(uint32_t)((e->base+PAGE-1)>>12),b=(uint32_t)(last>>12);for(uint32_t i=a;i<b;++i)if(!bit(usable,i)){mark(usable,i,true);++available;}}if(step>t->size-off)break;off+=step;}}
if(t->type==8&&t->size>=38){struct fbtag *f=(void *)t;uint64_t span=(uint64_t)f->pitch*f->height;if(f->kind==1&&(f->bpp==24||f->bpp==32)&&f->width>=120&&f->height>=32&&f->width<=4096&&f->height<=2160&&f->pitch>=f->width*(f->bpp/8)&&f->addr+span>=f->addr&&f->addr+span<=0xffffffffu&&f->rs&&f->rs<=8&&f->gs&&f->gs<=8&&f->bs&&f->bs<=8&&f->rp+f->rs<=f->bpp&&f->gp+f->gs<=f->bpp&&f->bp+f->bs<=f->bpp){fb=(void *)(uint32_t)f->addr;pitch=f->pitch;fw=f->width;fh=f->height;bytespp=f->bpp/8;rpos=f->rp;gpos=f->gp;bpos=f->bp;rbits=f->rs;gbits=f->gs;bbits=f->bs;cols=fw/12;rows=fh/16;fg=rgb(205,225,247);bg=rgb(13,23,39);}}
uint32_t next=(t->size+7)&~7u;if(next>end-(uint32_t)t)break;t=(void *)((uint8_t *)t+next);}
reserve(0,(uint32_t)&kernel_end);reserve(mbi,end);if(fb)reserve((uint32_t)fb,(uint32_t)fb+pitch*fh);
/* Boot modules remain reserved so future distribution payloads are not overwritten. */
for(struct tag *t=(void *)(mbi+8);(uint32_t)t<=end-8;){if(t->size<8||t->size>end-(uint32_t)t||!t->type)break;if(t->type==3&&t->size>=16){uint32_t *m=(void *)((uint8_t *)t+8);if(m[1]>=m[0])reserve(m[0],m[1]);}uint32_t next=(t->size+7)&~7u;if(next>end-(uint32_t)t)break;t=(void *)((uint8_t *)t+next);}
clear_screen();console_write("AVALANCHE KERNEL\n");console_write("--------------------------------------------------\nA small independent operating system kernel.\nBIOS / UEFI through GRUB Multiboot2\n\n");if(!fb)console_write("No supported RGB framebuffer. Using serial console.\n");console_write("Type help to see commands. Files live in RAM only.\n\n");char line[4096];for(;;){console_write("avalanche> ");size_t n=0;for(;;){char c=input();if(c=='\n'){console_putc('\n');break;}if(c=='\b'){if(n){--n;console_write("\b \b");}continue;}if(c>=32&&c<=126&&n<sizeof(line)-1){line[n++]=c;console_putc(c);}}line[n]=0;command(line);}
}
