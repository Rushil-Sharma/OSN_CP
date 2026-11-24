#ifndef OSN_NET_H
#define OSN_NET_H

#include <stddef.h>

typedef enum {
	RECV_ERR = -1,
	RECV_OK = 0,
	RECV_EOF = 1
} recv_status_t;

int send_all(int fd, const void *buf, size_t len);
recv_status_t recv_all(int fd, void *buf, size_t len);

#endif
