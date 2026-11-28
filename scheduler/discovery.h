#ifndef OSN_SCHEDULER_DISCOVERY_H
#define OSN_SCHEDULER_DISCOVERY_H

// Bind the UDP discovery socket and start a detached responder thread.
int discovery_responder_start(int discovery_port, int tcp_port);

#endif
