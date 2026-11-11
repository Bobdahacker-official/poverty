/* SPDX-License-Identifier: BSD-3-Clause
 * Hardware sequences adapted from rtw88, Copyright(c) 2018-2019 Realtek
 * Corporation. See vendor/rtw88/LICENSE and vendor/rtw88/UPSTREAM.md.
 */
#include "rtw8821cu.h"
#include "usb.h"

#define BIT(n) (UINT32_C(1) << (n))
#define TX_DESC 48u
#define FW_HEADER 64u
#define EFUSE_USABLE (512u - 96u)
extern const uint8_t rtw8821c_firmware[], rtw8821c_firmware_end[];
static uint64_t ticks_per_us;
static uint8_t control_data[4] __attribute__((aligned(64)));
static uint8_t tx_data[4096] __attribute__((aligned(64)));

static uint32_t le32(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; i++) p[i] = value >> (8 * i);
}
static uint64_t timestamp(void) {
  uint32_t lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}
static bool calibrate_delay(void) {
  if (ticks_per_us) return true;
  /* PIT channel 2, as in the native rge driver; no assumed CPU frequency. */
  uint8_t saved = io_in8(0x61);
  io_out8(0x61, saved & ~3u);
  io_out8(0x43, 0xb0);
  io_out8(0x42, 11932 & 255);
  io_out8(0x42, 11932 >> 8);
  uint64_t begin = timestamp();
  io_out8(0x61, (saved & ~2u) | 1u);
  unsigned count = 0;
  while (!(io_in8(0x61) & 0x20) && ++count < 10000000) cpu_pause();
  uint64_t elapsed = timestamp() - begin;
  io_out8(0x61, saved);
  if (count >= 10000000 || elapsed < 10000) return false;
  ticks_per_us = (elapsed + 9999) / 10000;
  return true;
}
static void delay_us(unsigned us) {
  uint64_t begin = timestamp(), duration = ticks_per_us * us;
  while (timestamp() - begin < duration) cpu_pause();
}
static bool fail(struct rtw8821cu_hw *h, unsigned reg) {
  if (!h->error) h->error_register = reg;
  h->error = true;
  return false;
}
static bool read_reg(struct rtw8821cu_hw *h, unsigned reg, unsigned width,
                     uint32_t *value) {
  if (h->error) return false;
  memset(control_data, 0, sizeof(control_data));
  if (!xhci_control(h->slot, 0xc0, 5, reg, 0, control_data, width))
    return fail(h, reg);
  *value = le32(control_data);
  return true;
}
static bool write_reg(struct rtw8821cu_hw *h, unsigned reg, unsigned width,
                      uint32_t value) {
  if (h->error) return false;
  put32(control_data, value);
  return xhci_control(h->slot, 0x40, 5, reg, 0, control_data, width) ||
         fail(h, reg);
}
static bool update(struct rtw8821cu_hw *h, unsigned reg, unsigned width,
                   uint32_t mask, uint32_t value) {
  uint32_t old;
  return read_reg(h, reg, width, &old) &&
         write_reg(h, reg, width, (old & ~mask) | (value & mask));
}
static bool poll_reg(struct rtw8821cu_hw *h, unsigned reg, unsigned width,
                     uint32_t mask, uint32_t target, unsigned timeout_us) {
  uint64_t begin = timestamp();
  for (unsigned attempts = 0; attempts < 10000; attempts++) {
    uint32_t value;
    if (!read_reg(h, reg, width, &value)) return false;
    if ((value & mask) == target) return true;
    if (timestamp() - begin >= ticks_per_us * timeout_us) break;
    delay_us(10);
  }
  return fail(h, reg);
}

struct power_step { uint16_t reg; uint8_t mask, value, poll; };
static bool power_sequence(struct rtw8821cu_hw *h,
                           const struct power_step *steps, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const struct power_step *s = &steps[i];
    if (s->poll) {
      if (!poll_reg(h, s->reg, 1, s->mask, s->value, 100000)) return false;
    } else if (!update(h, s->reg, 1, s->mask, s->value)) return false;
    if (s->reg == 0x20 && s->mask == 1 && s->value == 1) delay_us(1000);
  }
  return true;
}
#define SEQUENCE(h, name) power_sequence(h, name, sizeof(name) / sizeof(name[0]))

bool rtw8821cu_hw_stop(struct rtw8821cu_hw *h) {
  if (!h->slot || !h->powered) return true;
  static const struct power_step steps[] = {
      {0x93, 8, 0, 0}, {0x1f, 255, 0, 0}, {0x49, 2, 0, 0},
      {0x06, 1, 1, 0}, {0x02, 2, 0, 0}, {0x10c3, 1, 0, 0},
      {0x05, 2, 2, 0}, {0x05, 2, 0, 1}, {0x20, 8, 0, 0},
      {0x00, 32, 32, 0}, {0x07, 255, 32, 0}, {0x67, 32, 0, 0},
      {0x4a, 1, 0, 0}, {0x81, 192, 0, 0}, {0x05, 24, 8, 0},
      {0x90, 2, 0, 0},
  };
  h->error = false;
  h->stage = "power off";
  if (!update(h, 0x03, 1, 4, 0)) return false;
  h->firmware_running = false;
  if (!update(h, 0x1d, 1, 1, 0) || !SEQUENCE(h, steps)) return false;
  h->powered = h->firmware_running = false;
  h->stage = "powered off";
  return true;
}
static bool power_on(struct rtw8821cu_hw *h) {
  static const struct power_step steps[] = {
      {0x4a, 1, 0, 0}, {0x05, 152, 0, 0}, {0x20, 1, 1, 0},
      {0x00, 32, 0, 0}, {0x05, 28, 0, 0}, {0x06, 2, 2, 1},
      {0x06, 1, 1, 0}, {0x05, 128, 0, 0}, {0x05, 24, 0, 0},
      {0x10c3, 1, 1, 0}, {0x05, 1, 1, 0}, {0x05, 1, 0, 1},
      {0x20, 8, 8, 0}, {0x7c, 2, 0, 0},
  };
  h->stage = "MAC power on";
  uint32_t fw, rpwm, cr, status;
  if (!read_reg(h, 0x80, 2, &fw)) return false;
  if (fw == 0xc078 &&
      (!read_reg(h, 0xfe58, 1, &rpwm) ||
       !write_reg(h, 0xfe58, 1, (rpwm ^ 128) & 128))) return false;
  if (!read_reg(h, 0x100, 1, &cr) || !read_reg(h, 0xf5, 1, &status))
    return false;
  if (cr != 0xea && !(status & 1)) {
    h->powered = true;
    if (!rtw8821cu_hw_stop(h)) return false;
    h->stage = "MAC power on";
  }
  if (!write_reg(h, 0x1c, 1, 0) ||
      !update(h, 0x64, 4, BIT(29) | BIT(28), BIT(29) | BIT(28)) ||
      !update(h, 0x4c, 4, BIT(26) | BIT(25), 0) ||
      !update(h, 0x40, 4, BIT(2), BIT(2)) ||
      !update(h, 0x02, 1, 3, 0) || !update(h, 0x1f, 1, 7, 0) ||
      !update(h, 0xec, 4, BIT(24) | BIT(25) | BIT(26), 0)) return false;
  h->powered = true; /* Power-off remains available after a partial sequence. */
  if (!SEQUENCE(h, steps) || !update(h, 0xf5, 1, 1, 0) ||
      !update(h, 0x1080, 4, BIT(16) | BIT(8), BIT(16) | BIT(8)) ||
      !update(h, 0x03, 1, 0xd8, 0xd8) ||
      !update(h, 0x1103, 1, 15, 12) || !read_reg(h, 0x80, 4, &fw))
    return false;
  if ((fw & BIT(20)) &&
      (!update(h, 0x80, 4, BIT(20), 0) ||
       !update(h, 0x40, 4, BIT(19), 0))) return false;
  return true;
}

static bool read_efuse(struct rtw8821cu_hw *h) {
  uint8_t physical[512];
  uint32_t ldo, ctl;
  h->stage = "read factory calibration";
  /* Clear the programming supply. This routine never sets EF_FLAG or LDO25. */
  if (!read_reg(h, 0x34, 4, &ldo) ||
      !update(h, 0x34, 4, BIT(8) | BIT(9) | BIT(31), 0) ||
      !read_reg(h, 0x30, 4, &ctl)) return false;
  for (unsigned address = 0; address < sizeof(physical); address++) {
    uint32_t request = (ctl & ~(UINT32_C(0x3ffff) | BIT(31))) |
                       (address << 8);
    if (!write_reg(h, 0x30, 4, request) ||
        !poll_reg(h, 0x30, 4, BIT(31), BIT(31), 100000) ||
        !read_reg(h, 0x30, 4, &ctl)) return false;
    physical[address] = ctl;
  }
  /* Retain the disabled programming supply while restoring bank selection. */
  if (!update(h, 0x34, 4, BIT(8) | BIT(9), ldo)) return false;
  memset(h->efuse, 255, sizeof(h->efuse));
  for (unsigned p = 0; p < EFUSE_USABLE;) {
    unsigned head = physical[p++];
    if (head == 255) break;
    unsigned block = head >> 4, words = head & 15;
    if ((head & 31) == 15) {
      if (p >= EFUSE_USABLE) return fail(h, 0x30);
      unsigned extension = physical[p++];
      if (extension == 255) break;
      block = ((extension & 240) >> 1) | (head >> 5);
      words = extension & 15;
    }
    for (unsigned word = 0; word < 4; word++) {
      if (words & (1u << word)) continue;
      unsigned logical = block * 8 + word * 2;
      if (p + 2 > EFUSE_USABLE || logical + 2 > sizeof(h->efuse))
        return fail(h, 0x30);
      h->efuse[logical] = physical[p++];
      h->efuse[logical + 1] = physical[p++];
    }
  }
  /* USB MAC is at 0x107; PCIe and SDIO have different offsets. */
  const uint8_t *mac = h->efuse + 0x107;
  unsigned any = 0;
  for (unsigned i = 0; i < 6; i++) any |= mac[i];
  if ((mac[0] & 1) || !any || h->efuse[0] != 0x29 ||
      h->efuse[1] != 0x81) return fail(h, 0x30);
  h->efuse_valid = true;
  return true;
}

static bool firmware_packet(struct rtw8821cu_hw *h, unsigned endpoint,
                            const uint8_t *data, unsigned size) {
  uint32_t saved_cr, saved_beacon, saved_head;
  if (!read_reg(h, 0x101, 1, &saved_cr) ||
      !read_reg(h, 0x550, 1, &saved_beacon) ||
      !read_reg(h, 0x204, 2, &saved_head)) return false;
  unsigned payload = size + (((size + TX_DESC) & 511) == 0);
  memset(tx_data, 0, TX_DESC + payload);
  put32(tx_data, payload | (TX_DESC << 16) | BIT(26));
  put32(tx_data + 4, 16 << 8); /* Beacon queue, mapped to high-priority EP. */
  uint16_t checksum = 0;
  for (unsigned i = 0; i < 32; i += 2)
    checksum ^= tx_data[i] | ((uint16_t)tx_data[i + 1] << 8);
  tx_data[28] = checksum;
  tx_data[29] = checksum >> 8;
  memcpy(tx_data + TX_DESC, data, size);
  bool ok = write_reg(h, 0x204, 2, BIT(15)) &&
            write_reg(h, 0x101, 1, saved_cr | 1) &&
            write_reg(h, 0x550, 1, (saved_beacon & ~8u) | 16);
  if (ok && xhci_bulk(h->slot, endpoint, tx_data, TX_DESC + payload, false) !=
                (int)(TX_DESC + payload)) ok = fail(h, 0x204);
  if (ok) ok = poll_reg(h, 0x204, 2, BIT(15), BIT(15), 100000);
  bool previous_error = h->error;
  uint16_t previous_register = h->error_register;
  h->error = false;
  /* Attempt every restoration even after a transport failure. */
  bool restored = write_reg(h, 0x204, 2, (saved_head & 0xfff) | BIT(15));
  h->error = false;
  restored = write_reg(h, 0x550, 1, saved_beacon) && restored;
  h->error = false;
  restored = write_reg(h, 0x101, 1, saved_cr) && restored;
  if (previous_error) {
    h->error = true;
    h->error_register = previous_register;
  } else if (!restored) fail(h, 0x204);
  return ok && restored;
}
static bool firmware_section(struct rtw8821cu_hw *h, unsigned endpoint,
                             const uint8_t *data, uint32_t destination,
                             unsigned size) {
  if (!update(h, 0x1208, 4, BIT(25), BIT(25))) return false;
  for (unsigned offset = 0; offset < size;) {
    /* Leave space for the USB TX descriptor and any short-packet padding. */
    unsigned length = size - offset;
    if (length > 4000) length = 4000;
    if (!firmware_packet(h, endpoint, data + offset, length) ||
        !poll_reg(h, 0x1208, 4, BIT(31), 0, 100000) ||
        !write_reg(h, 0x1200, 4, UINT32_C(0x18780000) + TX_DESC) ||
        !write_reg(h, 0x1204, 4, destination + offset) ||
        !write_reg(h, 0x1208, 4, BIT(31) | BIT(29) |
                    (offset ? BIT(24) : 0) | length) ||
        !poll_reg(h, 0x1208, 4, BIT(31), 0, 100000)) return false;
    offset += length;
  }
  uint32_t checksum;
  if (!read_reg(h, 0x1208, 4, &checksum) || (checksum & BIT(27)))
    return fail(h, 0x1208);
  unsigned bits = destination < 0x200000 ? 0x18 : 0x60;
  return update(h, 0x80, 1, bits, bits);
}

struct backup { uint16_t reg; uint8_t width; uint32_t value; };
static bool download_firmware(struct rtw8821cu_hw *h, unsigned endpoint) {
  const uint8_t *fw = rtw8821c_firmware;
  size_t size = rtw8821c_firmware_end - fw;
  h->stage = "validate firmware image";
  if (size < FW_HEADER || size > 512 * 1024 ||
      (fw[0] | ((unsigned)fw[1] << 8)) != 0x8821) return fail(h, 0x80);
  uint32_t lengths[3] = {le32(fw + 0x24), le32(fw + 0x30),
                         (fw[0x18] & 16) ? le32(fw + 0x34) : 0};
  uint32_t addresses[3] = {le32(fw + 0x20) & ~BIT(31),
                          le32(fw + 0x3c) & ~BIT(31),
                          le32(fw + 0x38) & ~BIT(31)};
  size_t total = FW_HEADER;
  for (unsigned i = 0; i < 3; i++) {
    if (lengths[i] > size || (i < 2 && !lengths[i])) return fail(h, 0x80);
    if (i < 2 || lengths[i]) lengths[i] += 8;
    if (addresses[i] > UINT32_MAX - lengths[i]) return fail(h, 0x80);
    total += lengths[i];
  }
  if (total != size) return fail(h, 0x80);
  h->firmware_version = fw[4] | ((unsigned)fw[5] << 8);
  struct backup saved[] = {{0x10d, 1, 0}, {0x100, 1, 0},
      {0x1330, 4, 0}, {0x230, 2, 0}, {0x22c, 4, 0}, {0x550, 1, 0}};
  uint32_t coex;
  h->stage = "prepare firmware download";
  for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); i++)
    if (!read_reg(h, saved[i].reg, saved[i].width, &saved[i].value))
      return false;
  if (!poll_reg(h, 0x1700, 4, BIT(29), BIT(29), 100000) ||
      !write_reg(h, 0x1700, 4, 0x800f0038) ||
      !read_reg(h, 0x1708, 4, &coex)) return false;
  bool ok = update(h, 0x03, 1, 4, 0) && update(h, 0x1d, 1, 1, 0) &&
      write_reg(h, 0x10d, 1, 0xc0) && write_reg(h, 0x100, 1, 9) &&
      write_reg(h, 0x1330, 4, BIT(31)) && write_reg(h, 0x230, 2, 0x200) &&
      write_reg(h, 0x22c, 4, saved[4].value | BIT(31)) &&
      write_reg(h, 0x550, 1, (saved[5].value & ~8u) | 16) &&
      update(h, 0x1082, 1, 1, 0) && update(h, 0x09, 1, 64, 0) &&
      update(h, 0x1082, 1, 1, 1) && update(h, 0x09, 1, 64, 64) &&
      update(h, 0x80, 2, 0xc7ff, 1);
  size_t offset = FW_HEADER;
  h->stage = "download firmware sections";
  for (unsigned i = 0; ok && i < 3; i++) {
    if (lengths[i]) ok = firmware_section(h, endpoint, fw + offset,
                                          addresses[i], lengths[i]);
    offset += lengths[i];
  }
  bool previous_error = h->error;
  uint16_t previous_register = h->error_register;
  bool restored = true;
  for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); i++) {
    h->error = false;
    /* H2CQ_FULL and LD_RQPN are write-one triggers, not saved status. */
    uint32_t value = i == 2 ? BIT(31) : saved[i].value;
    if (i == 4) value |= BIT(31);
    if (!write_reg(h, saved[i].reg, saved[i].width, value)) restored = false;
  }
  h->error = previous_error;
  h->error_register = previous_register;
  if (!ok || !restored) return fail(h, previous_error ? previous_register : 0x100);
  h->stage = "start firmware CPU";
  uint32_t status;
  if (!write_reg(h, 0x210, 4, 4) || !read_reg(h, 0x80, 2, &status) ||
      (status & 0x50) != 0x50) return fail(h, 0x80);
  if (!write_reg(h, 0x80, 2, (status | BIT(14)) & ~1u) ||
      !update(h, 0x1d, 1, 1, 1) || !update(h, 0x03, 1, 4, 4) ||
      !poll_reg(h, 0x1700, 4, BIT(29), BIT(29), 100000) ||
      !write_reg(h, 0x1704, 4, coex) ||
      !write_reg(h, 0x1700, 4, 0xc00f0038) ||
      !poll_reg(h, 0x80, 2, 0xcfff, 0xc078, 100000)) return false;
  h->firmware_running = true;
  h->stage = "firmware running; RF and station setup pending";
  return true;
}

bool rtw8821cu_hw_start(unsigned slot, const struct rtw8821cu_usb_config *config,
                        struct rtw8821cu_hw *h) {
  if (h->powered && !rtw8821cu_hw_stop(h)) return false;
  memset(h, 0, sizeof(*h));
  h->slot = slot;
  h->stage = "calibrate register timing";
  if (!calibrate_delay()) return fail(h, 0);
  if (!config->tx_count || !config->tx[0].number) return fail(h, 0);
  if (power_on(h) && read_efuse(h) &&
      download_firmware(h, config->tx[0].number)) return true;
  const char *stage = h->stage;
  uint16_t reg = h->error_register;
  rtw8821cu_hw_stop(h);
  h->stage = stage;
  h->error = true;
  h->error_register = reg;
  return false;
}
