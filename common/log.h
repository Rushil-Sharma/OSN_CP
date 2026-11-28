#ifndef OSN_LOG_H
#define OSN_LOG_H

#include <stdint.h>

#include "types.h"

// Identify which process tag a log line should carry.
typedef enum {
	LOG_SCHEDULER,
	LOG_WORKER,
	LOG_CLIENT
} log_source_t;

// Print one timestamped, tagged, thread-safe log line.
void log_line(log_source_t source, int32_t worker_id, const char *fmt, ...);

// Log a worker state transition.
void log_worker_state(int32_t worker_id, worker_state_t old_state, worker_state_t new_state);

// Log a job state transition.
void log_job_state(uint32_t job_id, job_state_t old_state, job_state_t new_state);

// Log a scheduling decision with the reason the worker was chosen.
void log_scheduling_decision(uint32_t job_id, int32_t worker_id, const char *why);

// Log a scheduler discovery responder outcome for one requester.
void log_discovery_outcome(int success, const char *address, uint16_t port);

#define log_sched(...) log_line(LOG_SCHEDULER, -1, __VA_ARGS__)
#define log_worker(worker_id, ...) log_line(LOG_WORKER, (worker_id), __VA_ARGS__)

#endif
