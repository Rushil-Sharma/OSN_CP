#ifndef OSN_SCHEDULER_STATE_H
#define OSN_SCHEDULER_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/messages.h"
#include "../common/protocol.h"
#include "../common/types.h"

// Define capacities for log buffering, subscription placeholders, and client sessions.
#define MAX_LOG_SUBSCRIBERS 64u
#define LOG_BUFFER_CAP (64u * 1024u)
#define MAX_CLIENT_SESSIONS 256u

// Track one logged-in client connection; send_lock keeps frames from interleaving.
typedef struct {
	bool active;
	uint32_t id;
	int fd;
	char user[MAX_USERNAME];
	pthread_mutex_t send_lock;
} client_session_t;

// Define the scheduler ordering policies accepted by the CLI.
typedef enum {
	POLICY_FIFO,
	POLICY_PRIORITY
} policy_t;

// Store all shared scheduler state behind one mutex and one condition variable.
typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t work_cond;
	policy_t policy;
	bool stop;
	job_t jobs[QUEUE_CAPACITY];
	uint32_t job_count;
	uint32_t next_job_id;
	uint32_t queue[QUEUE_CAPACITY];
	uint32_t queue_head;
	uint32_t queue_tail;
	uint32_t queue_count;
	worker_t workers[MAX_WORKERS];
	uint32_t worker_count;
	int32_t next_worker_id;
	uint32_t next_session_id;
	char log_buffer[LOG_BUFFER_CAP];
	uint32_t log_len;
	int32_t subscribers[MAX_LOG_SUBSCRIBERS];
	uint32_t subscriber_count;
	client_session_t sessions[MAX_CLIENT_SESSIONS];
} scheduler_state_t;

// Return the process-wide scheduler state instance.
scheduler_state_t *scheduler_global_state(void);

// Initialize scheduler state and its synchronization primitives.
int scheduler_state_init(scheduler_state_t *state, policy_t policy);

// Destroy scheduler synchronization primitives.
void scheduler_state_destroy(scheduler_state_t *state);

// Register a worker snapshot and return its assigned id.
int scheduler_register_worker(scheduler_state_t *state, const worker_t *snapshot, int fd, int32_t *out_id);

// Change a worker state and log the transition while the caller holds the lock.
int scheduler_set_worker_state_locked(scheduler_state_t *state, int32_t worker_id, worker_state_t new_state);

// Change a job state and log the transition while the caller holds the lock.
int scheduler_set_job_state_locked(scheduler_state_t *state, uint32_t job_id, job_state_t new_state);

// Change a job state with a reason, log it, and push an [Event] to the job owner.
int scheduler_set_job_state_reason_locked(scheduler_state_t *state, uint32_t job_id,
                                          job_state_t new_state, const char *reason);

// Register a logged-in client session; returns its slot index or -1 when full.
int scheduler_add_session(scheduler_state_t *state, int fd, const char *user, uint32_t *out_id);

// Deactivate a client session so no more events are sent to its socket.
void scheduler_remove_session(scheduler_state_t *state, int slot);

// Send one frame on a client session, serialized against event pushes.
int scheduler_session_send(scheduler_state_t *state, int slot, msg_type_t type,
                           const void *payload, uint32_t length);

// Push an [Event] line to every session logged in as user (caller holds the lock).
void scheduler_notify_user_locked(scheduler_state_t *state, const char *user, const char *text);

// Create and enqueue a job owned by user; returns 0 or an msg_error_code_t.
int scheduler_submit_job(scheduler_state_t *state, const msg_submit_t *submit, const char *user,
                         uint32_t *out_job_id);

// Cancel a job on behalf of requester; returns 0 or an msg_error_code_t with text.
int scheduler_cancel_job(scheduler_state_t *state, uint32_t job_id, const char *requester,
                         job_state_t *old_state, char *err, size_t err_cap);

// Fill a status reply for one job; returns 0 or MSG_ERR_NO_SUCH_JOB.
int scheduler_fill_status(scheduler_state_t *state, uint32_t job_id, msg_status_t *out);

// Fill nodes rows for every registered worker; returns the row count.
uint32_t scheduler_fill_nodes(scheduler_state_t *state, msg_node_t *out, uint32_t cap);

// Copy a job's buffered output into dst (JOB_OUTPUT_CAP bytes); returns 0 or MSG_ERR_NO_SUCH_JOB.
int scheduler_copy_output(scheduler_state_t *state, uint32_t job_id, uint8_t *dst, uint32_t *len);

// Allocate the next client session id under the state lock.
uint32_t scheduler_next_session_id(scheduler_state_t *state);

// Clear the socket fd associated with a disconnected worker.
void scheduler_mark_worker_fd_closed(scheduler_state_t *state, int fd);

// Wake the scheduling thread while the caller holds the state lock.
void scheduler_signal_work_locked(scheduler_state_t *state);

#endif
