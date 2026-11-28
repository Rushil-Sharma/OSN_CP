#ifndef OSN_DISCOVERY_H
#define OSN_DISCOVERY_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "protocol.h"

// Define discovery defaults: UDP port, per-attempt timeout, and retry count.
#define DISCOVERY_DEFAULT_PORT 9999
#define DISCOVERY_TIMEOUT_MS 2000
#define DISCOVERY_ATTEMPTS 3

// Define the reply payload (IPv4 + TCP port) and the largest valid datagram.
#define DISCOVERY_REPLY_PAYLOAD 6u
#define DISCOVERY_DATAGRAM_CAP (OSN_HEADER_SIZE + DISCOVERY_REPLY_PAYLOAD)

// Hold a resolved scheduler TCP endpoint as dotted IPv4 text and port.
typedef struct {
	char address[INET_ADDRSTRLEN];
	int port;
} sched_endpoint_t;

// Encode a discovery request datagram (header only, zero-length payload).
void discovery_encode_request(uint8_t out[OSN_HEADER_SIZE]);

// Return 1 when a datagram is a well-formed discovery request, else 0.
int discovery_is_request(const uint8_t *buf, size_t len);

// Encode a discovery reply carrying a network-order IPv4 address and TCP port.
size_t discovery_encode_reply(uint8_t out[DISCOVERY_DATAGRAM_CAP], uint32_t ipv4_net, uint16_t tcp_port);

// Decode a discovery reply into a network-order IPv4 address and host-order port.
int discovery_decode_reply(const uint8_t *buf, size_t len, uint32_t *ipv4_net, uint16_t *tcp_port);

// Broadcast up to DISCOVERY_ATTEMPTS requests; return 0 and fill out on a reply.
int discover_scheduler(int discovery_port, log_source_t source, sched_endpoint_t *out);

// Run discovery, then fall back to the CLI address; return -1 if both are missing.
int discover_or_fallback(int discovery_port, const char *fallback_address, int fallback_port,
                         log_source_t source, sched_endpoint_t *out);

#endif
