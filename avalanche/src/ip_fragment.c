#include "system.h"
#include "ip_fragment.h"
#define SLOTS 8
#define PAYLOAD 65515u
struct datagram {
  bool used,rejected,first,last;
  uint32_t source,destination,created;
  uint16_t id,total,maximum,covered;
  uint8_t protocol,ihl,header[74],blocks[1024],data[PAYLOAD];
};
static struct datagram datagrams[SLOTS];
static uint8_t assembled[65549];
static bool delivering;
static uint16_t be16(const uint8_t *p) {return (uint16_t)p[0]<<8|p[1];}
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t value) {p[0]=value>>8;p[1]=value;}
void ip4_reassemble(const uint8_t *frame,unsigned frame_length,void (*receive)(const uint8_t *,uint32_t)) {
  if(delivering || frame_length<34)return;
  const uint8_t *h=frame+14;unsigned ihl=(h[0]&15)*4,total=be16(h+2),flags=be16(h+6);
  if(ihl<20 || ihl>60 || total<ihl || total>frame_length-14 || (flags&0xc000))return;
  unsigned length=total-ihl,offset=(flags&8191)*8;
  if(!length || offset>PAYLOAD || length>PAYLOAD-offset || ((flags&0x2000) && (length&7)))return;
  uint32_t now=clock_ticks(),source=be32(h+12),destination=be32(h+16);unsigned selected=SLOTS,oldest=0;
  for(unsigned i=0;i<SLOTS;i++) {
    struct datagram *d=&datagrams[i];
    if(d->used && (uint32_t)(now-d->created)>=3000)d->used=false;
    if(d->used && d->source==source && d->destination==destination && d->id==be16(h+4) && d->protocol==h[9]) {selected=i;break;}
    if(!d->used)oldest=i;
    else if(datagrams[oldest].used && (uint32_t)(now-d->created)>(uint32_t)(now-datagrams[oldest].created))oldest=i;
  }
  if(selected==SLOTS) {
    selected=oldest;struct datagram *d=&datagrams[selected];memset(d,0,sizeof(*d));
    d->used=true;d->created=now;d->source=source;d->destination=destination;d->id=be16(h+4);d->protocol=h[9];
  }
  struct datagram *d=&datagrams[selected];if(d->rejected)return;
  if((d->last && offset+length>d->total) || (!(flags&0x2000) && d->last && d->total!=offset+length)) {d->rejected=true;return;}
  if(!(flags&0x2000)) {d->last=true;d->total=offset+length;if(d->maximum>d->total) {d->rejected=true;return;}}
  unsigned first=offset/8,last=(offset+length-1)/8;
  for(unsigned block=first;block<=last;block++)if((d->blocks[block/8]>>(block&7))&1) {d->rejected=true;return;}
  for(unsigned block=first;block<=last;block++)d->blocks[block/8]|=1u<<(block&7);
  memcpy(d->data+offset,h+ihl,length);d->covered+=length;
  if(offset+length>d->maximum)d->maximum=offset+length;
  if(!offset) {d->first=true;d->ihl=ihl;memcpy(d->header,frame,14+ihl);}
  if(!d->first || !d->last || d->covered!=d->total)return;
  if(d->total>65535u-d->ihl) {d->rejected=true;return;}
  memcpy(assembled,d->header,14+d->ihl);memcpy(assembled+14+d->ihl,d->data,d->total);
  uint8_t *out=assembled+14;put16(out+2,d->ihl+d->total);put16(out+6,0);put16(out+10,0);
  uint32_t sum=0;for(unsigned i=0;i<d->ihl;i+=2)sum+=be16(out+i);
  while(sum>>16)sum=(sum&65535)+(sum>>16);
  put16(out+10,(uint16_t)~sum);
  unsigned size=14+d->ihl+d->total;d->used=false;delivering=true;receive(assembled,size);delivering=false;
}
