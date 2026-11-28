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

// Verify the username rule: 1-31 chars of letters, digits, '_', '-', '.'.
static void test_username_valid(void)
{
	assert(msg_username_valid("lordz"));
	assert(msg_username_valid("a.b-c_D9"));
	assert(msg_username_valid("x"));
	assert(msg_username_valid("abcdefghijklmnopqrstuvwxyz01234"));
	assert(!msg_username_valid("abcdefghijklmnopqrstuvwxyz012345"));
	assert(!msg_username_valid(""));
	assert(!msg_username_valid("has space"));
	assert(!msg_username_valid("semi;colon"));
	assert(!msg_username_valid(NULL));
}

// Verify the client command payloads round-trip and reject trailing bytes.
static void test_command_messages(void)
{
	static msg_submit_t submit;
	static msg_submit_t decoded_submit;
	static uint8_t big[JOB_OUTPUT_CAP + MSG_LOGS_CHUNK_HEADER];
	uint8_t buffer[512];
	msg_status_t status;
	msg_status_t decoded_status;
	msg_node_t nodes[2];
	msg_node_t decoded_nodes[2];
	const uint8_t *data = NULL;
	uint32_t data_len = 0;
	uint32_t job_id = 0;
	uint32_t count = 0;
	uint8_t flag = 0;
	char text[MSG_TEXT_CAP + 1u];
	size_t length = 0;

	// Check job id payloads and that an extra byte is rejected.
	assert(msg_encode_job_id(buffer, sizeof(buffer), &length, 77) == 0);
	assert(msg_decode_job_id(buffer, (uint32_t)length, &job_id) == 0 && job_id == 77);
	assert(msg_decode_job_id(buffer, (uint32_t)length + 1u, &job_id) != 0);

	// Check submit payload fields.
	memset(&submit, 0, sizeof(submit));
	submit.argc = 2;
	strcpy(submit.argv[0], "sleep");
	strcpy(submit.argv[1], "5");
	submit.cores = 2;
	submit.ram_mb = 1024;
	submit.time_sec = 10;
	submit.prio = 1;
	submit.gpu = 0;
	assert(msg_encode_submit(buffer, sizeof(buffer), &length, &submit) == 0);
	assert(msg_decode_submit(buffer, (uint32_t)length, &decoded_submit) == 0);
	assert(decoded_submit.argc == 2 && strcmp(decoded_submit.argv[1], "5") == 0);
	assert(decoded_submit.cores == 2 && decoded_submit.ram_mb == 1024);
	assert(decoded_submit.time_sec == 10 && decoded_submit.prio == 1 && decoded_submit.gpu == 0);
	assert(msg_decode_submit(buffer, (uint32_t)length - 1u, &decoded_submit) != 0);

	// Check status payload fields, including an unassigned worker id of -1.
	memset(&status, 0, sizeof(status));
	status.job_id = 3;
	strcpy(status.owner, "kg");
	status.state = JOB_QUEUED;
	status.worker_id = -1;
	status.ram_mb = 500;
	strcpy(status.command, "./long_script");
	assert(msg_encode_status(buffer, sizeof(buffer), &length, &status) == 0);
	assert(msg_decode_status(buffer, (uint32_t)length, &decoded_status) == 0);
	assert(decoded_status.job_id == 3 && decoded_status.worker_id == -1);
	assert(strcmp(decoded_status.owner, "kg") == 0 && decoded_status.ram_mb == 500);
	assert(strcmp(decoded_status.command, "./long_script") == 0);

	// Check nodes rows and the advertised wire size.
	memset(nodes, 0, sizeof(nodes));
	nodes[0].id = 1001;
	nodes[0].total_ram_mb = 2048;
	nodes[1].id = 1002;
	nodes[1].state = WORKER_DEAD;
	assert(msg_encode_nodes(buffer, sizeof(buffer), &length, nodes, 2) == 0);
	assert(length == 4u + 2u * MSG_NODE_WIRE_SIZE);
	assert(msg_decode_nodes(buffer, (uint32_t)length, decoded_nodes, 2, &count) == 0 && count == 2);
	assert(decoded_nodes[0].id == 1001 && decoded_nodes[0].total_ram_mb == 2048);
	assert(decoded_nodes[1].state == WORKER_DEAD);
	assert(msg_decode_nodes(buffer, (uint32_t)length, decoded_nodes, 1, &count) != 0);

	// Check logs request and a full-size logs chunk.
	assert(msg_encode_logs_req(buffer, sizeof(buffer), &length, 9, 1) == 0);
	assert(msg_decode_logs_req(buffer, (uint32_t)length, &job_id, &flag) == 0);
	assert(job_id == 9 && flag == 1);
	memset(big + MSG_LOGS_CHUNK_HEADER, 'x', JOB_OUTPUT_CAP);
	assert(msg_encode_logs_chunk(big, sizeof(big), &length, 9, 1, big + MSG_LOGS_CHUNK_HEADER,
	                             JOB_OUTPUT_CAP) == 0);
	assert(msg_decode_logs_chunk(big, (uint32_t)length, &job_id, &flag, &data, &data_len) == 0);
	assert(job_id == 9 && flag == 1 && data_len == JOB_OUTPUT_CAP && data[0] == 'x');

	// Check cancel acknowledgement and event text.
	assert(msg_encode_cancel_ack(buffer, sizeof(buffer), &length, 4, JOB_QUEUED) == 0);
	assert(msg_decode_cancel_ack(buffer, (uint32_t)length, &job_id, &flag) == 0);
	assert(job_id == 4 && flag == JOB_QUEUED);
	assert(msg_encode_event(buffer, sizeof(buffer), &length, "Job 1 COMPLETED successfully.") == 0);
	assert(msg_decode_event(buffer, (uint32_t)length, text, sizeof(text)) == 0);
	assert(strcmp(text, "Job 1 COMPLETED successfully.") == 0);
}

// Verify job assignment, result, and heartbeat payloads round-trip.
static void test_job_transfer_messages(void)
{
	static msg_job_assign_t assign;
	static msg_job_assign_t decoded_assign;
	static msg_job_result_t result;
	static msg_job_result_t decoded_result;
	uint8_t buffer[512];
	uint8_t binary[4] = {0x7f, 'E', 'L', 'F'};
	uint32_t cpu_load_pct = 0;
	uint32_t running_jobs = 0;
	size_t length = 0;

	memset(&assign, 0, sizeof(assign));
	assign.job_id = 12;
	assign.argc = 2;
	strcpy(assign.argv[0], "./script");
	strcpy(assign.argv[1], "arg");
	assign.cores = 2;
	assign.ram_mb = 512;
	assign.time_sec = 30;
	assign.prio = 2;
	assign.gpu = 1;
	assign.use_path = 0;
	assign.binary = binary;
	assign.binary_len = sizeof(binary);
	assert(msg_encode_job_assign(buffer, sizeof(buffer), &length, &assign) == 0);
	assert(msg_decode_job_assign(buffer, (uint32_t)length, &decoded_assign) == 0);
	assert(decoded_assign.job_id == 12 && decoded_assign.argc == 2);
	assert(strcmp(decoded_assign.argv[0], "./script") == 0);
	assert(decoded_assign.cores == 2 && decoded_assign.ram_mb == 512);
	assert(decoded_assign.time_sec == 30 && decoded_assign.prio == 2 && decoded_assign.gpu == 1);
	assert(decoded_assign.use_path == 0 && decoded_assign.binary_len == sizeof(binary));
	assert(memcmp(decoded_assign.binary, binary, sizeof(binary)) == 0);
	assert(msg_decode_job_assign(buffer, (uint32_t)length + 1u, &decoded_assign) != 0);

	memset(&result, 0, sizeof(result));
	result.job_id = 12;
	result.exit_code = -1;
	result.term_signal = 0;
	strcpy(result.reason, "execution not implemented");
	assert(msg_encode_job_result(buffer, sizeof(buffer), &length, &result) == 0);
	assert(msg_decode_job_result(buffer, (uint32_t)length, &decoded_result) == 0);
	assert(decoded_result.job_id == 12 && decoded_result.exit_code == -1);
	assert(decoded_result.term_signal == 0);
	assert(strcmp(decoded_result.reason, "execution not implemented") == 0);

	assert(msg_encode_heartbeat(buffer, sizeof(buffer), &length, 35, 2) == 0);
	assert(msg_decode_heartbeat(buffer, (uint32_t)length, &cpu_load_pct, &running_jobs) == 0);
	assert(cpu_load_pct == 35 && running_jobs == 2);
	assert(msg_decode_heartbeat(buffer, (uint32_t)length + 1u, &cpu_load_pct, &running_jobs) != 0);
}

// Run every assert-based framing and payload test.
int main(void)
{
	test_username_valid();
	test_command_messages();
	test_job_transfer_messages();
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
