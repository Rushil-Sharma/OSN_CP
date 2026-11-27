#include "messages.h"
#include "protocol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "types.h"

// Verify header encoding and decoding with a nonzero length.
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

// Verify a complete framed message over a socket pair.
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

// Verify that a zero-length payload is still a valid frame.
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

// Verify that a closed peer before any header returns a clean EOF.
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

// Verify that an unsupported protocol version is rejected.
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

// Verify that an oversize declared payload length is rejected.
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

// Verify that a partial header followed by EOF is rejected.
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

// Verify that a payload shorter than its declared length is rejected.
static void test_truncated_payload(void)
{
	int fds[2];
	uint8_t header[OSN_HEADER_SIZE];
	char byte = 'x';
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	msg_header_encode(header, MSG_OUTPUT_CHUNK, 8);
	assert(write(fds[0], header, sizeof(header)) == (ssize_t)sizeof(header));
	assert(write(fds[0], &byte, 1) == 1);
	close(fds[0]);
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_ERR);
	assert(received == NULL);
	close(fds[1]);
}

// Verify that an out-of-range message type is rejected.
static void test_invalid_type(void)
{
	int fds[2];
	uint8_t header[OSN_HEADER_SIZE];
	msg_type_t type;
	uint8_t *received = NULL;
	uint32_t length = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	msg_header_encode(header, MSG_COUNT, 0);
	assert(write(fds[0], header, sizeof(header)) == (ssize_t)sizeof(header));
	assert(recv_msg(fds[1], &type, &received, &length) == RECV_ERR);
	assert(received == NULL);
	close(fds[0]);
	close(fds[1]);
}

// Verify that the M0 payload helpers round-trip their fields.
static void test_message_helpers(void)
{
	uint8_t buffer[128];
	char user[MAX_USERNAME];
	char text[64];
	worker_t worker;
	worker_t decoded_worker;
	uint32_t session_id;
	uint16_t code = 0;
	int32_t worker_id;
	size_t length = 0;

	// Check hello payload fields.
	assert(msg_encode_client_hello(buffer, sizeof(buffer), &length, "alice") == 0);
	assert(msg_decode_client_hello(buffer, (uint32_t)length, user) == 0);
	assert(strcmp(user, "alice") == 0);

	// Check hello acknowledgement fields.
	assert(msg_encode_hello_ack(buffer, sizeof(buffer), &length, 42) == 0);
	assert(msg_decode_hello_ack(buffer, (uint32_t)length, &session_id) == 0);
	assert(session_id == 42);

	// Check worker register payload fields.
	memset(&worker, 0, sizeof(worker));
	worker.total_cores = 4;
	worker.total_ram_mb = 1024;
	worker.total_gpu = 1;
	assert(msg_encode_worker_register(buffer, sizeof(buffer), &length, &worker) == 0);
	assert(msg_decode_worker_register(buffer, (uint32_t)length, &decoded_worker) == 0);
	assert(decoded_worker.total_cores == 4);
	assert(decoded_worker.total_ram_mb == 1024);
	assert(decoded_worker.total_gpu == 1);

	// Check worker register acknowledgement fields.
	assert(msg_encode_worker_register_ack(buffer, sizeof(buffer), &length, 7) == 0);
	assert(msg_decode_worker_register_ack(buffer, (uint32_t)length, &worker_id) == 0);
	assert(worker_id == 7);

	// Check error payload fields.
	assert(msg_encode_error(buffer, sizeof(buffer), &length, (uint16_t)code, "bad frame") == 0);
	assert(msg_decode_error(buffer, (uint32_t)length, &code, text, sizeof(text)) == 0);
	assert(code == 0);
	assert(strcmp(text, "bad frame") == 0);
}

// Run every assert-based framing and payload test.
int main(void)
{
	test_header();
	test_round_trip();
	test_empty_payload();
	test_clean_eof();
	test_bad_version();
	test_oversize_length();
	test_partial_header();
	test_truncated_payload();
	test_invalid_type();
	test_message_helpers();
	return 0;
}
