# VIVA_PREP

Use this file as the shared index only. Each member reads only their own files plus this file. Do not edit code while preparing.

## Owners and files

| Owner | Files to study |
|---|---|
| M | `scheduler/main.c`, `scheduler/server.h`, `scheduler/server.c`, `common/log.h`, `common/log.c`, `Makefile`, `README.md` |
| H | `scheduler/state.h`, `scheduler/state.c`, `common/types.h`, `common/messages.h`, `common/messages.c` |
| R | `common/net.h`, `common/net.c`, `common/protocol.h`, `common/protocol.c`, `test/protocol_test.c`, `worker/main.c`, `client/main.c`, `CONTEXT.md`, `TODO` |

## TODO ownership map

`TODO` carries `(M)`, `(H)`, or `(R)` on every M1+ task. M0 lines 15 and 20 stay untagged.

| Milestone | M | H | R |
|---|---|---|---|
| M1 | 26, 27, 31, 40, 44, 59, 60, 62, 65, 67, 70, 92 | 55 | 34, 36, 37, 41, 43, 47, 48, 49, 52, 53, 54, 74, 75, 81, 82, 84, 85, 88, 89 |
| M2 | 101, 107, 137 | 98, 99, 102, 110, 112, 113, 119, 127, 128, 133, 134 | 106, 111, 116, 122, 123, 124, 129, 132 |
| M3 | 151, 154, 158, 160, 174, 179 | 144, 146, 147, 153, 157, 163, 164, 165, 166, 170, 175 | 150, 169, 171, 172, 173 |
| M4 | 188, 190, 193 | 192 | 186, 187 |
| Final | 198, 202, 203 | — | — |

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
