#include "messages.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

typedef struct {
	uint8_t *data;
	size_t cap;
	size_t pos;
} enc_cursor_t;

typedef struct {
	const uint8_t *data;
	size_t len;
	size_t pos;
} dec_cursor_t;

// Initialize an encode cursor over a caller-owned buffer.
static void enc_init(enc_cursor_t *cursor, uint8_t *data, size_t cap)
{
	cursor->data = data;
	cursor->cap = cap;
	cursor->pos = 0;
}

// Append raw bytes to an encode buffer with bounds checking.
static int enc_bytes(enc_cursor_t *cursor, const void *src, size_t len)
{
	if (cursor->data == NULL || src == NULL || len > cursor->cap - cursor->pos) {
		errno = EINVAL;
		return -1;
	}
	memcpy(cursor->data + cursor->pos, src, len);
	cursor->pos += len;
	return 0;
}

// Append one uint8_t to an encode buffer.
static int enc_u8(enc_cursor_t *cursor, uint8_t value)
{
	return enc_bytes(cursor, &value, sizeof(value));
}

// Append one network-order uint16_t to an encode buffer.
static int enc_u16(enc_cursor_t *cursor, uint16_t value)
{
	uint16_t raw = htons(value);
	return enc_bytes(cursor, &raw, sizeof(raw));
}

// Append one network-order uint32_t to an encode buffer.
static int enc_u32(enc_cursor_t *cursor, uint32_t value)
{
	uint32_t raw = htonl(value);
	return enc_bytes(cursor, &raw, sizeof(raw));
}

// Append one big-endian uint64_t to an encode buffer.
static int enc_u64(enc_cursor_t *cursor, uint64_t value)
{
	uint8_t raw[8];
	size_t i;

	for (i = 0; i < sizeof(raw); i++) {
		raw[i] = (uint8_t)(value >> (56u - i * 8u));
	}
	return enc_bytes(cursor, raw, sizeof(raw));
}

// Append one length-prefixed string to an encode buffer.
static int enc_str(enc_cursor_t *cursor, const char *value)
{
	size_t len;

	if (value == NULL) {
		errno = EINVAL;
		return -1;
	}
	len = strlen(value);
	if (len > UINT8_MAX) {
		errno = EINVAL;
		return -1;
	}
	if (enc_u8(cursor, (uint8_t)len) != 0) {
		return -1;
	}
	return enc_bytes(cursor, value, len);
}

// Initialize a decode cursor over a received payload.
static void dec_init(dec_cursor_t *cursor, const uint8_t *data, size_t len)
{
	cursor->data = data;
	cursor->len = len;
	cursor->pos = 0;
}

// Read raw bytes from a decode buffer with bounds checking.
static int dec_bytes(dec_cursor_t *cursor, void *dst, size_t len)
{
	if (cursor->data == NULL || dst == NULL || len > cursor->len - cursor->pos) {
		errno = EINVAL;
		return -1;
	}
	memcpy(dst, cursor->data + cursor->pos, len);
	cursor->pos += len;
	return 0;
}

// Point into a decode buffer without copying raw bytes.
static int dec_bytes_ptr(dec_cursor_t *cursor, const uint8_t **dst, size_t len)
{
	if (cursor->data == NULL || dst == NULL || len > cursor->len - cursor->pos) {
		errno = EINVAL;
		return -1;
	}
	*dst = cursor->data + cursor->pos;
	cursor->pos += len;
	return 0;
}

// Read one uint8_t from a decode buffer.
static int dec_u8(dec_cursor_t *cursor, uint8_t *value)
{
	return dec_bytes(cursor, value, sizeof(*value));
}

// Read one network-order uint16_t from a decode buffer.
static int dec_u16(dec_cursor_t *cursor, uint16_t *value)
{
	uint16_t raw;

	if (dec_bytes(cursor, &raw, sizeof(raw)) != 0) {
		return -1;
	}
	*value = ntohs(raw);
	return 0;
}

// Read one network-order uint32_t from a decode buffer.
static int dec_u32(dec_cursor_t *cursor, uint32_t *value)
{
	uint32_t raw;

	if (dec_bytes(cursor, &raw, sizeof(raw)) != 0) {
		return -1;
	}
	*value = ntohl(raw);
	return 0;
}

// Read one big-endian uint64_t from a decode buffer.
static int dec_u64(dec_cursor_t *cursor, uint64_t *value)
{
	uint8_t raw[8];
	size_t i;
	uint64_t result = 0;

	if (dec_bytes(cursor, raw, sizeof(raw)) != 0) {
		return -1;
	}
	for (i = 0; i < sizeof(raw); i++) {
		result = (result << 8u) | raw[i];
	}
	*value = result;
	return 0;
}

// Read one length-prefixed string into a caller-owned buffer.
static int dec_str(dec_cursor_t *cursor, char *dst, size_t cap)
{
	uint8_t len;

	if (dst == NULL || cap == 0 || dec_u8(cursor, &len) != 0) {
		errno = EINVAL;
		return -1;
	}
	if ((size_t)len >= cap || dec_bytes(cursor, dst, len) != 0) {
		errno = EINVAL;
		return -1;
	}
	dst[len] = '\0';
	return 0;
}

// Encode a client hello payload from a username.
int msg_encode_client_hello(uint8_t *buf, size_t cap, size_t *out_len, const char *user)
{
	enc_cursor_t cursor;

	if (out_len == NULL || user == NULL || strlen(user) >= MAX_USERNAME) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_str(&cursor, user) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a client hello payload into a username buffer.
int msg_decode_client_hello(const uint8_t *buf, uint32_t len, char user[MAX_USERNAME])
{
	dec_cursor_t cursor;

	if (user == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	return dec_str(&cursor, user, MAX_USERNAME);
}

// Encode a hello acknowledgement payload from a session id.
int msg_encode_hello_ack(uint8_t *buf, size_t cap, size_t *out_len, uint32_t session_id)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, session_id) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a hello acknowledgement payload into a session id.
int msg_decode_hello_ack(const uint8_t *buf, uint32_t len, uint32_t *session_id)
{
	dec_cursor_t cursor;

	if (session_id == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	return dec_u32(&cursor, session_id);
}

// Encode a worker register payload from worker resource fields.
int msg_encode_worker_register(uint8_t *buf, size_t cap, size_t *out_len, const worker_t *worker)
{
	enc_cursor_t cursor;

	if (out_len == NULL || worker == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, worker->total_cores) != 0 ||
	    enc_u64(&cursor, worker->total_ram_mb) != 0 ||
	    enc_u32(&cursor, worker->total_gpu) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a worker register payload into a worker snapshot.
int msg_decode_worker_register(const uint8_t *buf, uint32_t len, worker_t *worker)
{
	dec_cursor_t cursor;

	if (worker == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(worker, 0, sizeof(*worker));
	worker->id = -1;
	worker->fd = -1;
	worker->state = WORKER_ALIVE;
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &worker->total_cores) != 0 ||
	    dec_u64(&cursor, &worker->total_ram_mb) != 0 ||
	    dec_u32(&cursor, &worker->total_gpu) != 0) {
		return -1;
	}
	worker->free_cores = worker->total_cores;
	worker->free_ram_mb = worker->total_ram_mb;
	worker->free_gpu = worker->total_gpu;
	return 0;
}

// Encode a worker register acknowledgement payload from a worker id.
int msg_encode_worker_register_ack(uint8_t *buf, size_t cap, size_t *out_len, int32_t worker_id)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, (uint32_t)worker_id) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a worker register acknowledgement payload into a worker id.
int msg_decode_worker_register_ack(const uint8_t *buf, uint32_t len, int32_t *worker_id)
{
	dec_cursor_t cursor;
	uint32_t raw;

	if (worker_id == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &raw) != 0) {
		return -1;
	}
	*worker_id = (int32_t)raw;
	return 0;
}

// Encode an error payload from a numeric code and text.
int msg_encode_error(uint8_t *buf, size_t cap, size_t *out_len, uint16_t code, const char *text)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u16(&cursor, code) != 0 || enc_str(&cursor, text) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode an error payload into a numeric code and text buffer.
int msg_decode_error(const uint8_t *buf, uint32_t len, uint16_t *code, char *text, size_t cap)
{
	dec_cursor_t cursor;

	if (code == NULL || text == NULL || cap == 0) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u16(&cursor, code) != 0) {
		return -1;
	}
	return dec_str(&cursor, text, cap);
}

// Reject trailing bytes so a payload must match its message layout exactly.
static int dec_done(const dec_cursor_t *cursor)
{
	if (cursor->pos != cursor->len) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

// Return 1 for a valid username: 1-31 chars of letters, digits, '_', '-', '.'.
int msg_username_valid(const char *user)
{
	size_t len;

	if (user == NULL) {
		return 0;
	}
	len = strlen(user);
	if (len == 0 || len >= MAX_USERNAME) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		char c = user[i];
		int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		         c == '_' || c == '-' || c == '.';

		if (!ok) {
			return 0;
		}
	}
	return 1;
}

// Encode a payload holding only a job id.
int msg_encode_job_id(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, job_id) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a payload holding only a job id.
int msg_decode_job_id(const uint8_t *buf, uint32_t len, uint32_t *job_id)
{
	dec_cursor_t cursor;

	if (job_id == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, job_id) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode a submit request: argc, argv strings, then resource fields.
int msg_encode_submit(uint8_t *buf, size_t cap, size_t *out_len, const msg_submit_t *submit)
{
	enc_cursor_t cursor;

	if (out_len == NULL || submit == NULL || submit->argc == 0 || submit->argc > MAX_ARGV) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, submit->argc) != 0) {
		return -1;
	}
	for (uint32_t i = 0; i < submit->argc; i++) {
		if (enc_str(&cursor, submit->argv[i]) != 0) {
			return -1;
		}
	}
	if (enc_u32(&cursor, submit->cores) != 0 || enc_u64(&cursor, submit->ram_mb) != 0 ||
	    enc_u32(&cursor, submit->time_sec) != 0 || enc_u32(&cursor, submit->prio) != 0 ||
	    enc_u32(&cursor, submit->gpu) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a submit request.
int msg_decode_submit(const uint8_t *buf, uint32_t len, msg_submit_t *submit)
{
	dec_cursor_t cursor;

	if (submit == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(submit, 0, sizeof(*submit));
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &submit->argc) != 0 || submit->argc == 0 || submit->argc > MAX_ARGV) {
		errno = EINVAL;
		return -1;
	}
	for (uint32_t i = 0; i < submit->argc; i++) {
		if (dec_str(&cursor, submit->argv[i], MAX_ARG_LEN) != 0) {
			return -1;
		}
	}
	if (dec_u32(&cursor, &submit->cores) != 0 || dec_u64(&cursor, &submit->ram_mb) != 0 ||
	    dec_u32(&cursor, &submit->time_sec) != 0 || dec_u32(&cursor, &submit->prio) != 0 ||
	    dec_u32(&cursor, &submit->gpu) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode a status reply.
int msg_encode_status(uint8_t *buf, size_t cap, size_t *out_len, const msg_status_t *status)
{
	enc_cursor_t cursor;

	if (out_len == NULL || status == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, status->job_id) != 0 || enc_str(&cursor, status->owner) != 0 ||
	    enc_u8(&cursor, status->state) != 0 || enc_u32(&cursor, (uint32_t)status->worker_id) != 0 ||
	    enc_u32(&cursor, status->cores) != 0 || enc_u64(&cursor, status->ram_mb) != 0 ||
	    enc_u32(&cursor, status->time_sec) != 0 || enc_u32(&cursor, status->prio) != 0 ||
	    enc_u32(&cursor, status->gpu) != 0 || enc_u8(&cursor, status->attempts) != 0 ||
	    enc_str(&cursor, status->command) != 0 || enc_str(&cursor, status->reason) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a status reply.
int msg_decode_status(const uint8_t *buf, uint32_t len, msg_status_t *status)
{
	dec_cursor_t cursor;
	uint32_t worker_raw;

	if (status == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(status, 0, sizeof(*status));
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &status->job_id) != 0 ||
	    dec_str(&cursor, status->owner, sizeof(status->owner)) != 0 ||
	    dec_u8(&cursor, &status->state) != 0 || dec_u32(&cursor, &worker_raw) != 0 ||
	    dec_u32(&cursor, &status->cores) != 0 || dec_u64(&cursor, &status->ram_mb) != 0 ||
	    dec_u32(&cursor, &status->time_sec) != 0 || dec_u32(&cursor, &status->prio) != 0 ||
	    dec_u32(&cursor, &status->gpu) != 0 || dec_u8(&cursor, &status->attempts) != 0 ||
	    dec_str(&cursor, status->command, sizeof(status->command)) != 0 ||
	    dec_str(&cursor, status->reason, sizeof(status->reason)) != 0) {
		return -1;
	}
	status->worker_id = (int32_t)worker_raw;
	return dec_done(&cursor);
}

// Encode a nodes reply: a row count followed by fixed-size worker rows.
int msg_encode_nodes(uint8_t *buf, size_t cap, size_t *out_len, const msg_node_t *nodes, uint32_t count)
{
	enc_cursor_t cursor;

	if (out_len == NULL || (count > 0 && nodes == NULL) || count > MAX_WORKERS) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, count) != 0) {
		return -1;
	}
	for (uint32_t i = 0; i < count; i++) {
		const msg_node_t *n = &nodes[i];

		if (enc_u32(&cursor, (uint32_t)n->id) != 0 || enc_u8(&cursor, n->state) != 0 ||
		    enc_u32(&cursor, n->free_cores) != 0 || enc_u32(&cursor, n->total_cores) != 0 ||
		    enc_u64(&cursor, n->free_ram_mb) != 0 || enc_u64(&cursor, n->total_ram_mb) != 0 ||
		    enc_u32(&cursor, n->free_gpu) != 0 || enc_u32(&cursor, n->total_gpu) != 0 ||
		    enc_u32(&cursor, n->running_jobs) != 0) {
			return -1;
		}
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a nodes reply into at most cap rows.
int msg_decode_nodes(const uint8_t *buf, uint32_t len, msg_node_t *nodes, uint32_t cap, uint32_t *count)
{
	dec_cursor_t cursor;

	if (nodes == NULL || count == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, count) != 0 || *count > cap) {
		errno = EINVAL;
		return -1;
	}
	for (uint32_t i = 0; i < *count; i++) {
		msg_node_t *n = &nodes[i];
		uint32_t id_raw;

		if (dec_u32(&cursor, &id_raw) != 0 || dec_u8(&cursor, &n->state) != 0 ||
		    dec_u32(&cursor, &n->free_cores) != 0 || dec_u32(&cursor, &n->total_cores) != 0 ||
		    dec_u64(&cursor, &n->free_ram_mb) != 0 || dec_u64(&cursor, &n->total_ram_mb) != 0 ||
		    dec_u32(&cursor, &n->free_gpu) != 0 || dec_u32(&cursor, &n->total_gpu) != 0 ||
		    dec_u32(&cursor, &n->running_jobs) != 0) {
			return -1;
		}
		n->id = (int32_t)id_raw;
	}
	return dec_done(&cursor);
}

// Encode a logs request (job id + follow flag).
int msg_encode_logs_req(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t follow)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, job_id) != 0 || enc_u8(&cursor, follow) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a logs request (job id + follow flag).
int msg_decode_logs_req(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *follow)
{
	dec_cursor_t cursor;

	if (job_id == NULL || follow == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, job_id) != 0 || dec_u8(&cursor, follow) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode a logs chunk: job id, final flag, byte count, then raw output bytes.
int msg_encode_logs_chunk(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t final,
                          const uint8_t *data, uint32_t data_len)
{
	enc_cursor_t cursor;

	if (out_len == NULL || (data_len > 0 && data == NULL)) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, job_id) != 0 || enc_u8(&cursor, final) != 0 ||
	    enc_u32(&cursor, data_len) != 0 || (data_len > 0 && enc_bytes(&cursor, data, data_len) != 0)) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a logs chunk; data points into buf, so it lives as long as buf does.
int msg_decode_logs_chunk(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *final,
                          const uint8_t **data, uint32_t *data_len)
{
	dec_cursor_t cursor;

	if (job_id == NULL || final == NULL || data == NULL || data_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, job_id) != 0 || dec_u8(&cursor, final) != 0 ||
	    dec_u32(&cursor, data_len) != 0 || *data_len != cursor.len - cursor.pos) {
		errno = EINVAL;
		return -1;
	}
	*data = buf + cursor.pos;
	return 0;
}

// Encode a cancel acknowledgement (job id + state before cancelling).
int msg_encode_cancel_ack(uint8_t *buf, size_t cap, size_t *out_len, uint32_t job_id, uint8_t old_state)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, job_id) != 0 || enc_u8(&cursor, old_state) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a cancel acknowledgement (job id + state before cancelling).
int msg_decode_cancel_ack(const uint8_t *buf, uint32_t len, uint32_t *job_id, uint8_t *old_state)
{
	dec_cursor_t cursor;

	if (job_id == NULL || old_state == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, job_id) != 0 || dec_u8(&cursor, old_state) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode an [Event] text line.
int msg_encode_event(uint8_t *buf, size_t cap, size_t *out_len, const char *text)
{
	enc_cursor_t cursor;

	if (out_len == NULL || text == NULL || strlen(text) > MSG_TEXT_CAP) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_str(&cursor, text) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode an [Event] text line.
int msg_decode_event(const uint8_t *buf, uint32_t len, char *text, size_t cap)
{
	dec_cursor_t cursor;

	if (text == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_str(&cursor, text, cap) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode a job assignment with an optional executable image.
int msg_encode_job_assign(uint8_t *buf, size_t cap, size_t *out_len, const msg_job_assign_t *assign)
{
	enc_cursor_t cursor;

	if (out_len == NULL || assign == NULL || assign->argc > MAX_ARGV ||
	    assign->binary_len > MSG_BINARY_CAP || (assign->binary_len > 0 && assign->binary == NULL)) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, assign->job_id) != 0 || enc_u32(&cursor, assign->argc) != 0) {
		return -1;
	}
	for (uint32_t i = 0; i < assign->argc; i++) {
		if (enc_str(&cursor, assign->argv[i]) != 0) {
			return -1;
		}
	}
	if (enc_u32(&cursor, assign->cores) != 0 || enc_u64(&cursor, assign->ram_mb) != 0 ||
	    enc_u32(&cursor, assign->time_sec) != 0 || enc_u32(&cursor, assign->prio) != 0 ||
	    enc_u32(&cursor, assign->gpu) != 0 || enc_u8(&cursor, assign->use_path) != 0 ||
	    enc_u32(&cursor, assign->binary_len) != 0) {
		return -1;
	}
	if (assign->binary_len > 0 && enc_bytes(&cursor, assign->binary, assign->binary_len) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a job assignment; binary points into buf for the life of the payload.
int msg_decode_job_assign(const uint8_t *buf, uint32_t len, msg_job_assign_t *assign)
{
	dec_cursor_t cursor;

	if (assign == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(assign, 0, sizeof(*assign));
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &assign->job_id) != 0 || dec_u32(&cursor, &assign->argc) != 0 ||
	    assign->argc > MAX_ARGV) {
		errno = EINVAL;
		return -1;
	}
	for (uint32_t i = 0; i < assign->argc; i++) {
		if (dec_str(&cursor, assign->argv[i], MAX_ARG_LEN) != 0) {
			return -1;
		}
	}
	if (dec_u32(&cursor, &assign->cores) != 0 || dec_u64(&cursor, &assign->ram_mb) != 0 ||
	    dec_u32(&cursor, &assign->time_sec) != 0 || dec_u32(&cursor, &assign->prio) != 0 ||
	    dec_u32(&cursor, &assign->gpu) != 0 || dec_u8(&cursor, &assign->use_path) != 0 ||
	    dec_u32(&cursor, &assign->binary_len) != 0 || assign->binary_len > MSG_BINARY_CAP ||
	    dec_bytes_ptr(&cursor, &assign->binary, assign->binary_len) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (assign->binary_len == 0) {
		assign->binary = NULL;
	}
	return dec_done(&cursor);
}

// Encode a terminal job report from a worker.
int msg_encode_job_result(uint8_t *buf, size_t cap, size_t *out_len, const msg_job_result_t *result)
{
	enc_cursor_t cursor;

	if (out_len == NULL || result == NULL || strlen(result->reason) >= MAX_REASON_LEN) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, result->job_id) != 0 ||
	    enc_u32(&cursor, (uint32_t)result->exit_code) != 0 ||
	    enc_u32(&cursor, (uint32_t)result->term_signal) != 0 ||
	    enc_str(&cursor, result->reason) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a terminal job report from a worker.
int msg_decode_job_result(const uint8_t *buf, uint32_t len, msg_job_result_t *result)
{
	dec_cursor_t cursor;
	uint32_t raw;

	if (result == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, &result->job_id) != 0 || dec_u32(&cursor, &raw) != 0) {
		return -1;
	}
	result->exit_code = (int32_t)raw;
	if (dec_u32(&cursor, &raw) != 0) {
		return -1;
	}
	result->term_signal = (int32_t)raw;
	if (dec_str(&cursor, result->reason, MAX_REASON_LEN) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}

// Encode a heartbeat sample.
int msg_encode_heartbeat(uint8_t *buf, size_t cap, size_t *out_len, uint32_t cpu_load_pct,
                         uint32_t running_jobs)
{
	enc_cursor_t cursor;

	if (out_len == NULL) {
		errno = EINVAL;
		return -1;
	}
	enc_init(&cursor, buf, cap);
	if (enc_u32(&cursor, cpu_load_pct) != 0 || enc_u32(&cursor, running_jobs) != 0) {
		return -1;
	}
	*out_len = cursor.pos;
	return 0;
}

// Decode a heartbeat sample.
int msg_decode_heartbeat(const uint8_t *buf, uint32_t len, uint32_t *cpu_load_pct,
                         uint32_t *running_jobs)
{
	dec_cursor_t cursor;

	if (cpu_load_pct == NULL || running_jobs == NULL) {
		errno = EINVAL;
		return -1;
	}
	dec_init(&cursor, buf, len);
	if (dec_u32(&cursor, cpu_load_pct) != 0 || dec_u32(&cursor, running_jobs) != 0) {
		return -1;
	}
	return dec_done(&cursor);
}
