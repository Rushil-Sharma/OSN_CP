#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/discovery.h"
#include "../common/log.h"

// Hold the bound UDP socket and the TCP port advertised in replies.
typedef struct {
	int fd;
	uint16_t tcp_port;
} responder_ctx_t;

// Find the local IPv4 address the kernel would use to reach a requester.
static uint32_t local_address_for(const struct sockaddr_in *peer)
{
	struct sockaddr_in local;
	socklen_t len = sizeof(local);
	uint32_t result = htonl(INADDR_ANY);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		return result;
	}
	// connect() on UDP sends nothing; it only picks a route and source address.
	if (connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&local, &len) == 0) {
		result = local.sin_addr.s_addr;
	}
	close(fd);
	return result;
}

// Answer every valid discovery request with this scheduler's TCP address and port.
static void *responder_loop(void *arg)
{
	responder_ctx_t *ctx = arg;

	for (;;) {
		uint8_t buf[DISCOVERY_DATAGRAM_CAP];
		uint8_t reply[DISCOVERY_DATAGRAM_CAP];
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		char peer_text[INET_ADDRSTRLEN] = "unknown";
		size_t reply_len;
		ssize_t n;
		int ok;

		n = recvfrom(ctx->fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			log_sched("Discovery responder recvfrom failed: %s", strerror(errno));
			break;
		}
		(void)inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text));
		if (!discovery_is_request(buf, (size_t)n)) {
			log_discovery_outcome(0, peer_text, ntohs(peer.sin_port));
			continue;
		}
		reply_len = discovery_encode_reply(reply, local_address_for(&peer), ctx->tcp_port);
		ok = sendto(ctx->fd, reply, reply_len, 0, (struct sockaddr *)&peer, peer_len) ==
		     (ssize_t)reply_len;
		log_discovery_outcome(ok, peer_text, ntohs(peer.sin_port));
	}
	close(ctx->fd);
	free(ctx);
	return NULL;
}

// Bind the UDP discovery socket and start a detached responder thread.
int discovery_responder_start(int discovery_port, int tcp_port)
{
	struct sockaddr_in address;
	responder_ctx_t *ctx;
	pthread_t thread;
	int opt = 1;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("discovery socket");
		return -1;
	}
	// Allow an immediate scheduler restart without "address already in use".
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
		perror("discovery SO_REUSEADDR");
		close(fd);
		return -1;
	}
#ifdef SO_REUSEPORT
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) != 0) {
		perror("discovery SO_REUSEPORT");
		close(fd);
		return -1;
	}
#endif
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((uint16_t)discovery_port);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		perror("discovery bind");
		close(fd);
		return -1;
	}
	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL) {
		close(fd);
		return -1;
	}
	ctx->fd = fd;
	ctx->tcp_port = (uint16_t)tcp_port;
	if (pthread_create(&thread, NULL, responder_loop, ctx) != 0) {
		close(fd);
		free(ctx);
		return -1;
	}
	pthread_detach(thread);
	log_sched("Discovery responder listening on UDP port %d.", discovery_port);
	return 0;
}
