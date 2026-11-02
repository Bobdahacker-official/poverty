#include "system.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  struct pci_device device = {0};
  assert(pci_memory_bar(NULL, 0) == 0);
  assert(pci_memory_bar(&device, 6) == 0);
  device.bar[0] = 0xfebc0000;
  assert(pci_memory_bar(&device, 0) == 0xfebc0000);
  device.bar[0] = 0x2000000c;
  device.bar[1] = 12;
  device.bar[2] = 0xfebe0000;
  assert(pci_memory_bar(&device, 0) == UINT64_C(0xc20000000));
  assert(pci_memory_bar(&device, 1) == 0);
  assert(pci_memory_bar(&device, 2) == 0xfebe0000);
  device.bar[3] = 0x80000004;
  device.bar[4] = 1;
  device.bar[5] = 0xfed00000;
  assert(pci_memory_bar(&device, 3) == UINT64_C(0x180000000));
  assert(pci_memory_bar(&device, 4) == 0);
  assert(pci_memory_bar(&device, 5) == 0xfed00000);
  device.bar[5] = 4;
  assert(pci_memory_bar(&device, 5) == 0);
  device.bar[0] = 0x1001;
  assert(pci_memory_bar(&device, 0) == 0);
  device.bar[0] = 0xf0000006;
  assert(pci_memory_bar(&device, 0) == 0);
  puts("PASS PCI 32/64-bit memory BAR decoding and paired-slot validation");
  return 0;
}
