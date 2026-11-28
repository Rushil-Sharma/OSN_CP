#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/discovery.h"
#include "../common/log.h"
#include "../common/messages.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "../common/types.h"

// Parse a decimal port in the inclusive range 1 through 65535.
static int parse_port(const char *text, int *out)
{
	char *end;
	long value;

	if (text == NULL || out == NULL || *text == '\0') {
		return -1;
	}
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
		return -1;
	}
	*out = (int)value;
	return 0;
}

// Connect to the scheduler over IPv4 TCP.
static int connect_tcp(const char *address, int port)
{
	struct sockaddr_in server;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, address, &server.sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", address);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&server, sizeof(server)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

// Parse optional fallback address/port, -user, and -discovery-port in any order.
static int parse_client_args(int argc, char **argv, const char **address, int *port,
                             const char **user, int *discovery_port)
{
	int positional = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-user") == 0) {
			if (++i >= argc) {
				return -1;
			}
			*user = argv[i];
		} else if (strcmp(argv[i], "-discovery-port") == 0) {
			if (++i >= argc || parse_port(argv[i], discovery_port) != 0) {
				return -1;
			}
		} else if (argv[i][0] == '-') {
			return -1;
		} else if (positional == 0) {
			*address = argv[i];
			positional++;
		} else if (positional == 1) {
			if (parse_port(argv[i], port) != 0) {
				return -1;
			}
			positional++;
		} else {
			return -1;
		}
	}
	// The fallback address is optional, but an IP without a port is an error.
	return positional == 0 || positional == 2 ? 0 : -1;
}

// Print client command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [<scheduler_ip> <scheduler_port>] -user <name> "
	        "[-discovery-port N]\n", program);
}

// Discover the scheduler (CLI address as fallback), send hello, print the session id.
int main(int argc, char **argv)
{
	uint8_t payload[64];
	uint8_t *received = NULL;
	uint32_t length = 0;
	uint32_t session_id;
	size_t payload_len = 0;
	recv_status_t status;
	msg_type_t type;
	sched_endpoint_t endpoint;
	const char *address = NULL;
	const char *user = NULL;
	int discovery_port = DISCOVERY_DEFAULT_PORT;
	int port = 0;
	int fd;

	if (parse_client_args(argc, argv, &address, &port, &user, &discovery_port) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	// The interactive username prompt is TODO 17; until then -user is mandatory.
	if (user == NULL) {
		fprintf(stderr, "error: -user <name> is required (username prompt not implemented yet)\n");
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);
	if (discover_or_fallback(discovery_port, address, port, LOG_CLIENT, &endpoint) != 0) {
		fprintf(stderr, "error: no scheduler found via discovery and no fallback "
		        "<scheduler_ip> <scheduler_port> given\n");
		return EXIT_FAILURE;
	}
	fd = connect_tcp(endpoint.address, endpoint.port);
	if (fd < 0) {
		return EXIT_FAILURE;
	}
	log_line(LOG_CLIENT, -1, "Connected to Scheduler.");
	if (msg_encode_client_hello(payload, sizeof(payload), &payload_len, user) != 0 ||
	    send_msg(fd, MSG_CLIENT_HELLO, payload, (uint32_t)payload_len) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}
	status = recv_msg(fd, &type, &received, &length);
	if (status != RECV_OK || type != MSG_HELLO_ACK ||
	    msg_decode_hello_ack(received, length, &session_id) != 0) {
		msg_payload_free(received);
		close(fd);
		return EXIT_FAILURE;
	}
	printf("HELLO_ACK session %u\n", session_id);
	msg_payload_free(received);
	close(fd);
	return EXIT_SUCCESS;
}
