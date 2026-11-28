// Expose getifaddrs() interface flags (IFF_*) on glibc alongside _POSIX_C_SOURCE.
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Encode a discovery request datagram (header only, zero-length payload).
void discovery_encode_request(uint8_t out[OSN_HEADER_SIZE])
{
	msg_header_encode(out, MSG_DISCOVER_REQ, 0);
}

// Return 1 when a datagram is a well-formed discovery request, else 0.
int discovery_is_request(const uint8_t *buf, size_t len)
{
	msg_type_t type;
	uint16_t version;
	uint32_t length;

	if (buf == NULL || len != OSN_HEADER_SIZE ||
	    msg_header_decode(buf, &type, &version, &length) != 0) {
		return 0;
	}
	return type == MSG_DISCOVER_REQ && version == OSN_PROTOCOL_VERSION && length == 0;
}

// Encode a discovery reply carrying a network-order IPv4 address and TCP port.
size_t discovery_encode_reply(uint8_t out[DISCOVERY_DATAGRAM_CAP], uint32_t ipv4_net, uint16_t tcp_port)
{
	uint16_t raw_port = htons(tcp_port);

	msg_header_encode(out, MSG_DISCOVER_REPLY, DISCOVERY_REPLY_PAYLOAD);
	memcpy(out + OSN_HEADER_SIZE, &ipv4_net, sizeof(ipv4_net));
	memcpy(out + OSN_HEADER_SIZE + 4, &raw_port, sizeof(raw_port));
	return DISCOVERY_DATAGRAM_CAP;
}

// Decode a discovery reply into a network-order IPv4 address and host-order port.
int discovery_decode_reply(const uint8_t *buf, size_t len, uint32_t *ipv4_net, uint16_t *tcp_port)
{
	msg_type_t type;
	uint16_t version;
	uint32_t length;
	uint16_t raw_port;

	if (buf == NULL || ipv4_net == NULL || tcp_port == NULL || len != DISCOVERY_DATAGRAM_CAP ||
	    msg_header_decode(buf, &type, &version, &length) != 0) {
		return -1;
	}
	if (type != MSG_DISCOVER_REPLY || version != OSN_PROTOCOL_VERSION ||
	    length != DISCOVERY_REPLY_PAYLOAD) {
		return -1;
	}
	memcpy(ipv4_net, buf + OSN_HEADER_SIZE, sizeof(*ipv4_net));
	memcpy(&raw_port, buf + OSN_HEADER_SIZE + 4, sizeof(raw_port));
	*tcp_port = ntohs(raw_port);
	return *tcp_port == 0 ? -1 : 0;
}

// Return monotonic time in milliseconds for timeout bookkeeping.
static long long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

// Send one request datagram to an IPv4 destination; return 0 on success.
static int send_request_to(int fd, uint32_t ipv4_net, int port, const uint8_t *req)
{
	struct sockaddr_in dest;

	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons((uint16_t)port);
	dest.sin_addr.s_addr = ipv4_net;
	return sendto(fd, req, OSN_HEADER_SIZE, 0, (struct sockaddr *)&dest, sizeof(dest)) ==
	       (ssize_t)OSN_HEADER_SIZE ? 0 : -1;
}

// Broadcast one request on the limited address, every interface broadcast, and loopback.
static int broadcast_request(int fd, int port)
{
	uint8_t req[OSN_HEADER_SIZE];
	struct ifaddrs *ifs = NULL;
	int sent = 0;

	discovery_encode_request(req);
	// The limited broadcast only leaves the default interface, so also hit each subnet.
	if (send_request_to(fd, htonl(INADDR_BROADCAST), port, req) == 0) {
		sent++;
	}
	if (getifaddrs(&ifs) == 0) {
		for (struct ifaddrs *it = ifs; it != NULL; it = it->ifa_next) {
			if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET ||
			    !(it->ifa_flags & IFF_UP) || !(it->ifa_flags & IFF_BROADCAST) ||
			    (it->ifa_flags & IFF_LOOPBACK) || it->ifa_broadaddr == NULL) {
				continue;
			}
			if (send_request_to(fd, ((struct sockaddr_in *)it->ifa_broadaddr)->sin_addr.s_addr,
			                    port, req) == 0) {
				sent++;
			}
		}
		freeifaddrs(ifs);
	}
	// Loopback covers a scheduler on this host when broadcast routing is unavailable.
	if (send_request_to(fd, htonl(INADDR_LOOPBACK), port, req) == 0) {
		sent++;
	}
	return sent > 0 ? 0 : -1;
}

// Wait up to timeout_ms for a valid reply and fill out from it; return 0 on success.
static int await_reply(int fd, int timeout_ms, sched_endpoint_t *out)
{
	long long deadline = now_ms() + timeout_ms;

	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
		long long remaining = deadline - now_ms();
		uint8_t buf[DISCOVERY_DATAGRAM_CAP + 1];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		uint32_t ipv4_net;
		uint16_t tcp_port;
		ssize_t n;
		int rc;

		if (remaining <= 0) {
			return -1;
		}
		rc = poll(&pfd, 1, (int)remaining);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (rc <= 0) {
			return -1;
		}
		n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			continue;
		}
		// Ignore stray or malformed datagrams and keep waiting for a real reply.
		if (discovery_decode_reply(buf, (size_t)n, &ipv4_net, &tcp_port) != 0) {
			continue;
		}
		// A zero address in the reply means "connect to whoever answered".
		if (ipv4_net == htonl(INADDR_ANY)) {
			ipv4_net = from.sin_addr.s_addr;
		}
		if (inet_ntop(AF_INET, &ipv4_net, out->address, sizeof(out->address)) == NULL) {
			continue;
		}
		out->port = tcp_port;
		return 0;
	}
}

// Broadcast up to DISCOVERY_ATTEMPTS requests; return 0 and fill out on a reply.
int discover_scheduler(int discovery_port, log_source_t source, sched_endpoint_t *out)
{
	int one = 1;
	int fd;

	if (out == NULL || discovery_port < 1 || discovery_port > 65535) {
		return -1;
	}
	log_line(source, -1, "Looking for scheduler via UDP discovery (port %d)...", discovery_port);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		log_line(source, -1, "Discovery socket failed: %s", strerror(errno));
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) != 0) {
		log_line(source, -1, "Discovery SO_BROADCAST failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	for (int attempt = 1; attempt <= DISCOVERY_ATTEMPTS; attempt++) {
		if (broadcast_request(fd, discovery_port) != 0) {
			log_line(source, -1, "Discovery attempt %d/%d: send failed: %s", attempt,
			         DISCOVERY_ATTEMPTS, strerror(errno));
		}
		if (await_reply(fd, DISCOVERY_TIMEOUT_MS, out) == 0) {
			close(fd);
			log_line(source, -1, "Scheduler found at %s:%d.", out->address, out->port);
			return 0;
		}
		log_line(source, -1, "Discovery attempt %d/%d: no reply within %ds.", attempt,
		         DISCOVERY_ATTEMPTS, DISCOVERY_TIMEOUT_MS / 1000);
	}
	close(fd);
	return -1;
}

// Run discovery, then fall back to the CLI address; return -1 if both are missing.
int discover_or_fallback(int discovery_port, const char *fallback_address, int fallback_port,
                         log_source_t source, sched_endpoint_t *out)
{
	if (out == NULL) {
		return -1;
	}
	if (discover_scheduler(discovery_port, source, out) == 0) {
		return 0;
	}
	if (fallback_address == NULL || fallback_port <= 0) {
		log_line(source, -1, "No discovery reply and no fallback address given.");
		return -1;
	}
	snprintf(out->address, sizeof(out->address), "%s", fallback_address);
	out->port = fallback_port;
	log_line(source, -1, "No discovery reply, using fallback %s:%d.", out->address, out->port);
	return 0;
}
