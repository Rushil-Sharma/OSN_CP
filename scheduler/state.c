#include "state.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "../common/log.h"

static scheduler_state_t g_state;

// Return the process-wide scheduler state instance.
scheduler_state_t *scheduler_global_state(void)
{
	return &g_state;
}

// Initialize scheduler state and its synchronization primitives.
int scheduler_state_init(scheduler_state_t *state, policy_t policy)
{
	int rc;

	if (state == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(state, 0, sizeof(*state));
	rc = pthread_mutex_init(&state->lock, NULL);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	rc = pthread_cond_init(&state->work_cond, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&state->lock);
		errno = rc;
		return -1;
	}
	state->policy = policy;
	state->next_job_id = 1;
	state->next_worker_id = 1;
	state->next_session_id = 1;
	return 0;
}

// Destroy scheduler synchronization primitives.
void scheduler_state_destroy(scheduler_state_t *state)
{
	if (state == NULL) {
		return;
	}
	pthread_cond_destroy(&state->work_cond);
	pthread_mutex_destroy(&state->lock);
}

// Register a worker snapshot and return its assigned id.
int scheduler_register_worker(scheduler_state_t *state, const worker_t *snapshot, int fd, int32_t *out_id)
{
	worker_t *worker;
	int rc;

	if (state == NULL || snapshot == NULL || out_id == NULL) {
		errno = EINVAL;
		return -1;
	}
	rc = pthread_mutex_lock(&state->lock);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	if (state->worker_count >= MAX_WORKERS) {
		pthread_mutex_unlock(&state->lock);
		errno = ENOSPC;
		return -1;
	}
	worker = &state->workers[state->worker_count++];
	*worker = *snapshot;
	worker->id = state->next_worker_id++;
	worker->fd = fd;
	worker->state = WORKER_DEAD;
	*out_id = worker->id;
	(void)scheduler_set_worker_state_locked(state, worker->id, WORKER_ALIVE);
	scheduler_signal_work_locked(state);
	pthread_mutex_unlock(&state->lock);
	return 0;
}

// Change a worker state and log the transition while the caller holds the lock.
int scheduler_set_worker_state_locked(scheduler_state_t *state, int32_t worker_id, worker_state_t new_state)
{
	uint32_t i;

	if (state == NULL) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < state->worker_count; i++) {
		if (state->workers[i].id == worker_id) {
			worker_state_t old_state = state->workers[i].state;

			if (old_state == new_state) {
				return 0;
			}
			state->workers[i].state = new_state;
			log_worker_state(worker_id, old_state, new_state);
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

// Change a job state and log the transition while the caller holds the lock.
int scheduler_set_job_state_locked(scheduler_state_t *state, uint32_t job_id, job_state_t new_state)
{
	uint32_t i;

	if (state == NULL) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < state->job_count; i++) {
		if (state->jobs[i].id == job_id) {
			job_state_t old_state = state->jobs[i].state;

			if (old_state == new_state) {
				return 0;
			}
			state->jobs[i].state = new_state;
			log_job_state(job_id, old_state, new_state);
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

// Allocate the next client session id under the state lock.
uint32_t scheduler_next_session_id(scheduler_state_t *state)
{
	uint32_t session_id;
	int rc;

	if (state == NULL) {
		return 0;
	}
	rc = pthread_mutex_lock(&state->lock);
	if (rc != 0) {
		return 0;
	}
	session_id = state->next_session_id++;
	pthread_mutex_unlock(&state->lock);
	return session_id;
}

// Clear the socket fd associated with a disconnected worker.
void scheduler_mark_worker_fd_closed(scheduler_state_t *state, int fd)
{
	uint32_t i;
	int rc;

	if (state == NULL) {
		return;
	}
	rc = pthread_mutex_lock(&state->lock);
	if (rc != 0) {
		return;
	}
	for (i = 0; i < state->worker_count; i++) {
		if (state->workers[i].fd == fd) {
			int32_t worker_id = state->workers[i].id;

			state->workers[i].fd = -1;
			(void)scheduler_set_worker_state_locked(state, worker_id, WORKER_DEAD);
			break;
		}
	}
	pthread_mutex_unlock(&state->lock);
}

// Wake the scheduling thread while the caller holds the state lock.
void scheduler_signal_work_locked(scheduler_state_t *state)
{
	if (state != NULL) {
		pthread_cond_signal(&state->work_cond);
	}
}
