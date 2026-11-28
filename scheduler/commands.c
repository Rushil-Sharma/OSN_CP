#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "../common/log.h"
#include "../common/messages.h"
#include "../common/protocol.h"

// Bound how long a send to a stalled client may hold the state lock during an event push.
#define CLIENT_SEND_TIMEOUT_SEC 2

// Send an error frame on a logged-in session.
static void reply_error(scheduler_state_t *state, int slot, uint16_t code, const char *text)
{
	uint8_t payload[MSG_ERROR_TEXT_CAP + 8u];
	size_t length = 0;

	if (msg_encode_error(payload, sizeof(payload), &length, code, text) == 0) {
		(void)scheduler_session_send(state, slot, MSG_ERROR, payload, (uint32_t)length);
	}
}

// Send an error frame before a session exists (bad hello, too many clients).
static void reply_error_raw(int fd, uint16_t code, const char *text)
{
	uint8_t payload[MSG_ERROR_TEXT_CAP + 8u];
	size_t length = 0;

	if (msg_encode_error(payload, sizeof(payload), &length, code, text) == 0) {
		(void)send_msg(fd, MSG_ERROR, payload, (uint32_t)length);
	}
}

// Validate and enqueue a submit request, replying with the job id or an error.
static void handle_submit(scheduler_state_t *state, int slot, const char *user,
                          const uint8_t *body, uint32_t len)
{
	msg_submit_t *submit = malloc(sizeof(*submit));
	uint8_t ack[8];
	size_t ack_len = 0;
	uint32_t job_id = 0;
	int rc;

	if (submit == NULL) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "scheduler out of memory");
		return;
	}
	if (msg_decode_submit(body, len, submit) != 0) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "malformed submit request");
		free(submit);
		return;
	}
	// Re-check the client's validation; the scheduler never trusts the wire.
	if (submit->argv[0][0] == '\0' || submit->cores == 0 || submit->ram_mb == 0 ||
	    submit->time_sec == 0 || submit->prio < 1 || submit->prio > 3) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST,
		            "submit needs a command, -cores >= 1, -ram >= 1, -time >= 1, -prio 1-3");
		free(submit);
		return;
	}
	rc = scheduler_submit_job(state, submit, user, &job_id);
	free(submit);
	if (rc == MSG_ERR_QUEUE_FULL) {
		log_sched("Submit from %s rejected: QUEUE_FULL", user);
		reply_error(state, slot, MSG_ERR_QUEUE_FULL, "QUEUE_FULL");
		return;
	}
	if (rc != 0) {
		reply_error(state, slot, (uint16_t)rc, "job table full; no more jobs can be stored");
		return;
	}
	if (msg_encode_job_id(ack, sizeof(ack), &ack_len, job_id) == 0) {
		(void)scheduler_session_send(state, slot, MSG_SUBMIT_ACK, ack, (uint32_t)ack_len);
	}
}

// Reply with one job's status.
static void handle_status(scheduler_state_t *state, int slot, const uint8_t *body, uint32_t len)
{
	uint8_t payload[512];
	msg_status_t status;
	size_t out_len = 0;
	uint32_t job_id;
	char text[64];

	if (msg_decode_job_id(body, len, &job_id) != 0) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "malformed status request");
		return;
	}
	if (scheduler_fill_status(state, job_id, &status) != 0) {
		snprintf(text, sizeof(text), "Job %u does not exist", job_id);
		reply_error(state, slot, MSG_ERR_NO_SUCH_JOB, text);
		return;
	}
	if (msg_encode_status(payload, sizeof(payload), &out_len, &status) == 0) {
		(void)scheduler_session_send(state, slot, MSG_STATUS_REPLY, payload, (uint32_t)out_len);
	}
}

// Reply with every registered worker's capacity and state.
static void handle_nodes(scheduler_state_t *state, int slot)
{
	msg_node_t *nodes = malloc(sizeof(*nodes) * MAX_WORKERS);
	uint8_t *payload = malloc(MSG_NODES_REPLY_CAP);
	size_t out_len = 0;
	uint32_t count;

	if (nodes == NULL || payload == NULL) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "scheduler out of memory");
	} else {
		count = scheduler_fill_nodes(state, nodes, MAX_WORKERS);
		if (msg_encode_nodes(payload, MSG_NODES_REPLY_CAP, &out_len, nodes, count) == 0) {
			(void)scheduler_session_send(state, slot, MSG_NODES_REPLY, payload, (uint32_t)out_len);
		}
	}
	free(nodes);
	free(payload);
}

// Reply with all buffered output for a job; follow mode arrives with TODO 54.
static void handle_logs(scheduler_state_t *state, int slot, const char *user,
                        const uint8_t *body, uint32_t len)
{
	uint8_t *output = malloc(JOB_OUTPUT_CAP);
	uint8_t *payload = malloc(JOB_OUTPUT_CAP + MSG_LOGS_CHUNK_HEADER);
	uint32_t output_len = 0;
	size_t out_len = 0;
	uint32_t job_id;
	uint8_t follow;
	char text[64];

	if (output == NULL || payload == NULL) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "scheduler out of memory");
	} else if (msg_decode_logs_req(body, len, &job_id, &follow) != 0) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "malformed logs request");
	} else if (follow) {
		reply_error(state, slot, MSG_ERR_UNSUPPORTED, "logs -f is not implemented yet");
	} else if (scheduler_copy_output(state, job_id, output, &output_len) != 0) {
		snprintf(text, sizeof(text), "Job %u does not exist", job_id);
		reply_error(state, slot, MSG_ERR_NO_SUCH_JOB, text);
	} else if (msg_encode_logs_chunk(payload, JOB_OUTPUT_CAP + MSG_LOGS_CHUNK_HEADER, &out_len,
	                                 job_id, 1, output, output_len) == 0) {
		log_sched("Client '%s' read Job %u logs (%u bytes)", user, job_id, output_len);
		(void)scheduler_session_send(state, slot, MSG_LOGS_CHUNK, payload, (uint32_t)out_len);
	}
	free(output);
	free(payload);
}

// Cancel a job for this user and acknowledge with the state it was in.
static void handle_cancel(scheduler_state_t *state, int slot, const char *user,
                          const uint8_t *body, uint32_t len)
{
	char err[MSG_ERROR_TEXT_CAP + 1u];
	job_state_t old_state = JOB_QUEUED;
	uint8_t ack[8];
	size_t ack_len = 0;
	uint32_t job_id;
	int rc;

	if (msg_decode_job_id(body, len, &job_id) != 0) {
		reply_error(state, slot, MSG_ERR_BAD_REQUEST, "malformed cancel request");
		return;
	}
	rc = scheduler_cancel_job(state, job_id, user, &old_state, err, sizeof(err));
	if (rc != 0) {
		log_sched("Cancel of Job %u by %s refused: %s", job_id, user, err);
		reply_error(state, slot, (uint16_t)rc, err);
		return;
	}
	if (msg_encode_cancel_ack(ack, sizeof(ack), &ack_len, job_id, (uint8_t)old_state) == 0) {
		(void)scheduler_session_send(state, slot, MSG_CANCEL_ACK, ack, (uint32_t)ack_len);
	}
}

// Log in a client from its hello frame, then serve its commands until it disconnects.
void client_session_run(int fd, scheduler_state_t *state, const uint8_t *hello, uint32_t hello_len)
{
	struct timeval timeout = { .tv_sec = CLIENT_SEND_TIMEOUT_SEC, .tv_usec = 0 };
	char user[MAX_USERNAME];
	uint8_t ack[16];
	size_t ack_len = 0;
	uint32_t session_id = 0;
	int slot;

	if (msg_decode_client_hello(hello, hello_len, user) != 0) {
		reply_error_raw(fd, MSG_ERR_BAD_REQUEST, "bad hello payload");
		return;
	}
	if (!msg_username_valid(user)) {
		reply_error_raw(fd, MSG_ERR_BAD_USERNAME,
		                "username must be 1-31 characters of letters, digits, _, - and .");
		return;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	// Same-name clients are allowed: each gets its own session id.
	slot = scheduler_add_session(state, fd, user, &session_id);
	if (slot < 0) {
		reply_error_raw(fd, MSG_ERR_TABLE_FULL, "too many connected clients");
		return;
	}
	if (msg_encode_hello_ack(ack, sizeof(ack), &ack_len, session_id) != 0 ||
	    scheduler_session_send(state, slot, MSG_HELLO_ACK, ack, (uint32_t)ack_len) != 0) {
		scheduler_remove_session(state, slot);
		return;
	}
	log_sched("Client '%s' logged in (session %u)", user, session_id);
	for (;;) {
		msg_type_t type;
		uint8_t *body = NULL;
		uint32_t body_len = 0;
		char text[96];

		if (recv_msg(fd, &type, &body, &body_len) != RECV_OK) {
			break;
		}
		switch (type) {
		case MSG_SUBMIT: handle_submit(state, slot, user, body, body_len); break;
		case MSG_STATUS_REQ: handle_status(state, slot, body, body_len); break;
		case MSG_NODES_REQ: handle_nodes(state, slot); break;
		case MSG_LOGS_REQ: handle_logs(state, slot, user, body, body_len); break;
		case MSG_CANCEL: handle_cancel(state, slot, user, body, body_len); break;
		default:
			snprintf(text, sizeof(text), "%s is not implemented yet", msg_type_name(type));
			log_sched("Client '%s' sent unsupported %s", user, msg_type_name(type));
			reply_error(state, slot, MSG_ERR_UNSUPPORTED, text);
			break;
		}
		msg_payload_free(body);
	}
	scheduler_remove_session(state, slot);
	log_sched("Client '%s' disconnected (session %u)", user, session_id);
}
