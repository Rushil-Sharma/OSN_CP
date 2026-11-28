#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "../common/discovery.h"
#include "../common/log.h"
#include "../common/messages.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "../common/types.h"

// Define default resources used when CLI flags are absent.
#define DEFAULT_WORKER_CORES 1u
#define DEFAULT_WORKER_RAM_MB 512u
#define DEFAULT_WORKER_GPU 0u

// Limit consecutive failed connects so a missing scheduler does not hang the worker.
#define WORKER_CONNECT_ATTEMPTS 3
#define WORKER_RETRY_DELAY_SEC 1u

// Parse a decimal port in the inclusive range 1 through 65535.
static int parse_port(const char *text, int *out)
{
	char *end;
	long value;

	if (text == NULL || out == NULL || *text == '\0') {
		return -1;
	}
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
		return -1;
	}
	*out = (int)value;
	return 0;
}

// Parse an unsigned decimal value with an explicit upper bound.
static int parse_u64(const char *text, uint64_t max, uint64_t *out)
{
	char *end;
	unsigned long long value;

	if (text == NULL || out == NULL || *text == '\0') {
		return -1;
	}
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value > max) {
		return -1;
	}
	*out = (uint64_t)value;
	return 0;
}

// Return total host RAM in MiB using the platform-specific query.
static uint64_t host_ram_mb(void)
{
#ifdef __APPLE__
	uint64_t bytes = 0;
	size_t len = sizeof(bytes);

	if (sysctlbyname("hw.memsize", &bytes, &len, NULL, 0) != 0) {
		return 0;
	}
	return bytes / (1024u * 1024u);
#else
	long pages = sysconf(_SC_PHYS_PAGES);
	long page_size = sysconf(_SC_PAGESIZE);

	if (pages <= 0 || page_size <= 0) {
		return 0;
	}
	return ((uint64_t)pages * (uint64_t)page_size) / (1024u * 1024u);
#endif
}

// Clamp requested cores and RAM to host limits without clamping GPU.
static void clamp_resources(worker_t *worker)
{
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t ram_mb = host_ram_mb();

	if (cores <= 0) {
		log_worker(-1, "warning: host core count unavailable, skipping clamp");
	} else if (worker->total_cores > (uint32_t)cores) {
		log_worker(-1, "warning: cores %u exceeds host limit %ld, clamping",
		           worker->total_cores, cores);
		worker->total_cores = (uint32_t)cores;
	}
	if (ram_mb == 0) {
		log_worker(-1, "warning: host RAM unavailable, skipping clamp");
	} else if (worker->total_ram_mb > ram_mb) {
		log_worker(-1, "warning: RAM %llu MB exceeds host limit %llu MB, clamping",
		           (unsigned long long)worker->total_ram_mb, (unsigned long long)ram_mb);
		worker->total_ram_mb = ram_mb;
	}
	worker->free_cores = worker->total_cores;
	worker->free_ram_mb = worker->total_ram_mb;
	worker->free_gpu = worker->total_gpu;
}

// Parse optional fallback address/port, resource flags, and the discovery port.
static int parse_worker_args(int argc, char **argv, const char **address, int *port,
                             uint32_t *cores, uint64_t *ram_mb, uint32_t *gpu,
                             int *discovery_port)
{
	int positional = 0;
	uint64_t value;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-discovery-port") == 0) {
			if (++i >= argc || parse_port(argv[i], discovery_port) != 0) {
				return -1;
			}
			continue;
		}
		if (strcmp(argv[i], "-cores") == 0 || strcmp(argv[i], "-ram") == 0 ||
		    strcmp(argv[i], "-gpu") == 0) {
			const char *flag = argv[i];

			if (++i >= argc || parse_u64(argv[i], UINT32_MAX, &value) != 0) {
				return -1;
			}
			if (strcmp(flag, "-cores") == 0) {
				if (value == 0) {
					return -1;
				}
				*cores = (uint32_t)value;
			} else if (strcmp(flag, "-ram") == 0) {
				if (value == 0) {
					return -1;
				}
				*ram_mb = value;
			} else {
				*gpu = (uint32_t)value;
			}
			continue;
		}
		if (argv[i][0] == '-') {
			return -1;
		}
		if (positional == 0) {
			*address = argv[i];
			positional++;
		} else if (positional == 1) {
			if (parse_port(argv[i], port) != 0) {
				return -1;
			}
			positional++;
		} else {
			return -1;
		}
	}
	// The fallback address is optional, but an IP without a port is an error.
	return positional == 0 || positional == 2 ? 0 : -1;
}

// Fill worker resource fields from parsed CLI values and host limits.
static void init_worker_resources(worker_t *worker, uint32_t cores, uint64_t ram_mb, uint32_t gpu)
{
	memset(worker, 0, sizeof(*worker));
	worker->id = -1;
	worker->fd = -1;
	worker->state = WORKER_ALIVE;
	worker->total_cores = cores;
	worker->total_ram_mb = ram_mb;
	worker->total_gpu = gpu;
	clamp_resources(worker);
}

// Connect to the scheduler over IPv4 TCP.
static int connect_tcp(const char *address, int port)
{
	struct sockaddr_in server;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, address, &server.sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", address);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&server, sizeof(server)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

// Kill any running jobs before a reconnect, with no jobs in the current skeleton.
static void kill_running_jobs_stub(int32_t worker_id)
{
	log_worker(worker_id, "no running jobs to kill before reconnect");
}

// Register once and return the worker id assigned by the scheduler.
static int register_once(int fd, const worker_t *worker, int32_t *out_id)
{
	uint8_t payload[64];
	uint8_t *received = NULL;
	uint32_t length = 0;
	int32_t worker_id = -1;
	size_t payload_len = 0;
	recv_status_t status;
	msg_type_t type;

	log_worker(-1, "registering with scheduler cores=%u ram_mb=%llu gpu=%u",
	           worker->total_cores, (unsigned long long)worker->total_ram_mb, worker->total_gpu);
	if (msg_encode_worker_register(payload, sizeof(payload), &payload_len, worker) != 0 ||
	    send_msg(fd, MSG_WORKER_REGISTER, payload, (uint32_t)payload_len) != 0) {
		log_worker(-1, "registration send failed");
		return -1;
	}
	status = recv_msg(fd, &type, &received, &length);
	if (status != RECV_OK || type != MSG_WORKER_REGISTER_ACK ||
	    msg_decode_worker_register_ack(received, length, &worker_id) != 0) {
		msg_payload_free(received);
		log_worker(-1, "registration failed");
		return -1;
	}
	log_worker(worker_id, "registration acknowledged");
	printf("WORKER_REGISTER_ACK worker %d\n", worker_id);
	*out_id = worker_id;
	msg_payload_free(received);
	return 0;
}

// Write all staged executable bytes to a temporary file descriptor.
static int write_all_fd(int fd, const uint8_t *data, uint32_t length)
{
	uint32_t written = 0;

	while (written < length) {
		ssize_t count = write(fd, data + written, length - written);

		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return -1;
		}
		written += (uint32_t)count;
	}
	return 0;
}

// Stage a transferred executable in /tmp so the assignment is fully received.
static void stage_binary(int32_t worker_id, const msg_job_assign_t *assignment)
{
	char path[] = "/tmp/osn-job-XXXXXX";
	int fd = mkstemp(path);

	if (fd < 0) {
		log_worker(worker_id, "job %u binary staging failed", assignment->job_id);
		return;
	}
	if (write_all_fd(fd, assignment->binary, assignment->binary_len) != 0 || fchmod(fd, 0700) != 0) {
		close(fd);
		unlink(path);
		log_worker(worker_id, "job %u binary staging failed", assignment->job_id);
		return;
	}
	close(fd);
	log_worker(worker_id, "job %u binary staged at %s (%u bytes)", assignment->job_id, path,
	           assignment->binary_len);
}

// Report a terminal result for an assignment that is not executed yet.
static void send_job_result(int fd, const msg_job_assign_t *assignment, int exit_code,
                            const char *reason)
{
	uint8_t payload[MAX_REASON_LEN + 16u];
	msg_job_result_t result;
	size_t length = 0;

	memset(&result, 0, sizeof(result));
	result.job_id = assignment->job_id;
	result.exit_code = exit_code;
	snprintf(result.reason, sizeof(result.reason), "%s", reason);
	if (msg_encode_job_result(payload, sizeof(payload), &length, &result) == 0) {
		(void)send_msg(fd, MSG_JOB_RESULT, payload, (uint32_t)length);
	}
}

// Receive assignments until the scheduler drops; execution arrives in later milestones.
static void serve_until_disconnect(int fd, int32_t worker_id)
{
	log_worker(worker_id, "Waiting for jobs...");
	for (;;) {
		msg_type_t type;
		uint8_t *body = NULL;
		uint32_t body_len = 0;

		if (recv_msg(fd, &type, &body, &body_len) != RECV_OK) {
			break;
		}
		if (type == MSG_JOB_ASSIGN) {
			msg_job_assign_t assignment;

			if (msg_decode_job_assign(body, body_len, &assignment) != 0) {
				log_worker(worker_id, "bad job assignment payload");
				msg_payload_free(body);
				continue;
			}
			log_worker(worker_id, "job %u assigned (%s, binary=%u bytes)", assignment.job_id,
			           assignment.use_path ? "PATH command" : "transferred executable",
			           assignment.binary_len);
			if (assignment.use_path == 0u) {
				stage_binary(worker_id, &assignment);
			}
			send_job_result(fd, &assignment, -1, "execution not implemented");
		} else {
			log_worker(worker_id, "message %s not implemented", msg_type_name(type));
		}
		msg_payload_free(body);
	}
	log_worker(worker_id, "Lost connection to scheduler.");
}

// Print worker command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [<scheduler_ip> <scheduler_port>] [-cores N] [-ram MB] [-gpu N] "
	        "[-discovery-port N]\n", program);
}

// Discover the scheduler, register, and rediscover + re-register whenever the link drops.
int main(int argc, char **argv)
{
	const char *address = NULL;
	worker_t worker;
	sched_endpoint_t endpoint;
	uint32_t cores = DEFAULT_WORKER_CORES;
	uint64_t ram_mb = DEFAULT_WORKER_RAM_MB;
	uint32_t gpu = DEFAULT_WORKER_GPU;
	int32_t worker_id = -1;
	int discovery_port = DISCOVERY_DEFAULT_PORT;
	int port = 0;
	int failures = 0;
	int fd;

	signal(SIGPIPE, SIG_IGN);
	if (parse_worker_args(argc, argv, &address, &port, &cores, &ram_mb, &gpu,
	                      &discovery_port) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	init_worker_resources(&worker, cores, ram_mb, gpu);
	for (;;) {
		// Discovery runs at startup and again after every dropped or failed connection.
		if (discover_or_fallback(discovery_port, address, port, LOG_WORKER, &endpoint) != 0) {
			fprintf(stderr, "error: no scheduler found via discovery and no fallback "
			        "<scheduler_ip> <scheduler_port> given\n");
			return EXIT_FAILURE;
		}
		log_worker(-1, "Connecting to scheduler at %s:%d...", endpoint.address, endpoint.port);
		fd = connect_tcp(endpoint.address, endpoint.port);
		if (fd >= 0 && register_once(fd, &worker, &worker_id) == 0) {
			failures = 0;
			serve_until_disconnect(fd, worker_id);
		} else {
			failures++;
		}
		if (fd >= 0) {
			close(fd);
		}
		kill_running_jobs_stub(worker_id);
		worker_id = -1;
		if (failures >= WORKER_CONNECT_ATTEMPTS) {
			log_worker(-1, "registration failed after %d attempts", WORKER_CONNECT_ATTEMPTS);
			return EXIT_FAILURE;
		}
		sleep(WORKER_RETRY_DELAY_SEC);
	}
}
