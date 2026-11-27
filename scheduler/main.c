#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

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

// Print scheduler command usage to stderr.
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s <port> <FIFO|PRIORITY> [-discovery-port N]\n", program);
}

// Parse command-line arguments and start the threaded scheduler.
int main(int argc, char **argv)
{
	policy_t policy = POLICY_FIFO;
	int port = 0;
	int discovery_port = 9999;

	if (argc < 3 || argc > 5) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (parse_port(argv[1], &port) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (strcmp(argv[2], "FIFO") == 0) {
		policy = POLICY_FIFO;
	} else if (strcmp(argv[2], "PRIORITY") == 0) {
		policy = POLICY_PRIORITY;
	} else {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 5) {
		if (strcmp(argv[3], "-discovery-port") != 0 ||
		    parse_port(argv[4], &discovery_port) != 0) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	} else if (argc != 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (scheduler_run(port, policy, discovery_port) != 0) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
