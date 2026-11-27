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
