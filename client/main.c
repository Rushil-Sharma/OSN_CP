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

// Print client command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s <address> <port> -user <name>\n", program);
}

// Send a hello frame and print the acknowledged session id.
int main(int argc, char **argv)
{
	uint8_t payload[64];
	uint8_t *received = NULL;
	uint32_t length = 0;
	uint32_t session_id;
	size_t payload_len = 0;
	recv_status_t status;
	msg_type_t type;
	int port = 0;
	int fd;

	if (argc != 5 || strcmp(argv[3], "-user") != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (parse_port(argv[2], &port) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);
	fd = connect_tcp(argv[1], port);
	if (fd < 0) {
		return EXIT_FAILURE;
	}
	if (msg_encode_client_hello(payload, sizeof(payload), &payload_len, argv[4]) != 0 ||
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
