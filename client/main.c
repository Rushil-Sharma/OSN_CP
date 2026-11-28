#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/discovery.h"
#include "../common/log.h"
#include "../common/messages.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "../common/types.h"

// Define reconnect policy and the stdin line buffer size.
#define CLIENT_RECONNECT_ROUNDS 3
#define CLIENT_RECONNECT_DELAY_SEC 1u
#define CLIENT_LINE_CAP 4096u
#define CLIENT_MAX_TOKENS (MAX_ARGV + 16u)

// Hold the connection settings, login name, socket, and buffered stdin bytes.
typedef struct {
	const char *fallback_address;
	int fallback_port;
	int discovery_port;
	char user[MAX_USERNAME];
	int fd;
	char line[CLIENT_LINE_CAP];
	size_t line_len;
	bool stdin_eof;
} client_t;

// Outcome of waiting for a reply to one request.
typedef enum {
	REPLY_OK,
	REPLY_ERROR,
	REPLY_LOST
} reply_status_t;

// Outcome of running one command line.
typedef enum {
	CMD_CONTINUE,
	CMD_QUIT,
	CMD_FATAL
} cmd_status_t;

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

// Parse an unsigned decimal value with an inclusive range.
static int parse_u64_range(const char *text, uint64_t min, uint64_t max, uint64_t *out)
{
	char *end;
	unsigned long long value;

	if (text == NULL || out == NULL || *text == '\0' || *text == '-' || *text == '+') {
		return -1;
	}
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
		return -1;
	}
	*out = (uint64_t)value;
	return 0;
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

// Print an [Event] line; at an idle prompt, clear the prompt line first on a terminal.
static void print_event(const uint8_t *payload, uint32_t length, bool at_prompt)
{
	char text[MSG_TEXT_CAP + 1u];

	if (msg_decode_event(payload, length, text, sizeof(text)) != 0) {
		return;
	}
	if (at_prompt) {
		fputs(isatty(STDOUT_FILENO) ? "\r\033[K" : "\n", stdout);
	}
	printf("[Event] %s\n", text);
	fflush(stdout);
}

// Print the text carried by an MSG_ERROR payload.
static void print_error(const uint8_t *payload, uint32_t length, uint16_t *code_out)
{
	char text[MSG_ERROR_TEXT_CAP + 1u];
	uint16_t code = 0;

	if (msg_decode_error(payload, length, &code, text, sizeof(text)) != 0) {
		snprintf(text, sizeof(text), "malformed error reply");
	}
	if (code_out != NULL) {
		*code_out = code;
	}
	if (code == MSG_ERR_QUEUE_FULL) {
		printf("[Client] Queue is full (QUEUE_FULL). Job not submitted, try again later.\n");
	} else {
		printf("[Client] Error: %s\n", text);
	}
}

// Wait for the reply to a request, printing any [Event]s that arrive first.
static reply_status_t await_reply(client_t *c, msg_type_t expected, uint8_t **payload, uint32_t *length,
                                  uint16_t *error_code)
{
	for (;;) {
		msg_type_t type;
		uint8_t *body = NULL;
		uint32_t body_len = 0;

		if (recv_msg(c->fd, &type, &body, &body_len) != RECV_OK) {
			return REPLY_LOST;
		}
		if (type == MSG_EVENT) {
			print_event(body, body_len, false);
			msg_payload_free(body);
			continue;
		}
		if (type == MSG_ERROR) {
			print_error(body, body_len, error_code);
			msg_payload_free(body);
			return REPLY_ERROR;
		}
		if (type == expected) {
			*payload = body;
			*length = body_len;
			return REPLY_OK;
		}
		// Anything else is stale; drop it and keep waiting.
		msg_payload_free(body);
	}
}

// Send the username as the first frame and wait for the hello acknowledgement.
static int send_login(client_t *c)
{
	uint8_t payload[64];
	uint8_t *reply = NULL;
	uint32_t reply_len = 0;
	uint32_t session_id = 0;
	size_t payload_len = 0;
	uint16_t code = 0;
	reply_status_t status;

	if (msg_encode_client_hello(payload, sizeof(payload), &payload_len, c->user) != 0 ||
	    send_msg(c->fd, MSG_CLIENT_HELLO, payload, (uint32_t)payload_len) != 0) {
		return -1;
	}
	status = await_reply(c, MSG_HELLO_ACK, &reply, &reply_len, &code);
	if (status == REPLY_ERROR) {
		// The scheduler rejected the login itself, so retrying cannot help.
		return -2;
	}
	if (status != REPLY_OK || msg_decode_hello_ack(reply, reply_len, &session_id) != 0) {
		msg_payload_free(reply);
		return -1;
	}
	msg_payload_free(reply);
	return 0;
}

// Discover the scheduler (CLI address as fallback) and open a TCP connection.
static int open_connection(client_t *c)
{
	sched_endpoint_t endpoint;

	if (discover_or_fallback(c->discovery_port, c->fallback_address, c->fallback_port, LOG_CLIENT,
	                         &endpoint) != 0) {
		return -1;
	}
	c->fd = connect_tcp(endpoint.address, endpoint.port);
	if (c->fd < 0) {
		return -1;
	}
	log_line(LOG_CLIENT, -1, "Connected to Scheduler.");
	return 0;
}

// Rediscover, reconnect, and resend the username without prompting the user.
static int reconnect(client_t *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	log_line(LOG_CLIENT, -1, "Lost connection to scheduler. Reconnecting...");
	for (int round = 1; round <= CLIENT_RECONNECT_ROUNDS; round++) {
		if (open_connection(c) == 0) {
			int rc = send_login(c);

			if (rc == 0) {
				log_line(LOG_CLIENT, -1, "Reconnected. Logged in again as %s.", c->user);
				return 0;
			}
			close(c->fd);
			c->fd = -1;
			if (rc == -2) {
				return -1;
			}
		}
		if (round < CLIENT_RECONNECT_ROUNDS) {
			sleep(CLIENT_RECONNECT_DELAY_SEC);
		}
	}
	log_line(LOG_CLIENT, -1, "Could not reconnect after %d rounds; exiting.", CLIENT_RECONNECT_ROUNDS);
	return -1;
}

// Move one complete line out of the stdin buffer; 1 = got line, 0 = need more, -1 = EOF.
static int take_line(client_t *c, char *out, size_t cap)
{
	char *newline = memchr(c->line, '\n', c->line_len);
	size_t take;
	size_t consumed;

	if (newline != NULL) {
		take = (size_t)(newline - c->line);
		consumed = take + 1;
	} else if (c->line_len >= CLIENT_LINE_CAP - 1u || (c->stdin_eof && c->line_len > 0)) {
		// Hand over an overlong line or a final line with no newline as-is.
		take = c->line_len;
		consumed = c->line_len;
	} else {
		return c->stdin_eof ? -1 : 0;
	}
	if (take > 0 && c->line[take - 1] == '\r') {
		take--;
	}
	if (take >= cap) {
		take = cap - 1;
	}
	memcpy(out, c->line, take);
	out[take] = '\0';
	memmove(c->line, c->line + consumed, c->line_len - consumed);
	c->line_len -= consumed;
	return 1;
}

// Read whatever stdin has into the line buffer.
static void fill_stdin(client_t *c)
{
	ssize_t n = read(STDIN_FILENO, c->line + c->line_len, CLIENT_LINE_CAP - 1u - c->line_len);

	if (n > 0) {
		c->line_len += (size_t)n;
	} else if (n == 0 || errno != EINTR) {
		c->stdin_eof = true;
	}
}

// Block until one full stdin line is available; returns -1 on EOF.
static int read_line_blocking(client_t *c, char *out, size_t cap)
{
	for (;;) {
		int rc = take_line(c, out, cap);

		if (rc != 0) {
			return rc;
		}
		fill_stdin(c);
	}
}

// Ask for a username until a valid one is entered; returns -1 on EOF.
static int prompt_username(client_t *c)
{
	char line[CLIENT_LINE_CAP];

	for (;;) {
		printf("Username: ");
		fflush(stdout);
		if (read_line_blocking(c, line, sizeof(line)) != 1) {
			printf("\n");
			return -1;
		}
		// Validation guarantees the name fits in c->user (at most 31 chars + NUL).
		if (msg_username_valid(line)) {
			memcpy(c->user, line, strlen(line) + 1u);
			return 0;
		}
		printf("Invalid username: use 1-31 characters from letters, digits, _, - and .\n");
	}
}

// Send one request and wait for its reply; reconnects if the link drops mid-request.
static reply_status_t request(client_t *c, msg_type_t type, const void *payload, uint32_t length,
                              msg_type_t expected, uint8_t **reply, uint32_t *reply_len, bool *fatal)
{
	reply_status_t status = REPLY_LOST;

	if (send_msg(c->fd, type, payload, length) == 0) {
		status = await_reply(c, expected, reply, reply_len, NULL);
	}
	if (status == REPLY_LOST) {
		if (reconnect(c) != 0) {
			*fatal = true;
		} else {
			printf("[Client] The command may not have completed; please retry it.\n");
		}
	}
	return status;
}

// Parse a positive job id argument.
static int parse_job_id(const char *text, uint32_t *job_id)
{
	uint64_t value;

	if (parse_u64_range(text, 1, UINT32_MAX, &value) != 0) {
		printf("[Client] Invalid job id '%s'.\n", text != NULL ? text : "");
		return -1;
	}
	*job_id = (uint32_t)value;
	return 0;
}

// Print the cluster table from a nodes reply.
static void print_nodes(const uint8_t *payload, uint32_t length)
{
	msg_node_t *nodes = malloc(sizeof(*nodes) * MAX_WORKERS);
	uint32_t count = 0;

	if (nodes == NULL || msg_decode_nodes(payload, length, nodes, MAX_WORKERS, &count) != 0) {
		printf("[Client] Error: malformed nodes reply\n");
		free(nodes);
		return;
	}
	printf("--- Cluster Status ---\n");
	if (count == 0) {
		printf("(no workers registered)\n");
	} else {
		printf("%-6s %-10s %-19s %-19s %-17s %s\n", "ID", "Status", "Cores (Free/Total)",
		       "RAM (Free/Total)", "GPU (Free/Total)", "Jobs");
	}
	for (uint32_t i = 0; i < count; i++) {
		const msg_node_t *n = &nodes[i];
		char cores[32];
		char ram[48];
		char gpu[32];

		snprintf(cores, sizeof(cores), "%u / %u", n->free_cores, n->total_cores);
		snprintf(ram, sizeof(ram), "%llu / %llu", (unsigned long long)n->free_ram_mb,
		         (unsigned long long)n->total_ram_mb);
		snprintf(gpu, sizeof(gpu), "%u / %u", n->free_gpu, n->total_gpu);
		printf("%-6d %-10s %-19s %-19s %-17s %u\n", (int)n->id,
		       worker_state_name((worker_state_t)n->state), cores, ram, gpu, n->running_jobs);
	}
	free(nodes);
}

// Print one job's status reply.
static void print_status(const uint8_t *payload, uint32_t length)
{
	msg_status_t s;

	if (msg_decode_status(payload, length, &s) != 0) {
		printf("[Client] Error: malformed status reply\n");
		return;
	}
	printf("[Job %u] Status: %s\n", s.job_id, job_state_name((job_state_t)s.state));
	printf("[Job %u] User: %s\n", s.job_id, s.owner);
	printf("[Job %u] Command: %s\n", s.job_id, s.command);
	printf("[Job %u] Resources: %u cores, %llu MB RAM, %u GPU, %u s limit, priority %u\n", s.job_id,
	       s.cores, (unsigned long long)s.ram_mb, s.gpu, s.time_sec, s.prio);
	if (s.worker_id >= 0) {
		printf("[Job %u] Assigned to: Worker %d\n", s.job_id, (int)s.worker_id);
	} else {
		printf("[Job %u] Assigned to: None\n", s.job_id);
	}
	if (s.attempts > 0) {
		printf("[Job %u] Attempts: %u\n", s.job_id, (unsigned int)s.attempts);
	}
	if (s.reason[0] != '\0') {
		printf("[Job %u] Reason: %s\n", s.job_id, s.reason);
	}
}

// Print the output carried by a logs chunk.
static void print_logs(const uint8_t *payload, uint32_t length)
{
	const uint8_t *data = NULL;
	uint32_t data_len = 0;
	uint32_t job_id = 0;
	uint8_t final = 0;

	if (msg_decode_logs_chunk(payload, length, &job_id, &final, &data, &data_len) != 0) {
		printf("[Client] Error: malformed logs reply\n");
		return;
	}
	if (data_len == 0) {
		printf("[Client] Job %u has no output yet.\n", job_id);
		return;
	}
	fwrite(data, 1, data_len, stdout);
	if (data[data_len - 1] != '\n') {
		fputc('\n', stdout);
	}
}

// Return 1 if a token is a submit flag from a later milestone that is not supported yet.
static int is_later_submit_flag(const char *token)
{
	return strcmp(token, "-depends-on") == 0 || strcmp(token, "-group") == 0 ||
	       strcmp(token, "-preemptible") == 0 || strcmp(token, "-every") == 0;
}

// Parse "submit <path|command> [args] -cores N -ram MB -time S -prio 1-3 [-gpu N]".
static int parse_submit(char **tokens, int count, msg_submit_t *submit)
{
	bool have_cores = false;
	bool have_ram = false;
	bool have_time = false;
	bool have_prio = false;
	bool in_flags = false;
	int i;

	memset(submit, 0, sizeof(*submit));
	for (i = 1; i < count; i++) {
		const char *t = tokens[i];
		uint64_t value;

		if (is_later_submit_flag(t)) {
			printf("[Client] Flag %s is not supported yet.\n", t);
			return -1;
		}
		if (strcmp(t, "-cores") == 0 || strcmp(t, "-ram") == 0 || strcmp(t, "-time") == 0 ||
		    strcmp(t, "-prio") == 0 || strcmp(t, "-gpu") == 0) {
			bool is_prio = strcmp(t, "-prio") == 0;
			bool is_gpu = strcmp(t, "-gpu") == 0;
			uint64_t min = is_gpu ? 0 : 1;
			uint64_t max = is_prio ? 3 : (strcmp(t, "-ram") == 0 ? UINT64_MAX / 2 : UINT32_MAX);

			in_flags = true;
			if (i + 1 >= count || parse_u64_range(tokens[i + 1], min, max, &value) != 0) {
				printf("[Client] %s needs %s.\n", t,
				       is_prio ? "a value from 1 to 3" :
				       is_gpu ? "a whole number >= 0" : "a whole number >= 1");
				return -1;
			}
			i++;
			if (strcmp(t, "-cores") == 0) {
				submit->cores = (uint32_t)value;
				have_cores = true;
			} else if (strcmp(t, "-ram") == 0) {
				submit->ram_mb = value;
				have_ram = true;
			} else if (strcmp(t, "-time") == 0) {
				submit->time_sec = (uint32_t)value;
				have_time = true;
			} else if (is_prio) {
				submit->prio = (uint32_t)value;
				have_prio = true;
			} else {
				submit->gpu = (uint32_t)value;
			}
			continue;
		}
		// Tokens before the first flag are the command and its arguments.
		if (in_flags) {
			printf("[Client] Unexpected argument '%s' after the flags.\n", t);
			return -1;
		}
		if (submit->argc >= MAX_ARGV || strlen(t) >= MAX_ARG_LEN) {
			printf("[Client] Command too long (max %u arguments of %u characters).\n",
			       MAX_ARGV, MAX_ARG_LEN - 1u);
			return -1;
		}
		snprintf(submit->argv[submit->argc++], MAX_ARG_LEN, "%s", t);
	}
	if (submit->argc == 0 || !have_cores || !have_ram || !have_time || !have_prio) {
		printf("[Client] Usage: submit <path|command> -cores N -ram MB -time S -prio 1-3 [-gpu N]\n");
		printf("[Client] -cores, -ram, -time and -prio are required.\n");
		return -1;
	}
	return 0;
}

// Print the commands the client understands.
static void print_help(void)
{
	printf("Commands:\n");
	printf("  nodes\n");
	printf("  submit <path|command> -cores N -ram MB -time S -prio 1-3 [-gpu N]\n");
	printf("  status <job_id>\n");
	printf("  logs <job_id> [-f]\n");
	printf("  cancel <job_id>\n");
	printf("  help, exit\n");
}

// Split a line on whitespace in place; returns the token count.
static int tokenize(char *line, char **tokens, int cap)
{
	int count = 0;
	char *save = NULL;

	for (char *t = strtok_r(line, " \t", &save); t != NULL && count < cap;
	     t = strtok_r(NULL, " \t", &save)) {
		tokens[count++] = t;
	}
	return count;
}

// Run one command line typed at the client> prompt.
static cmd_status_t run_command(client_t *c, char *line)
{
	char *tokens[CLIENT_MAX_TOKENS];
	uint8_t payload[16];
	uint8_t *reply = NULL;
	uint32_t reply_len = 0;
	size_t payload_len = 0;
	bool fatal = false;
	uint32_t job_id;
	int count = tokenize(line, tokens, (int)CLIENT_MAX_TOKENS);
	const char *cmd;

	if (count == 0) {
		return CMD_CONTINUE;
	}
	cmd = tokens[0];
	if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
		return CMD_QUIT;
	}
	if (strcmp(cmd, "help") == 0) {
		print_help();
	} else if (strcmp(cmd, "nodes") == 0) {
		if (request(c, MSG_NODES_REQ, NULL, 0, MSG_NODES_REPLY, &reply, &reply_len, &fatal) == REPLY_OK) {
			print_nodes(reply, reply_len);
		}
	} else if (strcmp(cmd, "submit") == 0) {
		msg_submit_t *submit = malloc(sizeof(*submit));
		uint8_t *buf = malloc(sizeof(*submit) + 64u);
		size_t buf_len = 0;

		if (submit != NULL && buf != NULL && parse_submit(tokens, count, submit) == 0 &&
		    msg_encode_submit(buf, sizeof(*submit) + 64u, &buf_len, submit) == 0 &&
		    request(c, MSG_SUBMIT, buf, (uint32_t)buf_len, MSG_SUBMIT_ACK, &reply, &reply_len,
		            &fatal) == REPLY_OK) {
			if (msg_decode_job_id(reply, reply_len, &job_id) == 0) {
				printf("[Client] Job submitted.\n[Client] Job ID assigned: %u\n", job_id);
			}
		}
		free(submit);
		free(buf);
	} else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "cancel") == 0) {
		bool is_status = strcmp(cmd, "status") == 0;

		if (count != 2) {
			printf("[Client] Usage: %s <job_id>\n", cmd);
		} else if (parse_job_id(tokens[1], &job_id) == 0 &&
		           msg_encode_job_id(payload, sizeof(payload), &payload_len, job_id) == 0 &&
		           request(c, is_status ? MSG_STATUS_REQ : MSG_CANCEL, payload, (uint32_t)payload_len,
		                   is_status ? MSG_STATUS_REPLY : MSG_CANCEL_ACK, &reply, &reply_len,
		                   &fatal) == REPLY_OK) {
			uint8_t old_state;

			if (is_status) {
				print_status(reply, reply_len);
			} else if (msg_decode_cancel_ack(reply, reply_len, &job_id, &old_state) == 0) {
				printf("[Client] Job %u cancelled (was %s).\n", job_id,
				       job_state_name((job_state_t)old_state));
			}
		}
	} else if (strcmp(cmd, "logs") == 0) {
		bool follow = count == 3 && strcmp(tokens[2], "-f") == 0;

		if (count != 2 && !follow) {
			printf("[Client] Usage: logs <job_id> [-f]\n");
		} else if (parse_job_id(tokens[1], &job_id) == 0 &&
		           msg_encode_logs_req(payload, sizeof(payload), &payload_len, job_id,
		                               follow ? 1 : 0) == 0 &&
		           request(c, MSG_LOGS_REQ, payload, (uint32_t)payload_len, MSG_LOGS_CHUNK, &reply,
		                   &reply_len, &fatal) == REPLY_OK) {
			print_logs(reply, reply_len);
		}
	} else if (strcmp(cmd, "migrate") == 0 || strcmp(cmd, "crons") == 0 ||
	           strcmp(cmd, "cancel-cron") == 0) {
		printf("[Client] '%s' is not available yet.\n", cmd);
	} else {
		printf("[Client] Unknown command '%s'. Type 'help' for the list.\n", cmd);
	}
	msg_payload_free(reply);
	fflush(stdout);
	return fatal ? CMD_FATAL : CMD_CONTINUE;
}

// Serve the client> prompt, polling stdin and the socket so [Event]s show while idle.
static int command_loop(client_t *c)
{
	char line[CLIENT_LINE_CAP];
	bool show_prompt = true;

	for (;;) {
		struct pollfd fds[2];
		int rc;

		if (show_prompt) {
			printf("client> ");
			fflush(stdout);
			show_prompt = false;
		}
		// Run lines already buffered (e.g. several pasted at once) before polling again.
		rc = take_line(c, line, sizeof(line));
		if (rc == 1) {
			cmd_status_t status = run_command(c, line);

			if (status == CMD_QUIT) {
				return 0;
			}
			if (status == CMD_FATAL) {
				return -1;
			}
			show_prompt = true;
			continue;
		}
		if (rc == -1) {
			printf("\n");
			return 0;
		}
		fds[0].fd = STDIN_FILENO;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = c->fd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("poll");
			return -1;
		}
		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			msg_type_t type;
			uint8_t *body = NULL;
			uint32_t body_len = 0;

			if (recv_msg(c->fd, &type, &body, &body_len) != RECV_OK) {
				printf("\n");
				if (reconnect(c) != 0) {
					return -1;
				}
				show_prompt = true;
				continue;
			}
			if (type == MSG_EVENT) {
				print_event(body, body_len, true);
				show_prompt = true;
			}
			msg_payload_free(body);
		}
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			fill_stdin(c);
		}
	}
}

// Parse optional fallback address/port, -user, and -discovery-port in any order.
static int parse_client_args(int argc, char **argv, const char **address, int *port,
                             const char **user, int *discovery_port)
{
	int positional = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-user") == 0) {
			if (++i >= argc) {
				return -1;
			}
			*user = argv[i];
		} else if (strcmp(argv[i], "-discovery-port") == 0) {
			if (++i >= argc || parse_port(argv[i], discovery_port) != 0) {
				return -1;
			}
		} else if (argv[i][0] == '-') {
			return -1;
		} else if (positional == 0) {
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

// Print client command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [<scheduler_ip> <scheduler_port>] [-user <name>] "
	        "[-discovery-port N]\n", program);
}

// Connect, log in (prompting unless -user was given), then run the command prompt.
int main(int argc, char **argv)
{
	static client_t client;
	const char *user = NULL;
	int rc;

	client.fd = -1;
	client.discovery_port = DISCOVERY_DEFAULT_PORT;
	if (parse_client_args(argc, argv, &client.fallback_address, &client.fallback_port, &user,
	                      &client.discovery_port) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (user != NULL && !msg_username_valid(user)) {
		fprintf(stderr, "error: invalid -user '%s': use 1-31 characters from letters, digits, "
		        "_, - and .\n", user);
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);
	if (open_connection(&client) != 0) {
		fprintf(stderr, "error: could not reach the scheduler (no discovery reply and no usable "
		        "fallback <scheduler_ip> <scheduler_port>)\n");
		return EXIT_FAILURE;
	}
	// -user skips the prompt; otherwise ask until the name is valid.
	if (user != NULL) {
		snprintf(client.user, sizeof(client.user), "%s", user);
	} else if (prompt_username(&client) != 0) {
		close(client.fd);
		return EXIT_SUCCESS;
	}
	rc = send_login(&client);
	if (rc != 0 && (rc == -2 || reconnect(&client) != 0)) {
		if (client.fd >= 0) {
			close(client.fd);
		}
		return EXIT_FAILURE;
	}
	printf("Logged in as %s.\n", client.user);
	rc = command_loop(&client);
	if (client.fd >= 0) {
		close(client.fd);
	}
	return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
