#ifndef OSN_SCHEDULER_SERVER_H
#define OSN_SCHEDULER_SERVER_H

#include "state.h"

// Run the threaded TCP scheduler until the accept loop stops.
int scheduler_run(int port, policy_t policy, int discovery_port);

#endif
