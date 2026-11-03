#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Resolve through Avalanche's DNS implementation instead of glibc's NSS
 * modules, which are shared libraries on the build host. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result) {
  if (!result)
    return EAI_FAIL;
  *result = NULL;
  if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
    return EAI_FAMILY;
  int type = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    return EAI_SOCKTYPE;
  unsigned port = 0;
  if (service) {
    if (!strcmp(service, "http"))
      port = 80;
    else if (!strcmp(service, "https"))
      port = 443;
    else {
      const char *p = service;
      if (!*p)
        return EAI_SERVICE;
      while (*p) {
        if (*p < '0' || *p > '9')
          return EAI_SERVICE;
        port = port * 10 + *p++ - '0';
        if (port > 65535)
          return EAI_SERVICE;
      }
    }
  }
  struct in_addr address;
  if (!node)
    address.s_addr = htonl(
        hints && (hints->ai_flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
  else if (inet_pton(AF_INET, node, &address) != 1) {
    if (hints && (hints->ai_flags & AI_NUMERICHOST))
      return EAI_NONAME;
    uint32_t ip;
    if (syscall(4100, node, &ip) < 0)
      return EAI_AGAIN;
    address.s_addr = htonl(ip);
  }
  struct addrinfo *entry =
      calloc(1, sizeof(*entry) + sizeof(struct sockaddr_in));
  if (!entry)
    return EAI_MEMORY;
  struct sockaddr_in *socket = (void *)(entry + 1);
  socket->sin_family = AF_INET;
  socket->sin_port = htons(port);
  socket->sin_addr = address;
  entry->ai_family = AF_INET;
  entry->ai_socktype = type;
  entry->ai_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
  entry->ai_addrlen = sizeof(*socket);
  entry->ai_addr = (struct sockaddr *)socket;
  if (hints && (hints->ai_flags & AI_CANONNAME) && node) {
    entry->ai_canonname = strdup(node);
    if (!entry->ai_canonname) {
      free(entry);
      return EAI_MEMORY;
    }
  }
  *result = entry;
  return 0;
}
void freeaddrinfo(struct addrinfo *entry) {
  while (entry) {
    struct addrinfo *next = entry->ai_next;
    free(entry->ai_canonname);
    free(entry);
    entry = next;
  }
}
