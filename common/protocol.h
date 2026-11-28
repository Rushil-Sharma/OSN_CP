#ifndef OSN_PROTOCOL_H
#define OSN_PROTOCOL_H

#include <stdint.h>

#include "net.h"

// Define protocol version, fixed header size, and maximum payload size.
#define OSN_PROTOCOL_VERSION 1
#define OSN_HEADER_SIZE 8
#define OSN_MAX_PAYLOAD (UINT32_C(64) * 1024u * 1024u)

// Define every message type used now and by later milestones.
typedef enum {
	MSG_NONE = 0,
	MSG_CLIENT_HELLO,
	MSG_SUBMIT,
	MSG_STATUS_REQ,
	MSG_NODES_REQ,
	MSG_LOGS_REQ,
	MSG_LOGS_UNSUB,
	MSG_CANCEL,
	MSG_CRONS_REQ,
	MSG_CANCEL_CRON,
	MSG_MIGRATE,

	MSG_HELLO_ACK,
	MSG_SUBMIT_ACK,
	MSG_EVENT,
	MSG_STATUS_REPLY,
	MSG_NODES_REPLY,
	MSG_LOGS_CHUNK,
	MSG_ERROR,
	MSG_CANCEL_ACK,

	MSG_WORKER_REGISTER,
	MSG_HEARTBEAT,
	MSG_CACHE_REPORT,
	MSG_CACHE_MISS,
	MSG_OUTPUT_CHUNK,
	MSG_CHECKPOINT_CHUNK,
	MSG_JOB_RESULT,
	MSG_DRAIN_NOTIFY,

	MSG_WORKER_REGISTER_ACK,
	MSG_JOB_ASSIGN,
	MSG_JOB_CANCEL,
	MSG_JOB_PREEMPT,
	MSG_JOB_MIGRATE,
	MSG_HEARTBEAT_ACK,

	MSG_DISCOVER_REQ,
	MSG_DISCOVER_REPLY,

	MSG_COUNT
} msg_type_t;

// Define the in-memory view of the fixed eight-byte wire header.
typedef struct {
	uint16_t type;
	uint16_t version;
	uint32_t length;
} msg_header_t;

// Encode a header into network byte order.
void msg_header_encode(uint8_t out[OSN_HEADER_SIZE], msg_type_t type, uint32_t length);

// Decode and validate a header from network byte order.
int msg_header_decode(const uint8_t in[OSN_HEADER_SIZE], msg_type_t *type, uint16_t *version, uint32_t *length);

// Send one framed message with a header and optional payload.
int send_msg(int fd, msg_type_t type, const void *payload, uint32_t length);

// Receive one framed message and allocate its payload.
recv_status_t recv_msg(int fd, msg_type_t *type, uint8_t **payload, uint32_t *length);

// Release a payload buffer returned by recv_msg.
void msg_payload_free(uint8_t *payload);

// Return the printable name of a message type.
const char *msg_type_name(msg_type_t type);

#endif
