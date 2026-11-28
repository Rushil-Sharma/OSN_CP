#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Serialize complete log lines across scheduler session threads.
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

// Return the printable name of a worker state.
static const char *worker_state_name(worker_state_t state)
{
	switch (state) {
	case WORKER_ALIVE: return "ALIVE";
	case WORKER_SUSPECTED: return "SUSPECTED";
	case WORKER_DEAD: return "DEAD";
	case WORKER_DRAINING: return "DRAINING";
	default: return "UNKNOWN";
	}
}

// Return the printable name of a job state.
static const char *job_state_name(job_state_t state)
{
	switch (state) {
	case JOB_QUEUED: return "QUEUED";
	case JOB_RUNNING: return "RUNNING";
	case JOB_COMPLETED: return "COMPLETED";
	case JOB_FAILED: return "FAILED";
	case JOB_CANCELLED: return "CANCELLED";
	case JOB_WAITING: return "WAITING";
	case JOB_LOST: return "LOST";
	case JOB_FAILED_PERMANENT: return "FAILED_PERMANENT";
	default: return "UNKNOWN";
	}
}

// Print one timestamped, tagged, thread-safe log line.
void log_line(log_source_t source, int32_t worker_id, const char *fmt, ...)
{
	struct tm local = {0};
	time_t now = time(NULL);
	va_list args;

	if (fmt == NULL) {
		return;
	}
	if (now != (time_t)-1) {
		(void)localtime_r(&now, &local);
	}
	if (pthread_mutex_lock(&g_log_lock) != 0) {
		return;
	}
	printf("[%02d:%02d:%02d] ", local.tm_hour, local.tm_min, local.tm_sec);
	if (source == LOG_WORKER) {
		if (worker_id < 0) {
			fputs("[WORKER] ", stdout);
		} else {
			printf("[WORKER %d] ", (int)worker_id);
		}
	} else {
		fputs("[SCHEDULER] ", stdout);
	}
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	fputc('\n', stdout);
	fflush(stdout);
	(void)pthread_mutex_unlock(&g_log_lock);
}

// Log a worker state transition.
void log_worker_state(int32_t worker_id, worker_state_t old_state, worker_state_t new_state)
{
	log_worker(worker_id, "state %s -> %s", worker_state_name(old_state), worker_state_name(new_state));
}

// Log a job state transition.
void log_job_state(uint32_t job_id, job_state_t old_state, job_state_t new_state)
{
	log_sched("job %u state %s -> %s", job_id, job_state_name(old_state), job_state_name(new_state));
}

// Log a scheduling decision with the reason the worker was chosen.
void log_scheduling_decision(uint32_t job_id, int32_t worker_id, const char *why)
{
	if (worker_id < 0) {
		log_sched("scheduling pass: %s", why != NULL ? why : "no decision");
		return;
	}
	log_sched("job %u -> worker %d (%s)", job_id, (int)worker_id, why != NULL ? why : "no reason");
}

// Log a discovery success or failure with the reported address.
void log_discovery_outcome(int success, const char *address, uint16_t port)
{
	if (address != NULL && strcmp(address, "stub") == 0) {
		log_sched("discovery stub armed on port %u", (unsigned int)port);
		return;
	}
	log_sched("discovery %s from %s:%u", success ? "reply" : "failed",
	          address != NULL ? address : "unknown", (unsigned int)port);
}
