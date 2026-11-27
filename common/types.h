#ifndef OSN_TYPES_H
#define OSN_TYPES_H

#include <stdbool.h>
#include <stdint.h>

// Define central capacities for jobs, workers, and fixed-size buffers.
#define QUEUE_CAPACITY 1024u
#define MAX_ATTEMPTS 3u
#define MAX_WORKERS 256u
#define MAX_ARGV 64u
#define MAX_ARG_LEN 256u
#define MAX_USERNAME 32u
#define MAX_DEPS 64u
#define MAX_CACHED_HASHES 256u
#define HASH_SIZE 32u
#define JOB_OUTPUT_CAP (64u * 1024u)
#define MAX_REASON_LEN 64u
#define MAX_ADDR_LEN 64u
#define MAX_PATH_LEN 4096u

// Define all job states used across the full lifecycle.
typedef enum {
	JOB_QUEUED,
	JOB_RUNNING,
	JOB_COMPLETED,
	JOB_FAILED,
	JOB_CANCELLED,
	JOB_WAITING,
	JOB_LOST,
	JOB_FAILED_PERMANENT
} job_state_t;

// Define all worker states used across the full lifecycle.
typedef enum {
	WORKER_ALIVE,
	WORKER_SUSPECTED,
	WORKER_DEAD,
	WORKER_DRAINING
} worker_state_t;

// Store per-job runtime figures reported by workers.
typedef struct {
	uint64_t rss_bytes;
	uint64_t peak_rss_bytes;
	uint64_t page_faults;
	uint64_t cpu_time_ms;
	int32_t exit_code;
	int32_t term_signal;
	char reason[MAX_REASON_LEN];
} job_telemetry_t;

// Store the full lifecycle state of one submitted job.
typedef struct {
	uint32_t id;
	char owner[MAX_USERNAME];
	uint32_t argc;
	char argv[MAX_ARGV][MAX_ARG_LEN];
	uint32_t cores;
	uint64_t ram_mb;
	uint32_t time_sec;
	uint32_t prio;
	uint32_t gpu;
	bool preemptible;
	job_state_t state;
	uint8_t attempts;
	uint32_t deps[MAX_DEPS];
	uint32_t dep_count;
	uint32_t group_id;
	bool has_group;
	uint32_t cron_id;
	uint8_t hash[HASH_SIZE];
	uint8_t hash_len;
	uint32_t checkpoint_seq;
	uint64_t checkpoint_size;
	int64_t checkpoint_mtime;
	int32_t migrated_from;
	bool preempt_hold;
	int64_t submit_ts;
	int64_t start_ts;
	int32_t assigned_worker;
	uint8_t output[JOB_OUTPUT_CAP];
	uint32_t output_len;
	uint64_t output_total;
	job_telemetry_t telemetry;
} job_t;

// Store the full lifecycle state of one connected worker.
typedef struct {
	int32_t id;
	char address[MAX_ADDR_LEN];
	uint16_t port;
	uint32_t total_cores;
	uint32_t free_cores;
	uint64_t total_ram_mb;
	uint64_t free_ram_mb;
	uint32_t total_gpu;
	uint32_t free_gpu;
	worker_state_t state;
	uint32_t missed_heartbeats;
	uint32_t cpu_load_pct;
	uint32_t running_jobs;
	uint8_t cached_hashes[MAX_CACHED_HASHES][HASH_SIZE];
	uint32_t cached_hash_count;
	int fd;
	int64_t last_heartbeat_ts;
} worker_t;

#endif
