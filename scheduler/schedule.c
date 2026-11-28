#include "schedule.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../common/log.h"
#include "../common/messages.h"
#include "../common/protocol.h"

// Find a job by id while the caller holds the state lock.
static job_t *find_job_locked(scheduler_state_t *state, uint32_t job_id)
{
	for (uint32_t i = 0; i < state->job_count; i++) {
		if (state->jobs[i].id == job_id) {
			return &state->jobs[i];
		}
	}
	return NULL;
}

// Apply priority aging once per ten seconds of queue wait time.
static uint32_t effective_priority(const scheduler_state_t *state, const job_t *job)
{
	int64_t waited;
	uint32_t age;
	uint32_t prio;

	if (state->policy != POLICY_PRIORITY) {
		return job->prio;
	}
	waited = (int64_t)time(NULL) - job->queued_ts;
	age = waited >= 10 ? (uint32_t)(waited / 10) : 0u;
	prio = job->prio + age;
	if (prio < 1u) {
		prio = 1u;
	}
	return prio;
}

// Order PRIORITY jobs by effective priority, then age, then submission order.
static bool comes_before_locked(const scheduler_state_t *state, const job_t *a, const job_t *b)
{
	uint32_t prio_a = effective_priority(state, a);
	uint32_t prio_b = effective_priority(state, b);

	if (prio_a != prio_b) {
		return prio_a < prio_b;
	}
	if (a->queued_ts != b->queued_ts) {
		return a->queued_ts < b->queued_ts;
	}
	return a->id < b->id;
}

// Pick an ALIVE worker with enough free resources, preferring the most free cores.
static worker_t *choose_worker_locked(scheduler_state_t *state, const job_t *job)
{
	worker_t *best = NULL;
	uint32_t best_score = 0;

	for (uint32_t i = 0; i < state->worker_count; i++) {
		worker_t *worker = &state->workers[i];
		uint32_t score;

		if (worker->state != WORKER_ALIVE || worker->fd < 0 || worker->free_cores < job->cores ||
		    worker->free_ram_mb < job->ram_mb || worker->free_gpu < job->gpu) {
			continue;
		}
		score = worker->free_cores;
		if (best == NULL || score > best_score || (score == best_score && worker->id < best->id)) {
			best = worker;
			best_score = score;
		}
	}
	return best;
}

// Read a submitted executable into a bounded heap buffer.
static int read_executable(const char *path, uint8_t **out, uint32_t *out_len)
{
	struct stat info;
	FILE *file;
	uint8_t *buffer;
	size_t read_len;

	if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
	    (uint64_t)info.st_size > MSG_BINARY_CAP) {
		errno = EINVAL;
		return -1;
	}
	buffer = malloc((size_t)info.st_size + 1u);
	if (buffer == NULL) {
		return -1;
	}
	file = fopen(path, "rb");
	if (file == NULL) {
		free(buffer);
		return -1;
	}
	read_len = fread(buffer, 1, (size_t)info.st_size, file);
	fclose(file);
	if (read_len != (size_t)info.st_size) {
		free(buffer);
		errno = EIO;
		return -1;
	}
	*out = buffer;
	*out_len = (uint32_t)read_len;
	return 0;
}

// Undo a committed assignment after staging or sending fails.
static void rollback_assignment(scheduler_state_t *state, uint32_t job_id)
{
	if (pthread_mutex_lock(&state->lock) != 0) {
		return;
	}
	(void)scheduler_release_assignment_locked(state, job_id);
	pthread_mutex_unlock(&state->lock);
	log_scheduling_decision((int32_t)job_id, -1, "assignment failed before worker accepted it");
}

// Reserve resources, transfer the executable, and send one job assignment.
static int assign_one_job(scheduler_state_t *state, uint32_t job_id)
{
	msg_job_assign_t assignment;
	uint8_t *payload = NULL;
	uint8_t *binary = NULL;
	char path[MAX_PATH_LEN];
	uint32_t binary_len = 0;
	size_t payload_len = 0;
	int32_t worker_id = -1;
	int worker_fd = -1;
	int result = 1;
	char reason[MAX_REASON_LEN];

	memset(&assignment, 0, sizeof(assignment));
	memset(path, 0, sizeof(path));
	if (pthread_mutex_lock(&state->lock) != 0) {
		return -1;
	}
	job_t *job = find_job_locked(state, job_id);
	if (job == NULL || job->state != JOB_QUEUED || job->preempt_hold) {
		pthread_mutex_unlock(&state->lock);
		return 1;
	}
	worker_t *worker = choose_worker_locked(state, job);
	if (worker == NULL) {
		snprintf(reason, sizeof(reason), "no worker with cores=%u ram=%llu gpu=%u",
		         job->cores, (unsigned long long)job->ram_mb, job->gpu);
		pthread_mutex_unlock(&state->lock);
		log_scheduling_decision((int32_t)job_id, -1, reason);
		return 0;
	}
	worker_id = worker->id;
	worker_fd = worker->fd;
	worker->free_cores -= job->cores;
	worker->free_ram_mb -= job->ram_mb;
	worker->free_gpu -= job->gpu;
	worker->running_jobs++;
	job->attempts++;
	job->assigned_worker = worker_id;
	job->start_ts = (int64_t)time(NULL);
	assignment.job_id = job_id;
	assignment.argc = job->argc;
	for (uint32_t i = 0; i < job->argc; i++) {
		memcpy(assignment.argv[i], job->argv[i], sizeof(assignment.argv[i]));
	}
	assignment.cores = job->cores;
	assignment.ram_mb = job->ram_mb;
	assignment.time_sec = job->time_sec;
	assignment.prio = job->prio;
	assignment.gpu = job->gpu;
	assignment.use_path = strchr(job->argv[0], '/') == NULL ? 1u : 0u;
	if (assignment.use_path == 0u) {
		snprintf(path, sizeof(path), "%s", job->argv[0]);
	}
	snprintf(reason, sizeof(reason), "enough free resources on worker %d", (int)worker_id);
	(void)scheduler_set_job_state_reason_locked(state, job_id, JOB_RUNNING, NULL);
	pthread_mutex_unlock(&state->lock);
	log_scheduling_decision((int32_t)job_id, worker_id, reason);

	payload = malloc(MSG_JOB_ASSIGN_CAP);
	if (payload == NULL) {
		rollback_assignment(state, job_id);
		return -1;
	}
	if (assignment.use_path == 0u && read_executable(path, &binary, &binary_len) != 0) {
		rollback_assignment(state, job_id);
		free(payload);
		return -1;
	}
	assignment.binary = binary;
	assignment.binary_len = binary_len;
	if (msg_encode_job_assign(payload, MSG_JOB_ASSIGN_CAP, &payload_len, &assignment) != 0 ||
	    send_msg(worker_fd, MSG_JOB_ASSIGN, payload, (uint32_t)payload_len) != 0) {
		rollback_assignment(state, job_id);
		result = -1;
	}
	free(binary);
	free(payload);
	return result;
}

// Run one bounded scheduling pass without holding the state lock during I/O.
void scheduler_run_pass(scheduler_state_t *state)
{
	uint32_t ids[QUEUE_CAPACITY];
	size_t count = 0;

	if (state == NULL || pthread_mutex_lock(&state->lock) != 0) {
		return;
	}
	for (uint32_t i = 0; i < state->queue_count; i++) {
		uint32_t id = state->queue[(state->queue_head + i) % QUEUE_CAPACITY];
		job_t *job = find_job_locked(state, id);

		if (job != NULL && job->state == JOB_QUEUED && !job->preempt_hold) {
			ids[count++] = id;
		}
	}
	if (state->policy == POLICY_PRIORITY) {
		for (size_t i = 1; i < count; i++) {
			uint32_t id = ids[i];
			job_t *job = find_job_locked(state, id);
			size_t j = i;

			while (j > 0 && job != NULL &&
			       comes_before_locked(state, job, find_job_locked(state, ids[j - 1]))) {
				ids[j] = ids[j - 1];
				j--;
			}
			ids[j] = id;
		}
	}
	pthread_mutex_unlock(&state->lock);

	for (size_t i = 0; i < count; i++) {
		int rc = assign_one_job(state, ids[i]);

		if (rc == 0 && state->policy == POLICY_FIFO) {
			break;
		}
	}
}
