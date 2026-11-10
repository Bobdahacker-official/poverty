#include "system.h"
#define RX_COUNT 64
#define TX_COUNT 16
#define OWN (1u<<31)
#define EOR (1u<<30)
#define FIRST (1u<<29)
#define LAST (1u<<28)
struct descriptor { volatile uint32_t flags, options, address_low, address_high; };
static struct descriptor rx[RX_COUNT] __attribute__((aligned(256)));
static struct descriptor tx[TX_COUNT] __attribute__((aligned(256)));
static uint8_t rx_data[RX_COUNT][2048] __attribute__((aligned(4096)));
static uint8_t tx_data[TX_COUNT][2048] __attribute__((aligned(4096)));
static volatile uint8_t *mmio;
static uint16_t port;
static unsigned rx_next,tx_next;
static const struct pci_device *adapter;
static bool available;
static uint32_t received,transmitted,errors,stalls,mac_revision;
static uint8_t r8(unsigned reg) {return mmio?mmio[reg]:io_in8(port+reg);}
static uint16_t r16(unsigned reg) {return mmio?*(volatile uint16_t *)(mmio+reg):io_in16(port+reg);}
static uint32_t r32(unsigned reg) {return mmio?*(volatile uint32_t *)(mmio+reg):io_in32(port+reg);}
static void w8(unsigned reg,uint8_t v) {if(mmio)mmio[reg]=v;else io_out8(port+reg,v);}
static void w16(unsigned reg,uint16_t v) {if(mmio)*(volatile uint16_t *)(mmio+reg)=v;else io_out16(port+reg,v);}
static void w32(unsigned reg,uint32_t v) {if(mmio)*(volatile uint32_t *)(mmio+reg)=v;else io_out32(port+reg,v);}
static void ring(unsigned reg,const void *data) {
  uint64_t address=page_physical(data);w32(reg,address);w32(reg+4,address>>32);
}
static bool phy_read(unsigned reg,uint16_t *value) {
  w32(0x60,(reg&31)<<16);
  for(unsigned i=0;i<100000;i++) {uint32_t result=r32(0x60);if(result&0x80000000u) {*value=result;return true;}cpu_pause();}
  return false;
}
static bool phy_write(unsigned reg,uint16_t value) {
  w32(0x60,0x80000000u|((reg&31)<<16)|value);
  for(unsigned i=0;i<100000;i++) {if(!(r32(0x60)&0x80000000u))return true;cpu_pause();}
  return false;
}
static void phy_start(void) {
  uint16_t id,status,control;
  if(!phy_write(31,0) || !phy_read(2,&id) || !id || id==65535 ||
      !phy_read(1,&status) || !phy_read(0,&control))return;
  if((control&0x1000) && !(control&0x0c00))return;
  uint16_t advertisement=1,extended=0;
  if(status&(1u<<14))advertisement|=0x100;
  if(status&(1u<<13))advertisement|=0x80;
  if(status&(1u<<12))advertisement|=0x40;
  if(status&(1u<<11))advertisement|=0x20;
  phy_write(4,advertisement);
  if((status&0x100) && phy_read(15,&extended)) {
    uint16_t gigabit=0;if(extended&0x2000)gigabit|=0x200;if(extended&0x1000)gigabit|=0x100;
    phy_write(9,gigabit);
  }
  phy_write(0,(control&~0x0c00u)|0x1200u);
}
bool rtl8169_init(uint8_t mac[6]) {
  const uint16_t devices[]={0x8169,0x8168,0x8167};
  for(unsigned i=0;i<sizeof(devices)/sizeof(devices[0]);i++) {
    adapter=pci_find(0x10ec,devices[i]);if(adapter)break;
  }
  if(!adapter)return false;
  /* The 8125/8126 use a different descriptor/register ABI and are not matched. */
  for(unsigned bar=0;bar<6 && !mmio;bar++) {
    if(!(adapter->bar[bar]&1)) {
      uint64_t base=pci_memory_bar(adapter,bar);
      if(base && platform_mmio(base,256))mmio=page_pointer(base);
      if((adapter->bar[bar]&6)==4)bar++;
    }
  }
  if(!mmio)for(unsigned bar=0;bar<6;bar++)if(adapter->bar[bar]&1) {port=adapter->bar[bar]&~3u;break;}
  if(!mmio && !port)return false;
  pci_enable(adapter,mmio?6:5);
  w16(0x3c,0);w8(0x37,0x10);
  uint32_t start=clock_ticks();unsigned spins=0;
  while(r8(0x37)&0x10) {
    if((uint32_t)(clock_ticks()-start)>100 || ++spins>5000000)goto failed;
    cpu_pause();
  }
  mac_revision=r32(0x40)&0x7cf00000u;
  bool nonzero=false,nonff=false;
  for(unsigned i=0;i<6;i++) {mac[i]=r8(i);nonzero|=mac[i]!=0;nonff|=mac[i]!=255;}
  if(!nonzero || !nonff || (mac[0]&1))goto failed;
  memset(rx,0,sizeof(rx));memset(tx,0,sizeof(tx));
  for(unsigned i=0;i<RX_COUNT;i++) {
    uint64_t address=page_physical(rx_data[i]);rx[i].address_low=address;rx[i].address_high=address>>32;
    rx[i].flags=OWN|(i==RX_COUNT-1?EOR:0)|2048;
  }
  for(unsigned i=0;i<TX_COUNT;i++) {
    uint64_t address=page_physical(tx_data[i]);tx[i].address_low=address;tx[i].address_high=address>>32;
    tx[i].flags=i==TX_COUNT-1?EOR:0;
  }
  rx_next=tx_next=0;
  w8(0x50,0xc0);
  w16(0x3c,0);w16(0x3e,0xffff);
  /* Disable checksum/VLAN offload: the stack consumes complete Ethernet frames. */
  w16(0xe0,(r16(0xe0)&~0x60u)|8u);
  w16(0xda,1536);w8(0xec,0x3f);
  ring(0x20,tx);ring(0xe4,rx);
  w32(0x40,(3u<<24)|(7u<<8));
  w32(0x44,(r32(0x44)&0xff7e1880u)|(7u<<13)|(7u<<8)|0x0a);
  w32(0x08,0);w32(0x0c,0); /* No multicast hash entries until multicast is joined. */
  __asm__ volatile("sfence":::"memory");
  phy_start();
  w8(0x37,0x0c);w8(0x50,0);
  available=true;return true;
failed:
  w8(0x37,0);pci_write(adapter,4,pci_read(adapter,4)&0xfffbu);
  return false;
}
bool rtl8169_link(void) {return available && (r8(0x6c)&2);}
bool rtl8169_send(const void *frame,unsigned length) {
  if(!available || !rtl8169_link() || length<14 || length>1514)return false;
  unsigned index=tx_next;uint32_t start=clock_ticks();
  while(tx[index].flags&OWN) {
    if((uint32_t)(clock_ticks()-start)>100) {stalls++;return false;}cpu_pause();
  }
  unsigned padded=length<60?60:length;
  memset(tx_data[index],0,padded);memcpy(tx_data[index],frame,length);
  tx[index].options=0;
  __asm__ volatile("sfence":::"memory");
  tx[index].flags=OWN|FIRST|LAST|(index==TX_COUNT-1?EOR:0)|padded;
  __asm__ volatile("sfence":::"memory");w8(0x38,0x40);
  tx_next=(index+1)%TX_COUNT;transmitted++;return true;
}
void rtl8169_poll(void (*receive)(const uint8_t *,uint32_t)) {
  if(!available)return;
  for(unsigned budget=0;budget<RX_COUNT;budget++) {
    unsigned index=rx_next;uint32_t flags=rx[index].flags;
    if(flags&OWN)break;
    __asm__ volatile("lfence":::"memory");
    unsigned length=flags&0x3fff;
    if((flags&(FIRST|LAST))==(FIRST|LAST) && !(flags&0x00380000u) && length>=18 && length<=1518) {
      receive(rx_data[index],length-4);received++;
    } else errors++;
    rx[index].options=0;
    __asm__ volatile("sfence":::"memory");
    rx[index].flags=OWN|(index==RX_COUNT-1?EOR:0)|2048;
    rx_next=(index+1)%RX_COUNT;
  }
  w16(0x3e,r16(0x3e));
}
void rtl8169_diagnostics(void) {
  if(!adapter)return;
  console_write("Realtek MAC revision: ");print_hex(mac_revision);
  console_write(available?" initialized ":" initialization failed ");
  console_write(rtl8169_link()?"link up\n":"link down\n");
  console_write("RX ");print_uint(received);console_write(" TX ");print_uint(transmitted);
  console_write(" errors ");print_uint(errors);console_write(" stalled TX ");print_uint(stalls);console_putc('\n');
}
