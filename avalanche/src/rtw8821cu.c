#include "rtw8821cu.h"
#include "usb.h"

/* USB IDs shared by RTL8811CU and RTL8821CU devices. The vendor interface
 * check below keeps unrelated functions on composite devices unclaimed. */
bool rtw8821cu_usb_match(unsigned vendor, unsigned product) {
  static const struct {
    uint16_t vendor, product;
  } supported[] = {
      {0x0bda, 0x2006}, {0x0bda, 0x8731}, {0x0bda, 0x8811},
      {0x0bda, 0xb820}, {0x0bda, 0xb82b},
      {0x0bda, 0xc80c}, {0x0bda, 0xc811}, {0x0bda, 0xc820},
      {0x0bda, 0xc821}, {0x0bda, 0xc82a}, {0x0bda, 0xc82b},
      {0x2001, 0x331d}, {0x7392, 0xc811}, {0x7392, 0xd811},
      {0x2c4e, 0x0105},
  };
  for (unsigned i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
    if (supported[i].vendor == vendor && supported[i].product == product)
      return true;
  return false;
}
bool rtw8821cu_usb_parse(const uint8_t *data, unsigned length,
                         struct rtw8821cu_usb_config *out) {
  if (length < 9 || data[0] != 9 || data[1] != 2 || !data[5] ||
      (data[2] | ((unsigned)data[3] << 8)) != length)
    return false;
  struct rtw8821cu_usb_config config = {.configuration = data[5]};
  bool candidate = false, duplicate = false;
  unsigned seen = 0;
  for (unsigned offset = 9; offset < length;) {
    const uint8_t *entry = data + offset;
    if (length - offset < 2 || entry[0] < 2 || entry[0] > length - offset)
      return false;
    if (entry[1] == 4) {
      if (candidate && !duplicate && config.rx.number && config.tx_count) {
        *out = config;
        return true;
      }
      config.rx = (struct rtw8821cu_endpoint){0};
      config.tx_count = seen = 0;
      duplicate = false;
      candidate = entry[0] >= 9 && !entry[3] && entry[5] == 0xff &&
                  entry[6] == 0xff && entry[7] == 0xff;
    } else if (candidate && entry[1] == 5 && entry[0] >= 7 &&
               (entry[3] & 3) == 2) {
      unsigned endpoint = entry[2] & 15, packet = entry[4] | (entry[5] << 8);
      bool input = entry[2] & 0x80;
      unsigned bit = 1u << (endpoint + (input ? 16 : 0));
      if (!endpoint || (entry[2] & 0x70) || !packet || packet > 1024 ||
          (seen & bit)) {
        duplicate = true;
      } else if (input) {
        if (config.rx.number) duplicate = true;
        else config.rx = (struct rtw8821cu_endpoint){endpoint, packet};
      } else if (config.tx_count == RTW8821CU_TX_PIPES)
        duplicate = true;
      else
        config.tx[config.tx_count++] =
            (struct rtw8821cu_endpoint){endpoint, packet};
      seen |= bit;
    }
    offset += entry[0];
  }
  if (!candidate || duplicate || !config.rx.number || !config.tx_count)
    return false;
  *out = config;
  return true;
}

/* This state describes the USB transport only. It is deliberately not a
 * network interface until the MAC/PHY and station state machine are ready. */
static struct {
  unsigned slot, vendor, product;
  struct rtw8821cu_usb_config config;
  struct rtw8821cu_hw hw;
  uint32_t silicon;
  bool silicon_read;
} adapter[8];
static uint8_t register_data[4] __attribute__((aligned(64)));

static bool read_silicon(unsigned slot, uint32_t *out) {
  /* RTW88 vendor GET_REGISTER, SYS_CFG1. No radio register writes here. */
  memset(register_data, 0xff, sizeof(register_data));
  if (!xhci_control(slot, 0xc0, 0x05, 0x00f0, 0, register_data,
                    sizeof(register_data)))
    return false;
  uint32_t value = register_data[0] | ((uint32_t)register_data[1] << 8) |
                   ((uint32_t)register_data[2] << 16) |
                   ((uint32_t)register_data[3] << 24);
  if (value == UINT32_MAX) return false;
  *out = value;
  return true;
}
void rtw8821cu_usb_attach(unsigned slot, unsigned vendor, unsigned product,
                         const struct rtw8821cu_usb_config *config) {
  for (unsigned i = 0; i < sizeof(adapter) / sizeof(adapter[0]); i++) {
    if (adapter[i].slot) continue;
    adapter[i].slot = slot;
    adapter[i].vendor = vendor;
    adapter[i].product = product;
    adapter[i].config = *config;
    adapter[i].silicon_read = read_silicon(slot, &adapter[i].silicon);
    return;
  }
}
void rtw8821cu_usb_detach(unsigned slot) {
  for (unsigned i = 0; i < sizeof(adapter) / sizeof(adapter[0]); i++)
    if (adapter[i].slot == slot) memset(&adapter[i], 0, sizeof(adapter[i]));
}
void rtw8821cu_usb_info(void) {
  for (unsigned i = 0; i < sizeof(adapter) / sizeof(adapter[0]); i++) {
    if (!adapter[i].slot) continue;
    console_write("Realtek RTL8821CU family USB ");
    print_hex(adapter[i].vendor);
    console_putc(':');
    print_hex(adapter[i].product);
    console_write("; RX endpoint ");
    print_uint(adapter[i].config.rx.number);
    console_write("; TX queues ");
    print_uint(adapter[i].config.tx_count);
    if (adapter[i].silicon_read) {
      console_write("; silicon revision ");
      print_uint((adapter[i].silicon >> 12) & 15);
    } else
      console_write("; silicon register unavailable");
    if (adapter[i].hw.stage) {
      console_write("; ");
      console_write(adapter[i].hw.stage);
      if (adapter[i].hw.error) {
        console_write("; failed at register ");
        print_hex(adapter[i].hw.error_register);
      }
      if (adapter[i].hw.efuse_valid) {
        static const char hex[] = "0123456789abcdef";
        console_write("; factory MAC ");
        for (unsigned n = 0; n < 6; n++) {
          unsigned byte = adapter[i].hw.efuse[0x107 + n];
          if (n) console_putc(':');
          console_putc(hex[byte >> 4]);
          console_putc(hex[byte & 15]);
        }
        console_write("; RFE ");
        print_uint(adapter[i].hw.efuse[0xca] & 31);
        console_write("; channel plan ");
        print_hex(adapter[i].hw.efuse[0xb8]);
      }
      if (adapter[i].hw.firmware_running) {
        console_write("; firmware version ");
        print_uint(adapter[i].hw.firmware_version);
      }
    } else console_write("; hardware initialization not requested");
    console_putc('\n');
  }
}
void rtw8821cu_command(const char *operation) {
  while (*operation == ' ') operation++;
  bool start = str_eq(operation, "init"), stop = str_eq(operation, "off");
  if (!start && !stop && !str_eq(operation, "status") && *operation) {
    console_write("wifi status | wifi init | wifi off\n");
    return;
  }
  unsigned count = 0;
  for (unsigned i = 0; i < sizeof(adapter) / sizeof(adapter[0]); i++) {
    if (!adapter[i].slot) continue;
    count++;
    if (start) rtw8821cu_hw_start(adapter[i].slot, &adapter[i].config,
                                 &adapter[i].hw);
    if (stop) rtw8821cu_hw_stop(&adapter[i].hw);
  }
  if (!count) console_write("No RTL8811CU/RTL8821CU USB adapter attached.\n");
  else rtw8821cu_usb_info();
}
