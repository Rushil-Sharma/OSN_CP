#ifndef OSN_SCHEDULER_SCHEDULE_H
#define OSN_SCHEDULER_SCHEDULE_H

#include "state.h"

// Run one scheduling pass over the current QUEUED jobs and ALIVE workers.
void scheduler_run_pass(scheduler_state_t *state);

#endif
