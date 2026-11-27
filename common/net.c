#include "net.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Send all requested bytes while retrying interrupted calls.
int send_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = send(fd, p + sent, len - sent, 0);

		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		return -1;
	}
	return 0;
}

// Receive all requested bytes and distinguish clean EOF from errors.
recv_status_t recv_all(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t received = 0;

	while (received < len) {
		ssize_t n = recv(fd, p + received, len - received, 0);

		if (n > 0) {
			received += (size_t)n;
			continue;
		}
		if (n == 0) {
			if (received == 0) {
				return RECV_EOF;
			}
			errno = ECONNRESET;
			return RECV_ERR;
		}
		if (errno == EINTR) {
			continue;
		}
		return RECV_ERR;
	}
	return RECV_OK;
}
