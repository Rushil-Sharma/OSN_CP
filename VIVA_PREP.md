# VIVA_PREP

Use this file as the shared index only. Each member reads only their own files plus this file. Do not edit code while preparing.

## Owners and files

| Owner | Files to study |
|---|---|
| M | `scheduler/main.c`, `scheduler/server.h`, `scheduler/server.c`, `scheduler/discovery.h`, `scheduler/discovery.c`, `common/log.h`, `common/log.c`, `Makefile`, `README.md` |
| H | `scheduler/state.h`, `scheduler/state.c`, `scheduler/commands.h`, `scheduler/commands.c`, `common/types.h`, `common/messages.h`, `common/messages.c` |
| R | `common/net.h`, `common/net.c`, `common/protocol.h`, `common/protocol.c`, `common/discovery.h`, `common/discovery.c`, `test/protocol_test.c`, `worker/main.c`, `client/main.c`, `CONTEXT.md`, `TODO` |

## TODO ownership map

`TODO` carries `(M)`, `(H)`, or `(R)` on every M1+ task. M0 tasks 4 and 5 stay untagged. Numbers below are TODO task indices.

| Milestone | M | H | R |
|---|---|---|---|
| M1 | 6, 7, 8, 12, 15, 23, 24, 25, 26, 27, 28, 37 | 22 | 9, 10, 11, 13, 14, 16, 17, 18, 19, 20, 21, 29, 30, 31, 32, 33, 34, 35, 36 |
| M2 | 40, 43, 59 | 38, 39, 41, 44, 46, 47, 49, 53, 54, 57, 58 | 42, 45, 48, 50, 51, 52, 55, 56 |
| M3 | 64, 66, 68, 69, 79, 81 | 60, 61, 62, 65, 67, 70, 71, 72, 73, 75, 80 | 63, 74, 76, 77, 78 |
| M4 | 84, 85, 87 | 86 | 82, 83 |
| Final | 88, 89, 90 | — | — |

Totals: M 27 tasks, H 24 tasks, R 34 tasks.

## Prep prompts

### M

- Explain `scheduler/main.c`, the listener in `scheduler/server.c`, detached session threads, and first-frame dispatch.
- Explain `common/log.{h,c}`: timestamped tags, mutex serialization, and state/decision helpers.
- Explain the build targets and flags in `Makefile`; `scheduler/server.o` is linked into the scheduler target.
- Explain scheduler CLI/startup, UDP responder, concurrency, bounded queue, FIFO/PRIORITY/aging, binary transfer, and the final scheduling pass.
- Treat `README.md` as target behavior, not as fully implemented behavior.

### H

- Explain `scheduler/state.{h,c}`: one mutex, one condition variable, ring queue, worker registry, session ids, and log buffer.
- Explain `common/types.h`: job/worker states, lifecycle fields, and fixed capacities.
- Explain `common/messages.{h,c}`: bounded encode/decode cursors and payload helpers.
- Own state-transition reasoning: worker IDs, job ownership, events, rescue/retries, WAITING/deps, cron state, caching metadata, and checkpoint status fields.

### R

- Explain `common/net.{h,c}` and `common/protocol.{h,c}`: exact-length I/O, 8-byte header, network byte order, version/type/length checks, EOF vs error.
- Explain `test/protocol_test.c`: socketpair-based framing tests and malformed frame cases.
- Explain `worker/main.c`, `client/main.c`, and `CONTEXT.md`; the entrypoints cover handshakes, worker resource flags, and reconnect retry, not the full M1 behavior.
- Own worker/client-facing work: CLI parsing, discovery client, username prompt, command loop, isolation, output capture, heartbeats, telemetry collection, crash reasons, draining, cache files, and follow-mode UX.

## Shared contract facts

- Concurrency model: thread-per-connection with mutex-protected scheduler state.
- Wire header: `uint16_t type`, `uint16_t version`, `uint32_t length`; network byte order.
- `OSN_PROTOCOL_VERSION` is 1; `OSN_MAX_PAYLOAD` is 64 MiB.
- `QUEUE_CAPACITY` is 1024; `MAX_ATTEMPTS` is 3.
- Scheduler policies: `FIFO` and `PRIORITY`; default discovery port is 9999.
- Build flags: `-std=c11 -Wall -Wextra -Werror -pthread`; platform feature macros set in `Makefile`.

## Current-repo cautions

- `scheduler/main.c`, `worker/main.c`, and `client/main.c` are handshake skeletons, not full M1 implementations.
- `README.md` describes broader target behavior than the current skeletons implement.
- `CONTEXT.md` and `TODO` are updated as milestones land; this file is only the prep index.
- `scheduler/server.c` is linked and runs, but scheduling, discovery, and command internals remain stubs.
