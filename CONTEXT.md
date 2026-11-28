# CONTEXT

Shared handoff for the OSN distributed compute cluster. This file records locked decisions and completed changes.

## Locked decisions

- Concurrency model: thread-per-connection, mutex-protected scheduler state, dedicated scheduling thread.
- Wire format: 8-byte header with `uint16_t type`, `uint16_t version`, `uint32_t length`, encoded in network byte order; raw padded structs are not sent.
- Protocol version: `OSN_PROTOCOL_VERSION` 1; max payload `OSN_MAX_PAYLOAD` 64 MiB.
- Queue capacity: `QUEUE_CAPACITY` 1024; retry limit `MAX_ATTEMPTS` 3 as a `#define`.
- Scheduler policies: `FIFO` and `PRIORITY`; default discovery port `9999`.
- Build flags: `-std=c11 -Wall -Wextra -Werror -pthread`; `_POSIX_C_SOURCE=200809L` on non-Darwin, `_DARWIN_C_SOURCE` on macOS.
- Commit convention: Conventional Commits directly to `main`, `type(scope): imperative lower-case subject`.
- TODO marking: plain `[x]` after completion; M1+ tasks carry `(M)`, `(H)`, or `(R)` owner tags; no section split.
- C comments: one-line `//` explanations immediately before code blocks.

## Completed changes

- Added root `Makefile` with `all`, `scheduler`, `worker`, `client`, `test`, `clean`, and `clear` targets.
- `clean` removes generated objects by wildcard and all four binaries; `clear` aliases `clean`.
- `common/*.c` sources are discovered by wildcard and linked into `scheduler/scheduler`, `worker/worker`, `client/client`, and `test/protocol_test`.
- Added `common/types.h` with central capacities, full job/worker state enums, telemetry, job, and worker structs.
- Added `common/messages.{h,c}` with bounded encode/decode helpers for hello, register, acks, and error payloads.
- Added `scheduler/state.{h,c}` with one mutex, one condition variable, job/worker state, queue, log, and subscription placeholders.
- Added `scheduler/server.{h,c}` with a TCP listener, detached session threads, first-frame dispatch, and a dedicated scheduling thread with a 1-second tick.
- `scheduler/main.c` parses `<port> <FIFO|PRIORITY> [-discovery-port N]` with default discovery port `9999`.
- `worker/main.c` parses `<address> <port> [-cores N] [-ram MB] [-gpu N]`, clamps cores and RAM to host limits with warnings, sends registration, and retries through fallback rediscovery after lost connections.
- `client/main.c` parses address, port, and `-user`, sends a hello skeleton, and prints the session id from `MSG_HELLO_ACK`.
- Extended `test/protocol_test.c` with truncated payload, invalid type, and payload-helper tests.
- Added `common/log.{h,c}` with thread-safe timestamped `[SCHEDULER]` / `[WORKER <id>]` logging and event helpers.
- Worker/job state transitions, scheduling decisions, and discovery outcomes route through common logging helpers.
- Added one-line `//` comments across all C sources and headers.
- Added this `CONTEXT.md` summary.

## Existing M0 pieces

- `.gitignore` ignores `*.o` and all four binaries.
- `common/net.{h,c}` provides `send_all()`, `recv_all()`, `RECV_OK`, `RECV_EOF`, and `RECV_ERR`.
- `common/protocol.{h,c}` provides all `MSG_*` types, header encode/decode, `send_msg()`, `recv_msg()`, version/type/bounds checks, and `msg_type_name()`.
- M1+ behavior is isolated in named stubs such as `m1_scheduling_pass_stub`, `m1_client_command_stub`, `m2_worker_message_stub`, and `kill_running_jobs_stub`.
- `TODO` tasks 1-11 are marked `[x]`; remaining M1+ items are still `[ ]`.

## Verification performed

- `make clean && make all && make test` passed.
- Smoke check against `./scheduler/scheduler 9000 FIFO` printed `HELLO_ACK session 1` and `WORKER_REGISTER_ACK worker 1`.
- Worker smoke check with `-cores 999 -ram 999999999 -gpu 2` logged clamped resource warnings and printed `WORKER_REGISTER_ACK worker 1`.
- Build outputs stay ignored by `.gitignore`.
