#ifndef OSN_SCHEDULER_STATE_H
#define OSN_SCHEDULER_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/types.h"

// Define capacities for log buffering and subscription placeholders.
#define MAX_LOG_SUBSCRIBERS 64u
#define LOG_BUFFER_CAP (64u * 1024u)

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

// Allocate the next client session id under the state lock.
uint32_t scheduler_next_session_id(scheduler_state_t *state);

// Clear the socket fd associated with a disconnected worker.
void scheduler_mark_worker_fd_closed(scheduler_state_t *state, int fd);

// Wake the scheduling thread while the caller holds the state lock.
void scheduler_signal_work_locked(scheduler_state_t *state);

#endif
