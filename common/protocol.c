#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

void msg_header_encode(uint8_t out[OSN_HEADER_SIZE], msg_type_t type, uint32_t length)
{
	uint16_t raw_type = htons((uint16_t)type);
	uint16_t raw_version = htons((uint16_t)OSN_PROTOCOL_VERSION);
	uint32_t raw_length = htonl(length);

	memcpy(out, &raw_type, sizeof(raw_type));
	memcpy(out + 2, &raw_version, sizeof(raw_version));
	memcpy(out + 4, &raw_length, sizeof(raw_length));
}

int msg_header_decode(const uint8_t in[OSN_HEADER_SIZE], msg_type_t *type, uint16_t *version, uint32_t *length)
{
	uint16_t raw_type;
	uint16_t raw_version;
	uint32_t raw_length;

	if (in == NULL || type == NULL || version == NULL || length == NULL) {
		errno = EINVAL;
		return -1;
	}

	memcpy(&raw_type, in, sizeof(raw_type));
	memcpy(&raw_version, in + 2, sizeof(raw_version));
	memcpy(&raw_length, in + 4, sizeof(raw_length));

	*type = (msg_type_t)ntohs(raw_type);
	*version = ntohs(raw_version);
	*length = ntohl(raw_length);
	return 0;
}

int send_msg(int fd, msg_type_t type, const void *payload, uint32_t length)
{
	uint8_t header[OSN_HEADER_SIZE];

	if (type <= MSG_NONE || type >= MSG_COUNT || length > OSN_MAX_PAYLOAD ||
	    (length > 0 && payload == NULL)) {
		errno = EINVAL;
		return -1;
	}

	msg_header_encode(header, type, length);
	if (send_all(fd, header, sizeof(header)) != 0) {
		return -1;
	}
	if (length > 0 && send_all(fd, payload, length) != 0) {
		return -1;
	}
	return 0;
}

recv_status_t recv_msg(int fd, msg_type_t *type, uint8_t **payload, uint32_t *length)
{
	uint8_t header[OSN_HEADER_SIZE];
	msg_type_t parsed_type;
	uint16_t version;
	uint32_t parsed_length;
	uint8_t *buffer;
	recv_status_t status;

	if (type == NULL || payload == NULL || length == NULL) {
		errno = EINVAL;
		return RECV_ERR;
	}

	status = recv_all(fd, header, sizeof(header));
	if (status != RECV_OK) {
		return status;
	}
	if (msg_header_decode(header, &parsed_type, &version, &parsed_length) != 0) {
		return RECV_ERR;
	}
	if (version != OSN_PROTOCOL_VERSION || parsed_type <= MSG_NONE ||
	    parsed_type >= MSG_COUNT || parsed_length > OSN_MAX_PAYLOAD) {
		errno = EPROTO;
		return RECV_ERR;
	}

	buffer = malloc(parsed_length > 0 ? parsed_length : 1);
	if (buffer == NULL) {
		return RECV_ERR;
	}
	status = recv_all(fd, buffer, parsed_length);
	if (status != RECV_OK) {
		free(buffer);
		if (status == RECV_EOF) {
			errno = EPROTO;
		}
		return RECV_ERR;
	}

	*type = parsed_type;
	*payload = buffer;
	*length = parsed_length;
	return RECV_OK;
}

void msg_payload_free(uint8_t *payload)
{
	free(payload);
}

const char *msg_type_name(msg_type_t type)
{
	switch (type) {
	case MSG_CLIENT_HELLO: return "CLIENT_HELLO";
	case MSG_SUBMIT: return "SUBMIT";
	case MSG_STATUS_REQ: return "STATUS_REQ";
	case MSG_NODES_REQ: return "NODES_REQ";
	case MSG_LOGS_REQ: return "LOGS_REQ";
	case MSG_LOGS_UNSUB: return "LOGS_UNSUB";
	case MSG_CANCEL: return "CANCEL";
	case MSG_CRONS_REQ: return "CRONS_REQ";
	case MSG_CANCEL_CRON: return "CANCEL_CRON";
	case MSG_MIGRATE: return "MIGRATE";
	case MSG_HELLO_ACK: return "HELLO_ACK";
	case MSG_SUBMIT_ACK: return "SUBMIT_ACK";
	case MSG_EVENT: return "EVENT";
	case MSG_STATUS_REPLY: return "STATUS_REPLY";
	case MSG_NODES_REPLY: return "NODES_REPLY";
	case MSG_LOGS_CHUNK: return "LOGS_CHUNK";
	case MSG_ERROR: return "ERROR";
	case MSG_WORKER_REGISTER: return "WORKER_REGISTER";
	case MSG_HEARTBEAT: return "HEARTBEAT";
	case MSG_CACHE_REPORT: return "CACHE_REPORT";
	case MSG_CACHE_MISS: return "CACHE_MISS";
	case MSG_OUTPUT_CHUNK: return "OUTPUT_CHUNK";
	case MSG_CHECKPOINT_CHUNK: return "CHECKPOINT_CHUNK";
	case MSG_JOB_RESULT: return "JOB_RESULT";
	case MSG_DRAIN_NOTIFY: return "DRAIN_NOTIFY";
	case MSG_WORKER_REGISTER_ACK: return "WORKER_REGISTER_ACK";
	case MSG_JOB_ASSIGN: return "JOB_ASSIGN";
	case MSG_JOB_CANCEL: return "JOB_CANCEL";
	case MSG_JOB_PREEMPT: return "JOB_PREEMPT";
	case MSG_JOB_MIGRATE: return "JOB_MIGRATE";
	case MSG_HEARTBEAT_ACK: return "HEARTBEAT_ACK";
	case MSG_DISCOVER_REQ: return "DISCOVER_REQ";
	case MSG_DISCOVER_REPLY: return "DISCOVER_REPLY";
	default: return "UNKNOWN";
	}
}
