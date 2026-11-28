#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../common/log.h"
#include "../common/messages.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "../common/types.h"
#include "commands.h"
#include "discovery.h"
#include "schedule.h"

// Define the TCP listen backlog for the scheduler socket.
#define LISTEN_BACKLOG 64

// Hold the accepted socket and shared state for one session thread.
typedef struct {
	int fd;
	scheduler_state_t *state;
} session_ctx_t;

// Hold the listening socket and shared state for the accept thread.
typedef struct {
	int listen_fd;
	scheduler_state_t *state;
} accept_ctx_t;

// Mark the scheduler stopped and wake the scheduling thread.
static void stop_scheduler(scheduler_state_t *state)
{
	if (pthread_mutex_lock(&state->lock) != 0) {
		return;
	}
	state->stop = true;
	scheduler_signal_work_locked(state);
	pthread_mutex_unlock(&state->lock);
}

// Send an encoded error frame before closing a session.
static void send_error(int fd, uint16_t code, const char *text)
{
	uint8_t payload[MSG_ERROR_TEXT_CAP + 8u];
	size_t length = 0;

	if (msg_encode_error(payload, sizeof(payload), &length, code, text) == 0) {
		(void)send_msg(fd, MSG_ERROR, payload, (uint32_t)length);
	}
}

// Handle a worker session after its first register frame.
static void worker_session(int fd, scheduler_state_t *state, const uint8_t *payload, uint32_t length)
{
	worker_t snapshot;
	int32_t worker_id;
	uint8_t ack[16];
	size_t ack_len = 0;

	if (msg_decode_worker_register(payload, length, &snapshot) != 0) {
		send_error(fd, MSG_ERR_BAD_REGISTER, "bad register payload");
		return;
	}
	if (scheduler_register_worker(state, &snapshot, fd, &worker_id) != 0) {
		send_error(fd, MSG_ERR_WORKER_CAPACITY, "worker capacity reached");
		return;
	}
	log_sched("worker %d registered cores=%u ram_mb=%llu gpu=%u", worker_id,
	          snapshot.total_cores, (unsigned long long)snapshot.total_ram_mb, snapshot.total_gpu);
	if (msg_encode_worker_register_ack(ack, sizeof(ack), &ack_len, worker_id) != 0 ||
	    send_msg(fd, MSG_WORKER_REGISTER_ACK, ack, (uint32_t)ack_len) != 0) {
		log_sched("worker %d registration ack failed", worker_id);
		scheduler_mark_worker_fd_closed(state, fd);
		return;
	}
	for (;;) {
		msg_type_t type;
		uint8_t *body = NULL;
		uint32_t body_len = 0;
		recv_status_t status = recv_msg(fd, &type, &body, &body_len);

		if (status != RECV_OK) {
			break;
		}
		if (type == MSG_HEARTBEAT) {
			uint32_t cpu_load_pct = 0;
			uint32_t running_jobs = 0;

			if (body_len > 0 &&
			    msg_decode_heartbeat(body, body_len, &cpu_load_pct, &running_jobs) != 0) {
				log_worker(worker_id, "bad heartbeat payload");
			}
			if (pthread_mutex_lock(&state->lock) == 0) {
				(void)scheduler_note_heartbeat_locked(state, fd, cpu_load_pct, running_jobs);
				pthread_mutex_unlock(&state->lock);
			}
		} else if (type == MSG_JOB_RESULT) {
			msg_job_result_t result;
			int32_t reporting_worker = -1;

			if (msg_decode_job_result(body, body_len, &result) == 0 &&
			    pthread_mutex_lock(&state->lock) == 0) {
				for (uint32_t i = 0; i < state->worker_count; i++) {
					if (state->workers[i].fd == fd) {
						reporting_worker = state->workers[i].id;
						break;
					}
				}
				if (reporting_worker >= 0) {
					(void)scheduler_finish_job_locked(state, result.job_id, reporting_worker,
					                                  &result);
				}
				pthread_mutex_unlock(&state->lock);
			} else {
				log_worker(worker_id, "bad job result payload");
			}
		} else {
			log_worker(worker_id, "message %s not implemented", msg_type_name(type));
		}
		msg_payload_free(body);
	}
	log_sched("worker %d session closed", worker_id);
	scheduler_mark_worker_fd_closed(state, fd);
}

// Read the first frame and dispatch a detached session to client or worker handling.
static void *session_thread(void *arg)
{
	session_ctx_t *ctx = arg;
	msg_type_t type;
	uint8_t *payload = NULL;
	uint32_t length = 0;
	recv_status_t status;
	struct timeval send_timeout;

	// Bound all session writes so one slow peer cannot stall scheduler work.
	send_timeout.tv_sec = 2;
	send_timeout.tv_usec = 0;
	(void)setsockopt(ctx->fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
	status = recv_msg(ctx->fd, &type, &payload, &length);
	if (status == RECV_OK) {
		if (type == MSG_CLIENT_HELLO) {
			client_session_run(ctx->fd, ctx->state, payload, length);
		} else if (type == MSG_WORKER_REGISTER) {
			worker_session(ctx->fd, ctx->state, payload, length);
		} else {
			send_error(ctx->fd, MSG_ERR_UNEXPECTED_FIRST, "expected hello or register");
		}
	}
	msg_payload_free(payload);
	close(ctx->fd);
	free(ctx);
	return NULL;
}

// Accept TCP connections and start one detached session thread for each.
static void *accept_loop(void *arg)
{
	accept_ctx_t *ctx = arg;

	for (;;) {
		int fd = accept(ctx->listen_fd, NULL, NULL);
		session_ctx_t *session;
		pthread_t thread;

		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("accept");
			stop_scheduler(ctx->state);
			break;
		}
		session = malloc(sizeof(*session));
		if (session == NULL) {
			close(fd);
			continue;
		}
		session->fd = fd;
		session->state = ctx->state;
		if (pthread_create(&thread, NULL, session_thread, session) != 0) {
			close(fd);
			free(session);
			continue;
		}
		pthread_detach(thread);
	}
	free(ctx);
	return NULL;
}

// Run one bounded scheduling tick at least once per second.
static void *scheduling_loop(void *arg)
{
	scheduler_state_t *state = arg;

	for (;;) {
		bool stop;

		if (pthread_mutex_lock(&state->lock) != 0) {
			break;
		}
		while (!state->stop) {
			struct timespec deadline;
			int rc;

			if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
				state->stop = true;
				break;
			}
			deadline.tv_sec += 1;
			rc = pthread_cond_timedwait(&state->work_cond, &state->lock, &deadline);
			if (rc == ETIMEDOUT || rc == EINTR) {
				break;
			}
			if (rc != 0) {
				state->stop = true;
				break;
			}
		}
		stop = state->stop;
		pthread_mutex_unlock(&state->lock);
		if (stop) {
			break;
		}
		scheduler_run_pass(state);
	}
	return NULL;
}

// Open and configure the TCP listening socket.
static int open_listener(int port)
{
	struct sockaddr_in address;
	int fd;
	int opt = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
		perror("setsockopt");
		close(fd);
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	if (listen(fd, LISTEN_BACKLOG) != 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

// Start the scheduler listener, accept thread, and dedicated scheduling thread.
int scheduler_run(int port, policy_t policy, int discovery_port)
{
	scheduler_state_t *state = scheduler_global_state();
	accept_ctx_t *accept_ctx;
	pthread_t accept_thread;
	pthread_t scheduling_thread;
	int listen_fd;

	signal(SIGPIPE, SIG_IGN);
	if (scheduler_state_init(state, policy) != 0) {
		perror("scheduler_state_init");
		return -1;
	}
	listen_fd = open_listener(port);
	if (listen_fd < 0) {
		scheduler_state_destroy(state);
		return -1;
	}
	log_sched("startup port=%d policy=%s discovery_port=%d", port,
	          policy == POLICY_FIFO ? "FIFO" : "PRIORITY", discovery_port);
	if (discovery_responder_start(discovery_port, port) != 0) {
		close(listen_fd);
		scheduler_state_destroy(state);
		return -1;
	}
	if (pthread_create(&scheduling_thread, NULL, scheduling_loop, state) != 0) {
		close(listen_fd);
		scheduler_state_destroy(state);
		return -1;
	}
	accept_ctx = malloc(sizeof(*accept_ctx));
	if (accept_ctx == NULL) {
		stop_scheduler(state);
		pthread_join(scheduling_thread, NULL);
		close(listen_fd);
		scheduler_state_destroy(state);
		return -1;
	}
	accept_ctx->listen_fd = listen_fd;
	accept_ctx->state = state;
	if (pthread_create(&accept_thread, NULL, accept_loop, accept_ctx) != 0) {
		free(accept_ctx);
		stop_scheduler(state);
		pthread_join(scheduling_thread, NULL);
		close(listen_fd);
		scheduler_state_destroy(state);
		return -1;
	}
	pthread_join(accept_thread, NULL);
	stop_scheduler(state);
	pthread_join(scheduling_thread, NULL);
	close(listen_fd);
	scheduler_state_destroy(state);
	return 0;
}
