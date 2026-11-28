#include "state.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../common/log.h"

static scheduler_state_t g_state;

// Return the job with this id, or NULL, while the caller holds the lock.
static job_t *find_job_locked(scheduler_state_t *state, uint32_t job_id)
{
	for (uint32_t i = 0; i < state->job_count; i++) {
		if (state->jobs[i].id == job_id) {
			return &state->jobs[i];
		}
	}
	return NULL;
}

// Build the [Event] text for one job transition, matching the spec's wording.
static void job_event_text(const job_t *job, job_state_t old_state, job_state_t new_state,
                           const char *reason, char *out, size_t cap)
{
	const char *old_name = job_state_name(old_state);
	const char *new_name = job_state_name(new_state);

	switch (new_state) {
	case JOB_RUNNING:
		if (job->attempts > 1) {
			snprintf(out, cap, "Job %u (attempt %u) RUNNING on Worker %d.", job->id,
			         (unsigned int)job->attempts, (int)job->assigned_worker);
		} else {
			snprintf(out, cap, "Job %u RUNNING on Worker %d.", job->id, (int)job->assigned_worker);
		}
		return;
	case JOB_COMPLETED:
		snprintf(out, cap, "Job %u COMPLETED successfully.", job->id);
		return;
	case JOB_FAILED:
	case JOB_FAILED_PERMANENT:
		snprintf(out, cap, "Job %u %s. Reason: %s.", job->id, new_name,
		         reason != NULL ? reason : "unknown");
		return;
	case JOB_QUEUED:
		if (old_state == JOB_LOST) {
			snprintf(out, cap, "Job %u (attempt %u) moved back to QUEUED.", job->id,
			         (unsigned int)job->attempts);
			return;
		}
		if (old_state == JOB_WAITING) {
			snprintf(out, cap, "Job %u moved from WAITING to QUEUED (dependency satisfied).", job->id);
			return;
		}
		break;
	default:
		break;
	}
	if (reason != NULL) {
		snprintf(out, cap, "Job %u %s -> %s (%s).", job->id, old_name, new_name, reason);
	} else {
		snprintf(out, cap, "Job %u %s -> %s.", job->id, old_name, new_name);
	}
}

// Warn each distinct owner of a job RUNNING on a worker that went SUSPECTED or DEAD.
static void warn_job_owners_locked(scheduler_state_t *state, int32_t worker_id, worker_state_t new_state)
{
	char text[MSG_TEXT_CAP + 1u];

	if (new_state == WORKER_SUSPECTED) {
		snprintf(text, sizeof(text), "WARNING: Worker %d SUSPECTED (missed heartbeat).", (int)worker_id);
	} else if (new_state == WORKER_DEAD) {
		snprintf(text, sizeof(text), "WARNING: Worker %d DEAD.", (int)worker_id);
	} else {
		return;
	}
	for (uint32_t i = 0; i < state->job_count; i++) {
		const job_t *job = &state->jobs[i];
		bool already_told = false;

		if (job->state != JOB_RUNNING || job->assigned_worker != worker_id) {
			continue;
		}
		// Send one warning per user even if they have several jobs on that worker.
		for (uint32_t j = 0; j < i && !already_told; j++) {
			const job_t *prev = &state->jobs[j];

			already_told = prev->state == JOB_RUNNING && prev->assigned_worker == worker_id &&
			               strcmp(prev->owner, job->owner) == 0;
		}
		if (!already_told) {
			scheduler_notify_user_locked(state, job->owner, text);
		}
	}
}

// Remove one job id from the ring queue, keeping the order of the others.
static void queue_remove_locked(scheduler_state_t *state, uint32_t job_id)
{
	uint32_t kept = 0;
	uint32_t count = state->queue_count;
	uint32_t head = state->queue_head;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t id = state->queue[(head + i) % QUEUE_CAPACITY];

		if (id != job_id) {
			state->queue[(head + kept) % QUEUE_CAPACITY] = id;
			kept++;
		}
	}
	state->queue_count = kept;
	state->queue_tail = (head + kept) % QUEUE_CAPACITY;
}

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
	for (uint32_t i = 0; i < MAX_CLIENT_SESSIONS; i++) {
		rc = pthread_mutex_init(&state->sessions[i].send_lock, NULL);
		if (rc != 0) {
			while (i-- > 0) {
				pthread_mutex_destroy(&state->sessions[i].send_lock);
			}
			pthread_cond_destroy(&state->work_cond);
			pthread_mutex_destroy(&state->lock);
			errno = rc;
			return -1;
		}
		state->sessions[i].fd = -1;
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
	for (uint32_t i = 0; i < MAX_CLIENT_SESSIONS; i++) {
		pthread_mutex_destroy(&state->sessions[i].send_lock);
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
			warn_job_owners_locked(state, worker_id, new_state);
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

// Change a job state and log the transition while the caller holds the lock.
int scheduler_set_job_state_locked(scheduler_state_t *state, uint32_t job_id, job_state_t new_state)
{
	return scheduler_set_job_state_reason_locked(state, job_id, new_state, NULL);
}

// Change a job state with a reason, log it, and push an [Event] to the job owner.
int scheduler_set_job_state_reason_locked(scheduler_state_t *state, uint32_t job_id,
                                          job_state_t new_state, const char *reason)
{
	char text[MSG_TEXT_CAP + 1u];
	job_state_t old_state;
	job_t *job;

	if (state == NULL) {
		errno = EINVAL;
		return -1;
	}
	job = find_job_locked(state, job_id);
	if (job == NULL) {
		errno = ENOENT;
		return -1;
	}
	old_state = job->state;
	if (old_state == new_state) {
		return 0;
	}
	job->state = new_state;
	if (reason != NULL) {
		log_sched("Job %u %s -> %s (%s)", job_id, job_state_name(old_state),
		          job_state_name(new_state), reason);
	} else {
		log_job_state(job_id, old_state, new_state);
	}
	// Only the job's owner hears about its state changes.
	job_event_text(job, old_state, new_state, reason, text, sizeof(text));
	scheduler_notify_user_locked(state, job->owner, text);
	return 0;
}

// Register a logged-in client session; returns its slot index or -1 when full.
int scheduler_add_session(scheduler_state_t *state, int fd, const char *user, uint32_t *out_id)
{
	int slot = -1;

	if (state == NULL || user == NULL || out_id == NULL || pthread_mutex_lock(&state->lock) != 0) {
		return -1;
	}
	for (uint32_t i = 0; i < MAX_CLIENT_SESSIONS; i++) {
		client_session_t *s = &state->sessions[i];

		if (!s->active) {
			s->active = true;
			s->id = state->next_session_id++;
			s->fd = fd;
			snprintf(s->user, sizeof(s->user), "%s", user);
			*out_id = s->id;
			slot = (int)i;
			break;
		}
	}
	pthread_mutex_unlock(&state->lock);
	return slot;
}

// Deactivate a client session so no more events are sent to its socket.
void scheduler_remove_session(scheduler_state_t *state, int slot)
{
	if (state == NULL || slot < 0 || slot >= (int)MAX_CLIENT_SESSIONS ||
	    pthread_mutex_lock(&state->lock) != 0) {
		return;
	}
	// Event senders hold the state lock while sending, so after this no one uses the fd.
	state->sessions[slot].active = false;
	state->sessions[slot].fd = -1;
	pthread_mutex_unlock(&state->lock);
}

// Send one frame on a client session, serialized against event pushes.
int scheduler_session_send(scheduler_state_t *state, int slot, msg_type_t type,
                           const void *payload, uint32_t length)
{
	client_session_t *s;
	int rc;

	if (state == NULL || slot < 0 || slot >= (int)MAX_CLIENT_SESSIONS) {
		errno = EINVAL;
		return -1;
	}
	s = &state->sessions[slot];
	if (pthread_mutex_lock(&s->send_lock) != 0) {
		return -1;
	}
	rc = send_msg(s->fd, type, payload, length);
	pthread_mutex_unlock(&s->send_lock);
	return rc;
}

// Push an [Event] line to every session logged in as user (caller holds the lock).
void scheduler_notify_user_locked(scheduler_state_t *state, const char *user, const char *text)
{
	uint8_t payload[MSG_TEXT_CAP + 2u];
	size_t length = 0;

	if (state == NULL || user == NULL || text == NULL ||
	    msg_encode_event(payload, sizeof(payload), &length, text) != 0) {
		return;
	}
	// Duplicate usernames share events: every session with that name is the owner.
	for (uint32_t i = 0; i < MAX_CLIENT_SESSIONS; i++) {
		client_session_t *s = &state->sessions[i];

		if (!s->active || strcmp(s->user, user) != 0) {
			continue;
		}
		if (pthread_mutex_lock(&s->send_lock) == 0) {
			(void)send_msg(s->fd, MSG_EVENT, payload, (uint32_t)length);
			pthread_mutex_unlock(&s->send_lock);
		}
	}
}

// Create and enqueue a job owned by user; returns 0 or an msg_error_code_t.
int scheduler_submit_job(scheduler_state_t *state, const msg_submit_t *submit, const char *user,
                         uint32_t *out_job_id)
{
	job_t *job;

	if (state == NULL || submit == NULL || user == NULL || out_job_id == NULL) {
		return MSG_ERR_BAD_REQUEST;
	}
	if (pthread_mutex_lock(&state->lock) != 0) {
		return MSG_ERR_BAD_REQUEST;
	}
	// Reject immediately instead of blocking when the bounded queue is full.
	if (state->queue_count >= QUEUE_CAPACITY) {
		pthread_mutex_unlock(&state->lock);
		return MSG_ERR_QUEUE_FULL;
	}
	if (state->job_count >= QUEUE_CAPACITY) {
		pthread_mutex_unlock(&state->lock);
		return MSG_ERR_TABLE_FULL;
	}
	job = &state->jobs[state->job_count++];
	memset(job, 0, sizeof(*job));
	job->id = state->next_job_id++;
	snprintf(job->owner, sizeof(job->owner), "%s", user);
	job->argc = submit->argc;
	for (uint32_t i = 0; i < submit->argc; i++) {
		memcpy(job->argv[i], submit->argv[i], sizeof(job->argv[i]));
	}
	job->cores = submit->cores;
	job->ram_mb = submit->ram_mb;
	job->time_sec = submit->time_sec;
	job->prio = submit->prio;
	job->gpu = submit->gpu;
	job->state = JOB_QUEUED;
	job->submit_ts = (int64_t)time(NULL);
	job->assigned_worker = -1;
	job->migrated_from = -1;
	state->queue[state->queue_tail] = job->id;
	state->queue_tail = (state->queue_tail + 1u) % QUEUE_CAPACITY;
	state->queue_count++;
	*out_job_id = job->id;
	log_sched("Job %u SUBMITTED by %s: %s (cores=%u ram=%lluMB time=%us prio=%u gpu=%u)",
	          job->id, user, job->argv[0], job->cores, (unsigned long long)job->ram_mb,
	          job->time_sec, job->prio, job->gpu);
	scheduler_signal_work_locked(state);
	pthread_mutex_unlock(&state->lock);
	return 0;
}

// Cancel a job on behalf of requester; returns 0 or an msg_error_code_t with text.
int scheduler_cancel_job(scheduler_state_t *state, uint32_t job_id, const char *requester,
                         job_state_t *old_state, char *err, size_t err_cap)
{
	char reason[MAX_REASON_LEN];
	job_t *job;
	int rc = 0;

	if (state == NULL || requester == NULL || old_state == NULL || err == NULL ||
	    pthread_mutex_lock(&state->lock) != 0) {
		return MSG_ERR_BAD_REQUEST;
	}
	job = find_job_locked(state, job_id);
	if (job == NULL) {
		snprintf(err, err_cap, "Job %u does not exist", job_id);
		pthread_mutex_unlock(&state->lock);
		return MSG_ERR_NO_SUCH_JOB;
	}
	*old_state = job->state;
	snprintf(reason, sizeof(reason), "requested by %s", requester);
	switch (job->state) {
	case JOB_QUEUED:
		queue_remove_locked(state, job_id);
		(void)scheduler_set_job_state_reason_locked(state, job_id, JOB_CANCELLED, reason);
		break;
	case JOB_WAITING:
		(void)scheduler_set_job_state_reason_locked(state, job_id, JOB_CANCELLED, reason);
		break;
	case JOB_RUNNING:
	case JOB_LOST:
		// Killing on the worker needs the job-cancel path from TODO 45.
		snprintf(err, err_cap, "Job %u is %s; cancelling a running job is not implemented yet",
		         job_id, job_state_name(job->state));
		rc = MSG_ERR_UNSUPPORTED;
		break;
	default:
		snprintf(err, err_cap, "Job %u is already %s", job_id, job_state_name(job->state));
		rc = MSG_ERR_INVALID_STATE;
		break;
	}
	pthread_mutex_unlock(&state->lock);
	return rc;
}

// Fill a status reply for one job; returns 0 or MSG_ERR_NO_SUCH_JOB.
int scheduler_fill_status(scheduler_state_t *state, uint32_t job_id, msg_status_t *out)
{
	const job_t *job;
	size_t used = 0;

	if (state == NULL || out == NULL || pthread_mutex_lock(&state->lock) != 0) {
		return MSG_ERR_BAD_REQUEST;
	}
	job = find_job_locked(state, job_id);
	if (job == NULL) {
		pthread_mutex_unlock(&state->lock);
		return MSG_ERR_NO_SUCH_JOB;
	}
	memset(out, 0, sizeof(*out));
	out->job_id = job->id;
	snprintf(out->owner, sizeof(out->owner), "%s", job->owner);
	out->state = (uint8_t)job->state;
	out->worker_id = job->assigned_worker;
	out->cores = job->cores;
	out->ram_mb = job->ram_mb;
	out->time_sec = job->time_sec;
	out->prio = job->prio;
	out->gpu = job->gpu;
	out->attempts = job->attempts;
	// Join argv with spaces, truncating to the wire text cap.
	for (uint32_t i = 0; i < job->argc && used < MSG_TEXT_CAP; i++) {
		int n = snprintf(out->command + used, sizeof(out->command) - used, "%s%s",
		                 i > 0 ? " " : "", job->argv[i]);

		if (n < 0) {
			break;
		}
		used += (size_t)n;
	}
	snprintf(out->reason, sizeof(out->reason), "%s", job->telemetry.reason);
	pthread_mutex_unlock(&state->lock);
	return 0;
}

// Fill nodes rows for every registered worker; returns the row count.
uint32_t scheduler_fill_nodes(scheduler_state_t *state, msg_node_t *out, uint32_t cap)
{
	uint32_t count = 0;

	if (state == NULL || out == NULL || pthread_mutex_lock(&state->lock) != 0) {
		return 0;
	}
	for (uint32_t i = 0; i < state->worker_count && count < cap; i++) {
		const worker_t *w = &state->workers[i];
		msg_node_t *n = &out[count++];

		n->id = w->id;
		n->state = (uint8_t)w->state;
		n->free_cores = w->free_cores;
		n->total_cores = w->total_cores;
		n->free_ram_mb = w->free_ram_mb;
		n->total_ram_mb = w->total_ram_mb;
		n->free_gpu = w->free_gpu;
		n->total_gpu = w->total_gpu;
		n->running_jobs = w->running_jobs;
	}
	pthread_mutex_unlock(&state->lock);
	return count;
}

// Copy a job's buffered output into dst (JOB_OUTPUT_CAP bytes); returns 0 or MSG_ERR_NO_SUCH_JOB.
int scheduler_copy_output(scheduler_state_t *state, uint32_t job_id, uint8_t *dst, uint32_t *len)
{
	const job_t *job;

	if (state == NULL || dst == NULL || len == NULL || pthread_mutex_lock(&state->lock) != 0) {
		return MSG_ERR_BAD_REQUEST;
	}
	job = find_job_locked(state, job_id);
	if (job == NULL) {
		pthread_mutex_unlock(&state->lock);
		return MSG_ERR_NO_SUCH_JOB;
	}
	*len = job->output_len <= JOB_OUTPUT_CAP ? job->output_len : JOB_OUTPUT_CAP;
	memcpy(dst, job->output, *len);
	pthread_mutex_unlock(&state->lock);
	return 0;
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
