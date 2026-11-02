#include "tls.h"
#include "../vendor/bearssl/bearssl.h"
#include "system.h"
#include "trust_anchors.h"
static br_ssl_client_context client;
static br_x509_minimal_context verifier;
static br_sslio_context stream;
static unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
static bool active;
static unsigned error;
static int read_tcp(void *ctx, unsigned char *data, size_t size) {
  (void)ctx;
  int n = net_tcp_recv(data, size, 1000);
  return n > 0 ? n : -1;
}
static int write_tcp(void *ctx, const unsigned char *data, size_t size) {
  (void)ctx;
  return net_tcp_send(data, size);
}
bool tls_connect(const char *hostname) {
  active = false;
  error = 0;
  uint32_t days, seconds;
  unsigned char seed[48];
  if (!rtc_time(&days, &seconds)) {
    error = 1001;
    return false;
  }
  if (!secure_random(seed, sizeof(seed))) {
    error = 1002;
    return false;
  }
  br_ssl_client_init_full(&client, &verifier, trust_anchors,
                          sizeof(trust_anchors) / sizeof(trust_anchors[0]));
  br_ssl_engine_set_versions(&client.eng, BR_TLS12, BR_TLS12);
  br_x509_minimal_set_time(&verifier, days, seconds);
  br_ssl_engine_set_buffer(&client.eng, buffer, sizeof(buffer), 1);
  br_ssl_engine_inject_entropy(&client.eng, seed, sizeof(seed));
  memset(seed, 0, sizeof(seed));
  if (!br_ssl_client_reset(&client, hostname, 0)) {
    error = br_ssl_engine_last_error(&client.eng);
    return false;
  }
  br_sslio_init(&stream, &client.eng, read_tcp, NULL, write_tcp, NULL);
  active = true;
  return true;
}
int tls_send(const void *data, unsigned n) {
  if (!active)
    return -1;
  if (br_sslio_write_all(&stream, data, n) < 0 || br_sslio_flush(&stream) < 0) {
    error = br_ssl_engine_last_error(&client.eng);
    active = false;
    return -1;
  }
  return n;
}
int tls_recv(void *data, unsigned n) {
  if (!active)
    return -1;
  int count = br_sslio_read(&stream, data, n);
  if (count < 0) {
    error = br_ssl_engine_last_error(&client.eng);
    active = false;
  }
  return count;
}
void tls_close(void) {
  if (active)
    br_sslio_close(&stream);
  active = false;
  net_tcp_close();
}
unsigned tls_error(void) { return error; }
