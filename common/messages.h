#ifndef OSN_MESSAGES_H
#define OSN_MESSAGES_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

// Define the maximum text length carried in an error payload.
#define MSG_ERROR_TEXT_CAP 255u

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
