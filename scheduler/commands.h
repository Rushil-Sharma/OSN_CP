#ifndef OSN_SCHEDULER_COMMANDS_H
#define OSN_SCHEDULER_COMMANDS_H

#include <stdint.h>

#include "state.h"

// Log in a client from its hello frame, then serve its commands until it disconnects.
void client_session_run(int fd, scheduler_state_t *state, const uint8_t *hello, uint32_t hello_len);

#endif
