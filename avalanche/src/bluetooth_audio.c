#include "usb.h"

/* BR/EDR Basic Mode L2CAP and an AVDTP SBC source. The source accepts
 * elementary SBC streams, so media packets never need an encoder in kernel. */
#define SIGNAL_CID 1
#define AVDTP_PSM 0x19
#define LOCAL_MTU 672
#define TX_LIMIT 8
enum audio_phase {
  BT_AUDIO_IDLE, BT_AUDIO_SIGNAL, BT_AUDIO_DISCOVER, BT_AUDIO_CAPS,
  BT_AUDIO_CONFIGURE, BT_AUDIO_OPEN, BT_AUDIO_MEDIA, BT_AUDIO_START,
  BT_AUDIO_STREAM, BT_AUDIO_CLOSE, BT_AUDIO_ERROR
};
struct channel {
  unsigned local, remote, mtu, connect_id, config_id;
  bool local_configured, remote_configured;
};
static struct {
  struct channel signal, media;
  enum audio_phase phase;
  unsigned identifier, label, expected, seid;
  unsigned offset, size, frame_bytes, samples, rate, flags, bitpool;
  uint32_t timestamp, began, deadline, ssrc;
  uint16_t sequence;
  int error;
  char path[256];
} player;
static struct {
  unsigned size;
  uint8_t packet[LOCAL_MTU + 4];
} tx[TX_LIMIT];
static unsigned tx_head, tx_count;
static unsigned next_cid = 0x40;
static uint8_t rx[LOCAL_MTU + 4];
static unsigned rx_used, rx_length;

static unsigned get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void put16(uint8_t *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void big32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; i++) p[i] = value >> (24 - i * 8);
}
static void fail(int error) {
  player.error = error;
  player.phase = BT_AUDIO_ERROR;
  tx_count = rx_used = 0;
  usb_bluetooth_disconnect();
}
static bool enqueue_l2cap(unsigned cid, const void *data, unsigned size) {
  if (size > LOCAL_MTU || size + 4 > usb_bluetooth_acl_capacity() ||
      tx_count == TX_LIMIT)
    return false;
  unsigned at = (tx_head + tx_count++) % TX_LIMIT;
  put16(tx[at].packet, size);
  put16(tx[at].packet + 2, cid);
  memcpy(tx[at].packet + 4, data, size);
  tx[at].size = size + 4;
  return true;
}
static bool signal(unsigned code, unsigned id, const void *data, unsigned size) {
  uint8_t command[48];
  if (size > sizeof(command) - 4) return false;
  command[0] = code;
  command[1] = id;
  put16(command + 2, size);
  if (size) memcpy(command + 4, data, size);
  return enqueue_l2cap(SIGNAL_CID, command, size + 4);
}
static unsigned identifier(void) {
  player.identifier = player.identifier % 255 + 1;
  return player.identifier;
}
static unsigned allocate_cid(void) {
  unsigned cid = next_cid;
  next_cid = next_cid == 0xffff ? 0x40 : next_cid + 1;
  return cid;
}
static bool channel_connect(struct channel *channel, unsigned cid) {
  uint8_t request[4];
  *channel = (struct channel){.local = cid, .mtu = LOCAL_MTU,
                              .connect_id = identifier()};
  put16(request, AVDTP_PSM);
  put16(request + 2, cid);
  return signal(2, channel->connect_id, request, sizeof(request));
}
static bool channel_ready(const struct channel *channel) {
  return channel->remote && channel->local_configured && channel->remote_configured;
}
static struct channel *find_channel(unsigned local) {
  if (local && player.signal.local == local) return &player.signal;
  if (local && player.media.local == local) return &player.media;
  return NULL;
}
static void signaling_command(unsigned code, unsigned id, const uint8_t *data,
                              unsigned size) {
  if (!id) return;
  if (code == 3 && size == 8) {
    struct channel *channel = find_channel(get16(data + 2));
    if (!channel || id != channel->connect_id || channel->remote) return;
    unsigned result = get16(data + 4);
    if (result == 1) return;
    if (result || get16(data) < 0x40) { fail(-5); return; }
    channel->remote = get16(data);
    channel->config_id = identifier();
    uint8_t request[8] = {0};
    put16(request, channel->remote);
    request[4] = 1;
    request[5] = 2;
    put16(request + 6, LOCAL_MTU);
    if (!signal(4, channel->config_id, request, sizeof(request))) fail(-5);
  } else if (code == 4 && size >= 4) {
    struct channel *channel = find_channel(get16(data));
    if (!channel || !channel->remote) return;
    unsigned flags = get16(data + 2), result = flags ? 2 : 0;
    for (unsigned i = 4; i + 2 <= size;) {
      unsigned length = data[i + 1];
      if (length > size - i - 2) { result = 2; break; }
      if (data[i] == 1 && length == 2) {
        unsigned mtu = get16(data + i + 2);
        if (mtu < 48) result = 2;
        else channel->mtu = mtu;
      } else if (data[i] == 2 && length == 2) {
        /* Flush timeout governs the peer's outbound channel. */
      } else if (data[i] == 4 && length == 9 && !data[i + 2]) {
        /* Basic mode is the default on both sides. */
      } else if (!(data[i] & 128))
        result = 2;
      i += 2 + length;
    }
    uint8_t reply[6] = {0};
    put16(reply, channel->remote);
    put16(reply + 4, result);
    channel->remote_configured = !result;
    if (!signal(5, id, reply, sizeof(reply))) fail(-5);
  } else if (code == 5 && size >= 6) {
    struct channel *channel = find_channel(get16(data));
    if (!channel || id != channel->config_id) return;
    if (get16(data + 4) == 4) return;
    if (get16(data + 2) || get16(data + 4)) { fail(-95); return; }
    channel->local_configured = true;
  } else if (code == 2 && size == 4) {
    uint8_t reply[8] = {0};
    put16(reply + 2, get16(data + 2));
    put16(reply + 4, 2);
    (void)signal(3, id, reply, sizeof(reply));
  } else if (code == 6 && size == 4) {
    struct channel *channel = find_channel(get16(data));
    if (!channel || channel->remote != get16(data + 2)) return;
    (void)signal(7, id, data, size);
    channel->remote = 0;
    if (player.phase == BT_AUDIO_STREAM) fail(-19);
  } else if (code == 7 && size == 4) {
    struct channel *channel = find_channel(get16(data + 2));
    if (channel && channel->remote == get16(data)) channel->remote = 0;
  } else if (code == 10 && size == 2) {
    uint8_t reply[12] = {data[0], data[1]};
    unsigned kind = get16(data), length = 4;
    if (kind == 1) { put16(reply + 4, LOCAL_MTU); length = 6; }
    else if (kind == 2) length = 8;
    else put16(reply + 2, 1);
    (void)signal(11, id, reply, length);
  } else if (code == 8 && size <= 44)
    (void)signal(9, id, data, size);
}
static bool avdtp_command(unsigned command, const void *data, unsigned size) {
  uint8_t packet[24];
  if (!player.signal.remote || size > sizeof(packet) - 2) return false;
  player.label = (player.label + 1) & 15;
  player.expected = command;
  packet[0] = player.label << 4;
  packet[1] = command;
  if (size) memcpy(packet + 2, data, size);
  player.deadline = clock_ticks() + 500;
  return enqueue_l2cap(player.signal.remote, packet, size + 2);
}
static void sbc_configuration(uint8_t *config) {
  config[0] = (0x80u >> (player.flags >> 6)) |
              (8u >> ((player.flags >> 2) & 3));
  config[1] = (0x80u >> ((player.flags >> 4) & 3)) |
              ((player.flags & 1) ? 4u : 8u) |
              ((player.flags & 2) ? 2u : 1u);
}
static void close_channels(void) {
  struct channel *channels[] = {&player.media, &player.signal};
  for (unsigned i = 0; i < 2; i++)
    if (channels[i]->remote) {
      uint8_t request[4];
      put16(request, channels[i]->remote);
      put16(request + 2, channels[i]->local);
      (void)signal(6, identifier(), request, sizeof(request));
    }
  player.phase = BT_AUDIO_IDLE;
}
static void avdtp_receive(const uint8_t *data, unsigned size) {
  if (size < 2 || (data[0] & 12)) return;
  if (!(data[0] & 3)) {
    uint8_t reject[] = {(data[0] & 0xf0) | 1, data[1]};
    (void)enqueue_l2cap(player.signal.remote, reject, sizeof(reject));
    return;
  }
  if ((data[0] >> 4) != player.label || data[1] != player.expected) return;
  if ((data[0] & 3) != 2) { fail(-95); return; }
  unsigned command = player.expected;
  player.expected = 0;
  data += 2;
  size -= 2;
  if (command == 1 && player.phase == BT_AUDIO_DISCOVER) {
    unsigned seid = 0;
    for (unsigned i = 0; i + 1 < size; i += 2)
      if (!(data[i] & 2) && (data[i + 1] & 0xf8) == 8 &&
          (data[i] >> 2) && (data[i] >> 2) < 63) {
        seid = data[i] & 0xfc;
        break;
      }
    if (!seid) { fail(-19); return; }
    player.seid = seid;
    uint8_t endpoint = seid;
    player.phase = BT_AUDIO_CAPS;
    if (!avdtp_command(2, &endpoint, 1)) fail(-5);
  } else if (command == 2 && player.phase == BT_AUDIO_CAPS) {
    uint8_t cfg[2];
    sbc_configuration(cfg);
    bool transport = false, codec = false;
    for (unsigned i = 0; i + 2 <= size;) {
      unsigned category = data[i], length = data[i + 1];
      if (length > size - i - 2) { fail(-22); return; }
      const uint8_t *p = data + i + 2;
      if (category == 1 && !length) transport = true;
      if (category == 7 && length == 6 && !p[0] && !p[1] &&
          (p[2] & cfg[0]) == cfg[0] && (p[3] & cfg[1]) == cfg[1] &&
          player.bitpool >= p[4] && player.bitpool <= p[5]) codec = true;
      i += length + 2;
    }
    if (!transport || !codec) { fail(-95); return; }
    uint8_t set[] = {player.seid, 4, 1, 0, 7, 6, 0, 0, cfg[0], cfg[1],
                     player.bitpool, player.bitpool};
    player.phase = BT_AUDIO_CONFIGURE;
    if (!avdtp_command(3, set, sizeof(set))) fail(-5);
  } else if (command == 3 && player.phase == BT_AUDIO_CONFIGURE) {
    uint8_t endpoint = player.seid;
    player.phase = BT_AUDIO_OPEN;
    if (!avdtp_command(6, &endpoint, 1)) fail(-5);
  } else if (command == 6 && player.phase == BT_AUDIO_OPEN) {
    player.phase = BT_AUDIO_MEDIA;
    player.deadline = clock_ticks() + 500;
    if (!channel_connect(&player.media, allocate_cid())) fail(-5);
  } else if (command == 7 && player.phase == BT_AUDIO_START) {
    player.phase = BT_AUDIO_STREAM;
    player.began = clock_ticks();
    player.deadline = player.began + 200;
  } else if (command == 8 && player.phase == BT_AUDIO_CLOSE)
    close_channels();
}
void bluetooth_audio_receive(const uint8_t *packet, unsigned size) {
  if (size < 4) return;
  unsigned boundary = (packet[1] >> 4) & 3;
  packet += 4;
  size -= 4;
  if (boundary != 1) {
    rx_used = 0;
    if (size < 4) return;
    rx_length = get16(packet) + 4;
    if (rx_length > sizeof(rx)) { rx_length = 0; return; }
  } else if (!rx_used)
    return;
  if (size > rx_length - rx_used) { rx_used = 0; return; }
  memcpy(rx + rx_used, packet, size);
  rx_used += size;
  if (rx_used != rx_length) return;
  unsigned cid = get16(rx + 2);
  if (cid == SIGNAL_CID) {
    for (unsigned at = 4; at + 4 <= rx_length;) {
      unsigned length = get16(rx + at + 2);
      if (length > rx_length - at - 4) break;
      signaling_command(rx[at], rx[at + 1], rx + at + 4, length);
      at += length + 4;
    }
  } else if (cid == player.signal.local)
    avdtp_receive(rx + 4, rx_length - 4);
  rx_used = 0;
}
int bluetooth_audio_start(const char *path) {
  if (player.phase != BT_AUDIO_IDLE && player.phase != BT_AUDIO_ERROR) return -16;
  if (usb_bluetooth_acl_capacity() < 48) return -19;
  unsigned path_length = strlen(path);
  if (path_length >= sizeof(player.path)) return -36;
  int size = fs_size(path);
  uint8_t header[4];
  if (size < 4 || fs_read_at(path, 0, header, sizeof(header)) != 4 ||
      header[0] != 0x9c)
    return -22;
  unsigned flags = header[1], mode = (flags >> 2) & 3, bitpool = header[2];
  unsigned channels = mode ? 2 : 1, blocks = 4 * (((flags >> 4) & 3) + 1);
  unsigned bands = flags & 1 ? 8 : 4;
  if (bitpool < 2 || bitpool > 250 || bitpool > bands * (mode < 2 ? 16 : 32))
    return -22;
  unsigned bits = blocks * bitpool * (mode == 1 ? 2 : 1) +
                  (mode == 3 ? bands : 0);
  unsigned frame = 4 + bands * channels / 2 + (bits + 7) / 8;
  if ((unsigned)size % frame || frame + 17 > usb_bluetooth_acl_capacity())
    return -95;
  memset(&player, 0, sizeof(player));
  memcpy(player.path, path, path_length + 1);
  player.size = size;
  player.flags = flags;
  player.bitpool = bitpool;
  player.frame_bytes = frame;
  player.samples = blocks * bands;
  static const unsigned rates[] = {16000, 32000, 44100, 48000};
  player.rate = rates[flags >> 6];
  player.ssrc = clock_ticks() ^ ((uint32_t)size << 12);
  player.phase = BT_AUDIO_SIGNAL;
  player.deadline = clock_ticks() + 500;
  if (!channel_connect(&player.signal, allocate_cid())) { fail(-5); return -5; }
  return 0;
}
int bluetooth_audio_status(void) {
  if (player.phase == BT_AUDIO_ERROR) return player.error;
  return player.phase != BT_AUDIO_IDLE;
}
unsigned bluetooth_audio_bytes(void) { return player.offset; }
void bluetooth_audio_stop(void) {
  if (player.phase == BT_AUDIO_STREAM) {
    uint8_t endpoint = player.seid;
    player.phase = BT_AUDIO_CLOSE;
    if (!avdtp_command(8, &endpoint, 1)) fail(-5);
  } else if (player.phase != BT_AUDIO_IDLE && player.phase != BT_AUDIO_ERROR &&
             player.phase != BT_AUDIO_CLOSE)
    fail(-125);
}
static void media_packet(void) {
  if (tx_count) return;
  if (player.offset == player.size) {
    bluetooth_audio_stop();
    return;
  }
  unsigned mtu = usb_bluetooth_acl_capacity() - 4;
  if (mtu > player.media.mtu) mtu = player.media.mtu;
  if (mtu > LOCAL_MTU) mtu = LOCAL_MTU;
  if (mtu < 13 + player.frame_bytes) { fail(-95); return; }
  uint32_t now = clock_ticks();
  uint64_t wanted = (uint64_t)(now - player.began + 3) * player.rate / 100;
  if (player.timestamp >= wanted) return;
  unsigned frames = (wanted - player.timestamp) / player.samples;
  unsigned capacity = (mtu - 13) / player.frame_bytes;
  if (frames > capacity) frames = capacity;
  if (frames > 15) frames = 15;
  unsigned remaining = (player.size - player.offset) / player.frame_bytes;
  if (frames > remaining) frames = remaining;
  if (!frames) return;
  uint8_t media[LOCAL_MTU] = {0x80, 96};
  media[2] = player.sequence >> 8;
  media[3] = player.sequence;
  big32(media + 4, player.timestamp);
  big32(media + 8, player.ssrc);
  media[12] = frames;
  unsigned length = frames * player.frame_bytes;
  if (fs_read_at(player.path, player.offset, media + 13, length) != (int)length) {
    fail(-5);
    return;
  }
  for (unsigned i = 0; i < frames; i++) {
    uint8_t *frame = media + 13 + i * player.frame_bytes;
    if (frame[0] != 0x9c || frame[1] != player.flags || frame[2] != player.bitpool) {
      fail(-22);
      return;
    }
  }
  if (!enqueue_l2cap(player.media.remote, media, length + 13)) return;
  player.offset += length;
  player.timestamp += frames * player.samples;
  player.sequence++;
  player.deadline = now + 200;
}
void bluetooth_audio_poll(void) {
  if (!usb_bluetooth_acl_capacity()) {
    tx_count = rx_used = 0;
    if (bluetooth_audio_status() > 0) fail(-19);
    return;
  }
  if (bluetooth_audio_status() > 0 &&
      (int32_t)(clock_ticks() - player.deadline) >= 0) {
    fail(-110);
    return;
  }
  if (player.phase == BT_AUDIO_SIGNAL && channel_ready(&player.signal)) {
    player.phase = BT_AUDIO_DISCOVER;
    if (!avdtp_command(1, NULL, 0)) fail(-5);
  } else if (player.phase == BT_AUDIO_MEDIA && channel_ready(&player.media)) {
    uint8_t endpoint = player.seid;
    player.phase = BT_AUDIO_START;
    if (!avdtp_command(7, &endpoint, 1)) fail(-5);
  } else if (player.phase == BT_AUDIO_STREAM)
    media_packet();
  if (tx_count) {
    int result = usb_bluetooth_acl_send(tx[tx_head].packet, tx[tx_head].size);
    if (result < 0) fail(result);
    else if (result > 0) {
      tx_head = (tx_head + 1) % TX_LIMIT;
      tx_count--;
    }
  }
}
