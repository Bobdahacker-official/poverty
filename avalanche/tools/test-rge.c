#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/rge.c"
static uint8_t registers[65536] __attribute__((aligned(16)));
static unsigned packets;
uint32_t clock_ticks(void) {static uint32_t t;return t++;}
static void receive_frame(const uint8_t *p,uint32_t n) {
  assert(n==60 && p[0]==0x12 && p[59]==0x34);packets++;
}
int main(void) {
  device.mmio=registers;available=true;
  RGE_WRITE_2(&device,RGE_PHYSTAT,RGE_PHYSTAT_LINK);
  uint8_t frame[1515]={0};frame[0]=0x12;frame[59]=0x34;
  assert(!rge_send(frame,13) && !rge_send(frame,1515));
  memset(tx_data[0],0xff,sizeof(tx_data[0]));
  assert(rge_send(frame,14));
  assert(tx[0].rge_cmdsts==(RGE_TDCMDSTS_OWN|RGE_TDCMDSTS_SOF|RGE_TDCMDSTS_EOF|60));
  assert(!memcmp(tx_data[0],frame,14));
  for(unsigned i=14;i<60;i++)assert(tx_data[0][i]==0);
  assert(RGE_READ_2(&device,RGE_TXSTART)==RGE_TXSTART_START);
  tx_next=TX_COUNT-1;assert(rge_send(frame,1514));assert(tx_next==0);
  assert(tx[TX_COUNT-1].rge_cmdsts&RGE_TDCMDSTS_EOR);
  assert(!rge_send(frame,60) && stalls==1);
  for(unsigned i=0;i<RX_COUNT;i++)rx[i].hi_qword1.rx_qword4.rge_cmdsts=RGE_RDCMDSTS_OWN;
  rx_next=RX_COUNT-1;memcpy(rx_data[rx_next],frame,60);
  rx[rx_next].hi_qword1.rx_qword4.rge_cmdsts=RGE_RDCMDSTS_EOR|RGE_RDCMDSTS_SOF|RGE_RDCMDSTS_EOF|64;
  rge_poll(receive_frame);assert(packets==1 && received==1 && rx_next==0);
  assert(rx[RX_COUNT-1].hi_qword1.rx_qword4.rge_cmdsts==(RGE_RDCMDSTS_OWN|RGE_RDCMDSTS_EOR|2048));
  assert(rx[RX_COUNT-1].hi_qword0.rge_addr==page_physical(rx_data[RX_COUNT-1]));
  rx[0].hi_qword1.rx_qword4.rge_cmdsts=RGE_RDCMDSTS_SOF|64;
  rx[1].hi_qword1.rx_qword4.rge_cmdsts=RGE_RDCMDSTS_SOF|RGE_RDCMDSTS_EOF|RGE_RDCMDSTS_RXERRSUM|64;
  rx[2].hi_qword1.rx_qword4.rge_cmdsts=RGE_RDCMDSTS_SOF|RGE_RDCMDSTS_EOF|2048;
  rge_poll(receive_frame);assert(errors==3 && packets==1 && rx_next==3);
  RGE_WRITE_2(&device,RGE_PHYSTAT,0);assert(!rge_link() && !rge_send(frame,60));
  assert(firmware[2].nphy>=2528 && firmware[3].nphy>=1269 && firmware[4].nphy>=6989);
  assert(!device.io_failed);rge_read_phy_ocp(&device,0xa420);assert(device.io_failed);
  assert(rge_read_phy_ocp(&device,0xa420)==0xffff);
  device.io_failed=false;device.rge_flags=RGE_FLAG_DESCV2;
  RGE_WRITE_2(&device,RGE_PHYSTAT,RGE_PHYSTAT_LINK);
  tx_next=TX_COUNT-1;assert(rge_send(frame,60));assert(tx_next==0);
  assert(tx_v2[TX_COUNT-1].rge_cmdsts==(RGE_TDCMDSTS_OWN|RGE_TDCMDSTS_SOF|RGE_TDCMDSTS_EOF|RGE_TDCMDSTS_EOR|60));
  assert(*tx_status(TX_COUNT-1)==tx_v2[TX_COUNT-1].rge_cmdsts);
  assert((uintptr_t)tx_status(1)-(uintptr_t)tx_status(0)==16);
  assert((uintptr_t)rx_status(1)-(uintptr_t)rx_status(0)==16);
  for(unsigned i=0;i<RX_COUNT;i++)rearm_rx(i);
  rx_next=RX_COUNT-1;*rx_status(rx_next)=RGE_RDCMDSTS_SOF_V2|RGE_RDCMDSTS_EOF_V2|64;
  rge_poll(receive_frame);assert(packets==2 && rx_next==0);
  assert(rx_v2[RX_COUNT-1].qword0.rge_addr==page_physical(rx_data[RX_COUNT-1]));
  assert(*rx_status(RX_COUNT-1)==(RGE_RDCMDSTS_OWN|RGE_RDCMDSTS_EOR|2048));
  *rx_status(0)=RGE_RDCMDSTS_SOF_V2|RGE_RDCMDSTS_EOF_V2|RGE_RDCMDSTS_RXERRSUM_V2|64;
  rge_poll(receive_frame);assert(errors==4 && packets==2 && rx_next==1);
  puts("PASS Realtek v1/v2 DMA: TX padding/ownership/wrap/stall, RX CRC/error/length/rearm/wrap, PHY timeout");
}
