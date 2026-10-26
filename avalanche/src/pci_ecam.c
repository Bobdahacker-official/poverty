#include "system.h"
/* Multiboot 2 ACPI RSDP tags supply firmware-owned table addresses. */
static uint64_t ecam[256];
static uint8_t checksum(const void *p, unsigned length) {
  const uint8_t *bytes = p;
  uint8_t sum = 0;
  for (unsigned i = 0; i < length; i++)
    sum += bytes[i];
  return sum;
}
static bool accessible(uint64_t p, uint64_t length) {
  return p >= 4096 && length && p < ((uint64_t)IDENTITY_ENTRIES << 21) &&
         length <= ((uint64_t)IDENTITY_ENTRIES << 21) - p;
}
static const uint8_t *table(uint64_t physical, const char *signature) {
  if (!accessible(physical, 36))
    return NULL;
  const uint8_t *p = (const void *)(uintptr_t)physical;
  uint32_t length;
  memcpy(&length, p + 4, 4);
  if (length < 36 || length > 1024 * 1024 || !accessible(physical, length) ||
      __builtin_memcmp(p, signature, 4) || checksum(p, length))
    return NULL;
  return p;
}
void pci_ecam_init(uint32_t mbi, uint32_t end) {
  const uint8_t *rsdp = NULL;
  bool extended = false;
  for (uint32_t p = mbi + 8; p + 8 <= end;) {
    const uint8_t *t = (const void *)(uintptr_t)p;
    uint32_t type, size;
    memcpy(&type, t, 4);
    memcpy(&size, t + 4, 4);
    if (size < 8 || size > end - p || type == 0)
      break;
    if ((type == 14 || type == 15) && size >= 28 &&
        !__builtin_memcmp(t + 8, "RSD PTR ", 8) && !checksum(t + 8, 20)) {
      if (type == 15 && size >= 44 && !checksum(t + 8, 36)) {
        rsdp = t + 8;
        extended = true;
      } else if (!rsdp)
        rsdp = t + 8;
    }
    p += (size + 7) & ~7u;
  }
  if (!rsdp)
    return;
  uint64_t root_address = 0;
  unsigned entry_size = 4;
  if (extended && rsdp[15] >= 2) {
    memcpy(&root_address, rsdp + 24, 8);
    if (root_address)
      entry_size = 8;
  }
  if (!root_address)
    memcpy(&root_address, rsdp + 16, 4);
  const uint8_t *root = table(root_address, entry_size == 8 ? "XSDT" : "RSDT");
  if (!root)
    return;
  uint32_t length;
  memcpy(&length, root + 4, 4);
  for (uint32_t i = 36; i + entry_size <= length; i += entry_size) {
    uint64_t address = 0;
    memcpy(&address, root + i, entry_size);
    const uint8_t *mcfg = table(address, "MCFG");
    if (!mcfg)
      continue;
    uint32_t size;
    memcpy(&size, mcfg + 4, 4);
    for (uint32_t at = 44; at + 16 <= size; at += 16) {
      uint64_t base;
      uint16_t segment;
      memcpy(&base, mcfg + at, 8);
      memcpy(&segment, mcfg + at + 8, 2);
      unsigned first = mcfg[at + 10], last = mcfg[at + 11];
      if (segment || last < first || !base || base & ((1u << 20) - 1) ||
          !accessible(base + ((uint64_t)(last - first) << 20), 1u << 20))
        continue;
      for (unsigned bus = first; bus <= last; bus++) {
        uint64_t address = base + ((uint64_t)(bus - first) << 20);
        if (platform_mmio(address, 1u << 20))
          ecam[bus] = address;
      }
    }
    return;
  }
}
uint64_t pci_ecam_bus(unsigned bus) { return ecam[bus & 255]; }
