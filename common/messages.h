#ifndef OSN_MESSAGES_H
#define OSN_MESSAGES_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

// Define the maximum text length carried in an error payload.
#define MSG_ERROR_TEXT_CAP 255u

// Define the maximum text length of an event line or a joined command line.
#define MSG_TEXT_CAP 255u

// Define the numeric codes carried in MSG_ERROR payloads.
typedef enum {
	MSG_ERR_BAD_REQUEST = 1,
	MSG_ERR_BAD_REGISTER = 2,
	MSG_ERR_WORKER_CAPACITY = 3,
	MSG_ERR_UNEXPECTED_FIRST = 4,
	MSG_ERR_QUEUE_FULL = 5,
	MSG_ERR_NO_SUCH_JOB = 6,
	MSG_ERR_INVALID_STATE = 7,
	MSG_ERR_UNSUPPORTED = 8,
	MSG_ERR_TABLE_FULL = 9,
	MSG_ERR_BAD_USERNAME = 10
} msg_error_code_t;

// Carry one submit request: command argv plus resource accounting.
typedef struct {
	uint32_t argc;
	char argv[MAX_ARGV][MAX_ARG_LEN];
	uint32_t cores;
	uint64_t ram_mb;
	uint32_t time_sec;
	uint32_t prio;
	uint32_t gpu;
} msg_submit_t;

// Carry one status reply for a single job.
typedef struct {
	uint32_t job_id;
	char owner[MAX_USERNAME];
	uint8_t state;
	int32_t worker_id;
	uint32_t cores;
	uint64_t ram_mb;
	uint32_t time_sec;
	uint32_t prio;
	uint32_t gpu;
	uint8_t attempts;
	char command[MSG_TEXT_CAP + 1u];
	char reason[MAX_REASON_LEN];
} msg_status_t;

// Carry one worker row of a nodes reply.
typedef struct {
	int32_t id;
	uint8_t state;
	uint32_t free_cores;
	uint32_t total_cores;
	uint64_t free_ram_mb;
	uint64_t total_ram_mb;
	uint32_t free_gpu;
	uint32_t total_gpu;
	uint32_t running_jobs;
} msg_node_t;

// Define the encoded size of one nodes row and of a full nodes reply.
#define MSG_NODE_WIRE_SIZE 41u
#define MSG_NODES_REPLY_CAP (4u + MAX_WORKERS * MSG_NODE_WIRE_SIZE)

// Define the encoded overhead of a logs chunk before its output bytes.
#define MSG_LOGS_CHUNK_HEADER 9u

// Return 1 for a valid username: 1-31 chars of letters, digits, '_', '-', '.'.
int msg_username_valid(const char *user);

// Encode or decode a payload holding only a job id (status, cancel, submit ack).
int msg_encode_job_id(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id);
int msg_decode_job_id(const uint8_t *buf, uint32_t len, uint32_t *job_id);

// Encode or decode a submit request.
int msg_encode_submit(uint8_t *buf, size_t cap, size_t *out_len, const msg_submit_t *submit);
int msg_decode_submit(const uint8_t *buf, uint32_t len, msg_submit_t *submit);

// Encode or decode a status reply.
int msg_encode_status(uint8_t *buf, size_t cap, size_t *out_len, const msg_status_t *status);
int msg_decode_status(const uint8_t *buf, uint32_t len, msg_status_t *status);

// Encode or decode a nodes reply holding count worker rows.
int msg_encode_nodes(uint8_t *buf, size_t cap, size_t *out_len, const msg_node_t *nodes, uint32_t count);
int msg_decode_nodes(const uint8_t *buf, uint32_t len, msg_node_t *nodes, uint32_t cap, uint32_t *count);

// Encode or decode a logs request (job id + follow flag).
int msg_encode_logs_req(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t follow);
int msg_decode_logs_req(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *follow);

// Encode a logs chunk; decode returns a pointer to the output bytes inside buf.
int msg_encode_logs_chunk(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t final,
                          const uint8_t *data, uint32_t data_len);
int msg_decode_logs_chunk(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *final,
                          const uint8_t **data, uint32_t *data_len);

// Encode or decode a cancel acknowledgement (job id + state before cancelling).
int msg_encode_cancel_ack(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t old_state);
int msg_decode_cancel_ack(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *old_state);

// Encode or decode an [Event] text line.
int msg_encode_event(uint8_t *buf, size_t cap, size_t *out_len, const char *text);
int msg_decode_event(const uint8_t *buf, uint32_t len, char *text, size_t cap);

// Encode a client hello payload from a username.
int msg_encode_client_hello(uint8_t *buf, size_t cap, size_t *out_len, const char *user);

// Decode a client hello payload into a username buffer.
int msg_decode_client_hello(const uint8_t *buf, uint32_t len, char user[MAX_USERNAME]);

// Encode a hello acknowledgement payload from a session id.
int msg_encode_hello_ack(uint8_t *buf, size_t cap, size_t *out_len, uint32_t session_id);

// Decode a hello acknowledgement payload into a session id.
int msg_decode_hello_ack(const uint8_t *buf, uint32_t len, uint32_t *session_id);

// Encode a worker register payload from worker resource fields.
int msg_encode_worker_register(uint8_t *buf, size_t cap, size_t *out_len, const worker_t *worker);

// Decode a worker register payload into a worker snapshot.
int msg_decode_worker_register(const uint8_t *buf, uint32_t len, worker_t *worker);

// Encode a worker register acknowledgement payload from a worker id.
int msg_encode_worker_register_ack(uint8_t *buf, size_t cap, size_t *out_len, int32_t worker_id);

// Decode a worker register acknowledgement payload into a worker id.
int msg_decode_worker_register_ack(const uint8_t *buf, uint32_t len, int32_t *worker_id);

// Encode an error payload from a numeric code and text.
int msg_encode_error(uint8_t *buf, size_t cap, size_t *out_len, uint16_t code, const char *text);

// Decode an error payload into a numeric code and text buffer.
int msg_decode_error(const uint8_t *buf, uint32_t len, uint16_t *code, char *text, size_t cap);

#endif
