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

#include "../common/log.h"
#include "../common/messages.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "../common/types.h"

// Define placeholder resource values until milestone-one clamping exists.
#define DEFAULT_WORKER_CORES 1u
#define DEFAULT_WORKER_RAM_MB 512u
#define DEFAULT_WORKER_GPU 0u

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

// Fill worker resource fields with placeholder values for the registration skeleton.
static void m1_worker_resources_stub(worker_t *worker)
{
	memset(worker, 0, sizeof(*worker));
	worker->id = -1;
	worker->fd = -1;
	worker->state = WORKER_ALIVE;
	worker->total_cores = DEFAULT_WORKER_CORES;
	worker->free_cores = DEFAULT_WORKER_CORES;
	worker->total_ram_mb = DEFAULT_WORKER_RAM_MB;
	worker->free_ram_mb = DEFAULT_WORKER_RAM_MB;
	worker->total_gpu = DEFAULT_WORKER_GPU;
	worker->free_gpu = DEFAULT_WORKER_GPU;
}

// Print worker command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s <address> <port>\n", program);
}

// Register with the scheduler and print the assigned worker id.
int main(int argc, char **argv)
{
	worker_t worker;
	uint8_t payload[64];
	uint8_t *received = NULL;
	uint32_t length = 0;
	int32_t worker_id;
	size_t payload_len = 0;
	recv_status_t status;
	msg_type_t type;
	int port = 0;
	int fd;

	if (argc != 3) {
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
	m1_worker_resources_stub(&worker);
	log_worker(-1, "registering with %s:%d", argv[1], port);
	if (msg_encode_worker_register(payload, sizeof(payload), &payload_len, &worker) != 0 ||
	    send_msg(fd, MSG_WORKER_REGISTER, payload, (uint32_t)payload_len) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}
	status = recv_msg(fd, &type, &received, &length);
	if (status != RECV_OK || type != MSG_WORKER_REGISTER_ACK ||
	    msg_decode_worker_register_ack(received, length, &worker_id) != 0) {
		msg_payload_free(received);
		close(fd);
		return EXIT_FAILURE;
	}
	log_worker(worker_id, "registration acknowledged");
	printf("WORKER_REGISTER_ACK worker %d\n", worker_id);
	msg_payload_free(received);
	close(fd);
	return EXIT_SUCCESS;
}
