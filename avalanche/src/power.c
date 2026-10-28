#include "system.h"
/* ACPI fixed hardware power controls. Firmware provides all register addresses. */
extern const uint8_t *acpi_table(const char *);
extern const uint8_t *acpi_physical_table(uint64_t,const char *);
static uint32_t word(const uint8_t *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t quad(const uint8_t *p) { uint64_t n; memcpy(&n,p,8); return n; }
bool power_flush(void) {
  bool ok=true;
  for (unsigned i=0;i<disk_count();i++) {
    const struct block_device *d=disk_get(i);
    if (d && !d->read_only && !disk_flush(i)) ok=false;
  }
  return ok;
}
bool firmware_reset(void) {
  const uint8_t *f=acpi_table("FACP");
  if (!f || word(f+4)<129 || !(word(f+112)&(1u<<10))) return false;
  const uint8_t *gas=f+116;
  uint64_t address=quad(gas+4);
  if (gas[1]!=8 || gas[2] || !address) return false;
  if (gas[0]==1 && address<=65535) io_out8(address,f[128]);
  else if (gas[0]==0 && platform_mmio(address,1))
    *(volatile uint8_t *)(uintptr_t)address=f[128];
  else return false;
  sleep_ticks(20);
  return false;
}
/* Decode only constant integers, never execute firmware AML as kernel code. */
static bool integer(const uint8_t **cursor,const uint8_t *end,unsigned *value) {
  if (*cursor>=end) return false;
  unsigned op=*(*cursor)++;
  if (op<=1) { *value=op; return true; }
  if (op==0x0a && *cursor<end) { *value=*(*cursor)++; return true; }
  return false;
}
bool firmware_poweroff(void) {
  const uint8_t *f=acpi_table("FACP");
  if (!f || word(f+4)<116 || (word(f+112)&(1u<<20))) return false;
  uint64_t address=word(f+40);
  if (word(f+4)>=148 && quad(f+140)) address=quad(f+140);
  const uint8_t *dsdt=acpi_physical_table(address,"DSDT");
  if (!dsdt) return false;
  uint32_t length=word(dsdt+4);
  unsigned a=0,b=0; bool found=false;
  for (unsigned i=36;i+8<length;i++) {
    if (dsdt[i]!=8) continue; /* NameOp followed by optional root prefix. */
    unsigned at=i+1;
    if (dsdt[at]=='\\') at++;
    if (at+6>=length || __builtin_memcmp(dsdt+at,"_S5_",4) || dsdt[at+4]!=0x12) continue;
    at+=5;
    unsigned first=dsdt[at], extra=first>>6, size=first&(extra ? 15 : 63);
    if (at+extra>=length) continue;
    for (unsigned j=0;j<extra;j++) size|=(unsigned)dsdt[at+1+j]<<(4+8*j);
    if (size<extra+4 || size>length-at) continue;
    const uint8_t *p=dsdt+at+extra+1,*end=dsdt+at+size;
    if (p>=end || *p++<2) continue;
    if (integer(&p,end,&a) && integer(&p,end,&b) && a<8 && b<8) { found=true; break; }
  }
  uint32_t pm1a=word(f+64),pm1b=word(f+68),smi=word(f+48);
  if (!found || !pm1a || pm1a>65533 || pm1b>65533 || f[89]<2) return false;
  if (!(io_in16(pm1a)&1)) {
    if (!smi || smi>65535 || !f[52]) return false;
    io_out8(smi,f[52]);
    uint32_t start=clock_ticks();
    while (!(io_in16(pm1a)&1)) { if (clock_ticks()-start>300) return false; cpu_pause(); }
  }
  io_out16(pm1a,(io_in16(pm1a)&~0x3c00u)|(a<<10)|(1u<<13));
  if (pm1b) io_out16(pm1b,(io_in16(pm1b)&~0x3c00u)|(b<<10)|(1u<<13));
  sleep_ticks(20);
  return false;
}
