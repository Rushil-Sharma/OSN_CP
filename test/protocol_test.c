#include "protocol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void test_header(void)
{
	uint8_t header[OSN_HEADER_SIZE];
	msg_type_t type;
	uint16_t version;
	uint32_t length;

	msg_header_encode(header, MSG_JOB_ASSIGN, 0x01020304u);
	assert(msg_header_decode(header, &type, &version, &length) == 0);
	assert(type == MSG_JOB_ASSIGN);
	assert(version == OSN_PROTOCOL_VERSION);
	assert(length == 0x01020304u);
	assert(strcmp(msg_type_name(MSG_JOB_ASSIGN), "JOB_ASSIGN") == 0);
}

static void test_round_trip(void)
{
	int fds[2];
	const char payload[] = "wire frame";
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	assert(send_msg(fds[0], MSG_CLIENT_HELLO, payload, sizeof(payload)) == 0);
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_OK);
	assert(type == MSG_CLIENT_HELLO);
	assert(length == sizeof(payload));
	assert(memcmp(received, payload, sizeof(payload)) == 0);
	msg_payload_free(received);

	close(fds[0]);
	close(fds[1]);
}

static void test_empty_payload(void)
{
	int fds[2];
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 1;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	assert(send_msg(fds[0], MSG_NODES_REQ, NULL, 0) == 0);
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_OK);
	assert(type == MSG_NODES_REQ);
	assert(length == 0);
	assert(received != NULL);
	msg_payload_free(received);

	close(fds[0]);
	close(fds[1]);
}

static void test_clean_eof(void)
{
	int fds[2];
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	close(fds[0]);
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_EOF);
	assert(received == NULL);
	close(fds[1]);
}

static void test_bad_version(void)
{
	int fds[2];
	uint8_t header[OSN_HEADER_SIZE];
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	msg_header_encode(header, MSG_STATUS_REQ, 0);
	header[2] = 0;
	header[3] = 0;
	assert(write(fds[0], header, sizeof(header)) == (ssize_t)sizeof(header));
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_ERR);
	assert(received == NULL);

	close(fds[0]);
	close(fds[1]);
}

static void test_oversize_length(void)
{
	int fds[2];
	uint8_t header[OSN_HEADER_SIZE];
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	msg_header_encode(header, MSG_OUTPUT_CHUNK, OSN_MAX_PAYLOAD + 1u);
	assert(write(fds[0], header, sizeof(header)) == (ssize_t)sizeof(header));
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_ERR);
	assert(received == NULL);

	close(fds[0]);
	close(fds[1]);
}

static void test_partial_header(void)
{
	int fds[2];
	char byte = 'x';
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	assert(write(fds[0], &byte, 1) == 1);
	close(fds[0]);
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_ERR);
	assert(received == NULL);
	close(fds[1]);
}

int main(void)
{
	test_header();
	test_round_trip();
	test_empty_payload();
	test_clean_eof();
	test_bad_version();
	test_oversize_length();
	test_partial_header();
	return 0;
}
