#include "system.h"
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#include "../vendor/rge/registers.h"
#include "../vendor/rge/firmware.h"
#define MII_BMCR 0
#define MII_ANAR 4
#define MII_100T2CR 9
#define BMCR_RESET 0x8000
#define BMCR_AUTOEN 0x1000
#define BMCR_PDOWN 0x0800
#define BMCR_STARTNEG 0x0200
#define ANAR_10 0x0020
#define ANAR_10_FD 0x0040
#define ANAR_TX 0x0080
#define ANAR_TX_FD 0x0100
#define GTCR_ADV_1000THDX 0x0100
#define GTCR_ADV_1000TFDX 0x0200
#define ETIMEDOUT 110
#define RGE_FLAG_DESCV2 1
#define RGE_FLAG_MULTIVEC 4
#define nitems(a) ((int)(sizeof(a)/sizeof((a)[0])))
struct rge_softc {
  volatile uint8_t *mmio;
  enum rge_mac_type rge_type;
  uint32_t rge_flags;
  bool io_failed;
  uint64_t rge_mcodever;
  uint16_t rge_rcodever;
  const struct rge_pair *rge_macp,*rge_phyp;
  int rge_nmac,rge_nphy;
};
#define RGE_READ_1(sc,r) (*(volatile uint8_t *)((sc)->mmio+(r)))
#define RGE_READ_2(sc,r) (*(volatile uint16_t *)((sc)->mmio+(r)))
#define RGE_READ_4(sc,r) (*(volatile uint32_t *)((sc)->mmio+(r)))
#define RGE_WRITE_1(sc,r,v) (RGE_READ_1(sc,r)=(uint8_t)(v))
#define RGE_WRITE_2(sc,r,v) (RGE_READ_2(sc,r)=(uint16_t)(v))
#define RGE_WRITE_4(sc,r,v) (RGE_READ_4(sc,r)=(uint32_t)(v))
static uint64_t tsc_per_us;
static uint64_t timestamp(void) {
  uint32_t lo,hi;
  __asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");
  return ((uint64_t)hi<<32)|lo;
}
static bool calibrate_delay(void) {
  /* PIT channel 2 supplies a machine-independent reference for the local TSC. */
  uint8_t saved=io_in8(0x61);
  io_out8(0x61,saved&~3u);io_out8(0x43,0xb0);
  io_out8(0x42,11932&255);io_out8(0x42,11932>>8);
  uint64_t begin=timestamp();io_out8(0x61,(saved&~2u)|1u);
  unsigned count=0;
  while(!(io_in8(0x61)&0x20) && ++count<10000000)cpu_pause();
  uint64_t elapsed=timestamp()-begin;io_out8(0x61,saved);
  if(count>=10000000 || elapsed<10000)return false;
  tsc_per_us=(elapsed+9999)/10000;return true;
}
static void delay_us(unsigned us) {
  uint64_t begin=timestamp(),ticks=tsc_per_us*us;
  while(timestamp()-begin<ticks)cpu_pause();
}
#define DELAY(us) delay_us(us)
#include "../vendor/rge/hardware.inc"
static struct rge_softc device;
static const struct pci_device *adapter;
static bool available;
static uint32_t revision,received,transmitted,errors,stalls;
#define RX_COUNT 256
#define TX_COUNT 64
static volatile struct rge_rx_desc rx[RX_COUNT] __attribute__((aligned(256)));
static volatile struct rge_rx_desc_v2 rx_v2[RX_COUNT] __attribute__((aligned(256)));
static volatile struct rge_tx_desc_v2 tx_v2[TX_COUNT] __attribute__((aligned(256)));
static volatile struct rge_tx_desc tx[TX_COUNT] __attribute__((aligned(256)));
static uint8_t rx_data[RX_COUNT][2048] __attribute__((aligned(4096)));
static uint8_t tx_data[TX_COUNT][2048] __attribute__((aligned(4096)));
static unsigned rx_next,tx_next;
_Static_assert(sizeof(struct rge_rx_desc_v2)==16,"rge v2 RX descriptor");
_Static_assert(sizeof(struct rge_tx_desc_v2)==16,"rge v2 TX descriptor");
_Static_assert(sizeof(struct rge_rx_desc)==32,"rge v1 RX descriptor");
_Static_assert(sizeof(struct rge_tx_desc)==32,"rge v1 TX descriptor");
static volatile uint32_t *tx_status(unsigned i) {
  if(device.rge_flags&RGE_FLAG_DESCV2)return (volatile uint32_t *)((volatile uint8_t *)tx_v2+i*16);
  return (volatile uint32_t *)((volatile uint8_t *)tx+i*32);
}
static volatile uint32_t *rx_status(unsigned i) {
  if(device.rge_flags&RGE_FLAG_DESCV2)return (volatile uint32_t *)((volatile uint8_t *)rx_v2+i*16+12);
  return (volatile uint32_t *)((volatile uint8_t *)rx+i*32+28);
}
static void rearm_rx(unsigned i) {
  if(device.rge_flags&RGE_FLAG_DESCV2) {
    rx_v2[i].qword0.rge_addr=page_physical(rx_data[i]);rx_v2[i].rx_qword0.rge_extsts=0;
  } else {
    rx[i].hi_qword0.rge_addr=page_physical(rx_data[i]);rx[i].hi_qword1.rx_qword4.rge_extsts=0;
  }
  __asm__ volatile("sfence":::"memory");
  *rx_status(i)=RGE_RDCMDSTS_OWN|(i==RX_COUNT-1?RGE_RDCMDSTS_EOR:0)|sizeof(rx_data[i]);
}
struct firmware {
  uint32_t revision;
  enum rge_mac_type type;
  const struct rge_pair *mac,*phy;
  unsigned nmac,nphy;
};
#define FW(rev,type,mac,phy) {rev,type,mac,phy,nitems(mac),nitems(phy)}
static const struct firmware firmware[] = {
  FW(0x60900000,MAC_R25,rtl8125_mac_bps,mac_r25_mcu),
  FW(0x64100000,MAC_R25B,rtl8125b_mac_bps,mac_r25b_mcu),
  {0x68800000,MAC_R25D_1,NULL,mac_r25d_1_mcu,0,nitems(mac_r25d_1_mcu)},
  FW(0x68900000,MAC_R25D_2,rtl8125d_2_mac_bps,mac_r25d_2_mcu),
  FW(0x64900000,MAC_R26_1,rtl8126_1_mac_bps,mac_r26_1_mcu),
  FW(0x64a00000,MAC_R26_2,rtl8126_2_mac_bps,mac_r26_2_mcu),
  FW(0x6c900000,MAC_R27,rtl8127_mac_bps,mac_r27_mcu)
};
static void ring_address(struct rge_softc *sc,unsigned reg,const volatile void *p) {
  uint64_t address=page_physical((const void *)p);
  RGE_WRITE_4(sc,reg,address);RGE_WRITE_4(sc,reg+4,address>>32);
}
static void autonegotiate(struct rge_softc *sc) {
  RGE_PHY_CLRBIT(sc,0xa428,0x0200);
  RGE_PHY_CLRBIT(sc,0xa5ea,RGE_TYPE_R26(sc)?7:1);
  uint16_t speeds=rge_read_phy_ocp(sc,0xa5d4);
  speeds&=~(RGE_ADV_2500TFDX|RGE_ADV_5000TFDX|RGE_ADV_10000TFDX);
  speeds|=RGE_ADV_2500TFDX;
  if(RGE_TYPE_R26(sc))speeds|=RGE_ADV_5000TFDX;
  else if(sc->rge_type==MAC_R27)speeds|=RGE_ADV_5000TFDX|RGE_ADV_10000TFDX;
  /* No pause advertisement: polling rings do not implement flow control. */
  rge_write_phy(sc,0,MII_ANAR,1|ANAR_10|ANAR_10_FD|ANAR_TX|ANAR_TX_FD);
  rge_write_phy(sc,0,MII_100T2CR,GTCR_ADV_1000TFDX|GTCR_ADV_1000THDX);
  rge_write_phy_ocp(sc,0xa5d4,speeds);
  rge_write_phy(sc,0,MII_BMCR,BMCR_RESET|BMCR_AUTOEN|BMCR_STARTNEG);
}
bool rge_init(uint8_t mac[6]) {
  struct rge_softc *sc=&device;
  adapter=pci_find(0x10ec,0x8125);
  if(!adapter)adapter=pci_find(0x10ec,0x8126);
  if(!adapter)adapter=pci_find(0x10ec,0x8127);
  if(!adapter)adapter=pci_find(0x10ec,0x3000);
  if(!adapter)adapter=pci_find(0x10ec,0x5000);
  if(!adapter)return false;
  for(unsigned bar=0;bar<6 && !sc->mmio;bar++) {
    if(adapter->bar[bar]&1)continue;
    uint64_t base=pci_memory_bar(adapter,bar);
    if(base && platform_mmio(base,0x10000))sc->mmio=page_pointer(base);
    if((adapter->bar[bar]&6)==4)bar++;
  }
  if(!sc->mmio)return false;
  pci_enable(adapter,6);
  revision=RGE_READ_4(sc,RGE_TXCFG)&0x7cf00000;
  const struct firmware *fw=NULL;
  for(unsigned i=0;i<sizeof(firmware)/sizeof(firmware[0]);i++)
    if(firmware[i].revision==revision) {fw=&firmware[i];break;}
  if(!fw || !calibrate_delay())goto failed;
  sc->rge_type=fw->type;sc->rge_macp=fw->mac;sc->rge_phyp=fw->phy;
  sc->rge_nmac=fw->nmac;sc->rge_nphy=fw->nphy;
  if(sc->rge_type==MAC_R27) {
    sc->rge_flags=RGE_FLAG_DESCV2;
    if(rge_read_mac_ocp(sc,0xd006)==7)sc->rge_type=MAC_R27ATF;
  }
  bool nonzero=false,nonff=false;
  for(unsigned i=0;i<6;i++) {
    mac[i]=RGE_READ_1(sc,RGE_ADDR0+i);nonzero|=mac[i]!=0;nonff|=mac[i]!=255;
  }
  if(!nonzero || !nonff || (mac[0]&1))goto failed;
  if(rge_chipinit(sc) || sc->io_failed || rge_phy_config(sc) || sc->io_failed)goto failed;
  rge_set_macaddr(sc,mac);
  memset((void *)rx,0,sizeof(rx));memset((void *)tx,0,sizeof(tx));
  memset((void *)rx_v2,0,sizeof(rx_v2));memset((void *)tx_v2,0,sizeof(tx_v2));
  for(unsigned i=0;i<RX_COUNT;i++)rearm_rx(i);
  for(unsigned i=0;i<TX_COUNT;i++) {
    if(sc->rge_flags&RGE_FLAG_DESCV2)tx_v2[i].rge_addr=page_physical(tx_data[i]);
    else tx[i].rge_addr=page_physical(tx_data[i]);
    *tx_status(i)=i==TX_COUNT-1?RGE_TDCMDSTS_EOR:0;
  }
  RGE_SETBIT_1(sc,RGE_EECMD,RGE_EECMD_WRITECFG);
  RGE_CLRBIT_1(sc,0xf1,0x80);rge_disable_aspm_clkreq(sc);
  RGE_WRITE_2(sc,RGE_EEE_TXIDLE_TIMER,1536+32);
  ring_address(sc,RGE_RXDESC_ADDR_LO,sc->rge_flags&RGE_FLAG_DESCV2?(const volatile void *)rx_v2:rx);
  ring_address(sc,RGE_TXDESC_ADDR_LO,sc->rge_flags&RGE_FLAG_DESCV2?(const volatile void *)tx_v2:tx);
  uint32_t rxconf,val,reg;int i,num_miti;
#include "../vendor/rge/mac-setup.inc"
  RGE_CLRBIT_2(sc,RGE_CPLUSCMD,RGE_CPLUSCMD_RXCSUM);
  RGE_WRITE_2(sc,RGE_RXMAXSIZE,1536|((sc->rge_type==MAC_R25 ||
    sc->rge_type==MAC_R25B)?0:RGE_RXCFG_VLANPHYS));
  RGE_CLRBIT_1(sc,RGE_PPSW,0x08);
  RGE_WRITE_4(sc,RGE_MAR0,0);RGE_WRITE_4(sc,RGE_MAR4,0);
  RGE_SETBIT_4(sc,RGE_RXCFG,RGE_RXCFG_INDIV|RGE_RXCFG_BROAD);
  if(RGE_TYPE_R27(sc))RGE_CLRBIT_2(sc,RGE_RADMFIFO_PROTECT,0x2001);
  rge_disable_aspm_clkreq(sc);
  RGE_CLRBIT_1(sc,RGE_EECMD,RGE_EECMD_WRITECFG);DELAY(10);
  autonegotiate(sc);
  if(sc->io_failed)goto failed;
  RGE_WRITE_4(sc,RGE_IMR,0);RGE_WRITE_4(sc,RGE_ISR,0xffffffff);
  __asm__ volatile("sfence":::"memory");
  RGE_WRITE_1(sc,RGE_CMD,RGE_CMD_TXENB|RGE_CMD_RXENB);
  rx_next=tx_next=0;available=true;return true;
failed:
  RGE_WRITE_4(sc,RGE_IMR,0);RGE_WRITE_1(sc,RGE_CMD,0);
  pci_write(adapter,4,pci_read(adapter,4)&0xfffbu);return false;
}
bool rge_link(void) {return available && (RGE_READ_2(&device,RGE_PHYSTAT)&RGE_PHYSTAT_LINK);}
bool rge_send(const void *frame,unsigned length) {
  if(!rge_link() || length<14 || length>1514)return false;
  unsigned index=tx_next;uint32_t start=clock_ticks();
  while(*tx_status(index)&RGE_TDCMDSTS_OWN) {
    if((uint32_t)(clock_ticks()-start)>100) {stalls++;return false;}cpu_pause();
  }
  if(*tx_status(index)&RGE_TDCMDSTS_TXERR)errors++;
  unsigned padded=length<60?60:length;
  memset(tx_data[index],0,padded);memcpy(tx_data[index],frame,length);
  if(device.rge_flags&RGE_FLAG_DESCV2)tx_v2[index].rge_extsts=0;
  else tx[index].rge_extsts=0;
  __asm__ volatile("sfence":::"memory");
  *tx_status(index)=RGE_TDCMDSTS_OWN|RGE_TDCMDSTS_SOF|RGE_TDCMDSTS_EOF|
    (index==TX_COUNT-1?RGE_TDCMDSTS_EOR:0)|padded;
  __asm__ volatile("sfence":::"memory");
  RGE_WRITE_2(&device,RGE_TXSTART,RGE_TXSTART_START);
  tx_next=(index+1)%TX_COUNT;transmitted++;return true;
}
void rge_poll(void (*receive)(const uint8_t *,uint32_t)) {
  if(!available)return;
  for(unsigned budget=0;budget<RX_COUNT;budget++) {
    unsigned index=rx_next;
    uint32_t flags=*rx_status(index);
    if(flags&RGE_RDCMDSTS_OWN)break;
    __asm__ volatile("lfence":::"memory");
    unsigned length=flags&RGE_RDCMDSTS_FRAGLEN;
    uint32_t sof=device.rge_flags&RGE_FLAG_DESCV2?RGE_RDCMDSTS_SOF_V2:RGE_RDCMDSTS_SOF;
    uint32_t eof=device.rge_flags&RGE_FLAG_DESCV2?RGE_RDCMDSTS_EOF_V2:RGE_RDCMDSTS_EOF;
    uint32_t bad=device.rge_flags&RGE_FLAG_DESCV2?RGE_RDCMDSTS_RXERRSUM_V2:RGE_RDCMDSTS_RXERRSUM;
    if((flags&(sof|eof))==(sof|eof) && !(flags&bad)
       && length>=18 && length<=1518) {receive(rx_data[index],length-4);received++;}
    else errors++;
    rearm_rx(index);
    rx_next=(index+1)%RX_COUNT;
  }
  RGE_WRITE_4(&device,RGE_ISR,RGE_READ_4(&device,RGE_ISR));
}
void rge_diagnostics(void) {
  if(!adapter)return;
  console_write("Realtek 2.5/5/10 GbE revision ");print_hex(revision);
  console_write(available?" initialized ":" initialization failed ");
  console_write(rge_link()?"link up\n":"link down\n");
  console_write("RX ");print_uint(received);console_write(" TX ");print_uint(transmitted);
  console_write(" errors ");print_uint(errors);console_write(" stalled TX ");print_uint(stalls);console_putc('\n');
}
