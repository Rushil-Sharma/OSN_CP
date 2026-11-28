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
- Added `common/discovery.{h,c}`: shared UDP discovery client (2 s timeout, 3 attempts) that sends to 255.255.255.255, every interface broadcast address, and 127.0.0.1, then falls back to the CLI address; also encodes/decodes the discovery datagrams.
- Discovery wire format: request is a bare 8-byte header `MSG_DISCOVER_REQ`; reply is header `MSG_DISCOVER_REPLY` + 4-byte IPv4 + 2-byte TCP port, network byte order. IPv4 `0.0.0.0` means "use the sender address".
- Added `scheduler/discovery.{h,c}`: UDP responder thread bound with `SO_REUSEADDR` + `SO_REUSEPORT`; replies with the local IPv4 the kernel routes to the requester plus the scheduler TCP port. Startup fails if the bind fails.
- `worker/main.c` CLI is now `[<ip> <port>] [-cores N] [-ram MB] [-gpu N] [-discovery-port N]`; it runs discovery before every connect, stays connected after registering, and on a drop kills jobs, rediscovers, and re-registers. No reply and no fallback -> error + exit.
- `client/main.c` CLI is now `[<ip> <port>] [-user <name>] [-discovery-port N]` with discovery + fallback.
- Client login (TODO 17-18): without `-user` it prompts `Username:` until the name is 1-31 chars of `[A-Za-z0-9_.-]` (`msg_username_valid()` in `common/messages.c`, also enforced by the scheduler). The username is the first frame (`MSG_CLIENT_HELLO`); on any lost connection the client rediscovers, reconnects (3 rounds) and resends it without prompting. Duplicate usernames are accepted as separate sessions.
- Client prompt (TODO 19-21): one `poll()` loop over stdin + the scheduler socket, so `[Event]` lines print while idle at `client>` (on a terminal the prompt line is cleared first). Commands: `nodes`, `submit`, `status`, `logs`, `cancel`, `help`, `exit`. `submit <path|command> [args] -cores N -ram MB -time S -prio 1-3 [-gpu N]`: tokens before the first flag are the command + args; the four flags are required, `-prio` is 1-3, `-gpu` defaults to 0; `-depends-on/-group/-preemptible/-every` are refused as not supported yet. `QUEUE_FULL` prints "Queue is full".
- New wire payloads in `common/messages.{h,c}`: job id (status/cancel/submit ack), submit, status reply, nodes reply (41-byte rows), logs request/chunk, cancel ack (new `MSG_CANCEL_ACK`), event text. Decoders reject trailing bytes. Error codes are the `msg_error_code_t` enum.
- `send_msg()` now writes header + payload in one `send()`; two writes hit Nagle + delayed ACK (~40-90 ms per request after the first few).
- Added `scheduler/commands.{h,c}`: per-client session loop (replaces `client_session`/`m1_client_command_stub` in `server.c`) handling SUBMIT, STATUS_REQ, NODES_REQ, LOGS_REQ, CANCEL; anything else gets `MSG_ERR_UNSUPPORTED`.
- `scheduler/state.{h,c}` (TODO 22): client session table (`MAX_CLIENT_SESSIONS` 256, per-session `send_lock`, `SO_SNDTIMEO` 2 s); `scheduler_submit_job()` creates an owned QUEUED job and enqueues it, returning `QUEUE_FULL` instead of blocking; `scheduler_set_job_state_reason_locked()` logs every job transition and pushes an `[Event]` only to sessions logged in as the owner (spec wording for RUNNING, COMPLETED, FAILED + reason, LOST->QUEUED, WAITING->QUEUED, CANCELLED); worker -> SUSPECTED/DEAD pushes a WARNING once to each owner with a RUNNING job there. Lock order is state lock -> session `send_lock`.
- Cancel currently handles QUEUED (removed from the ring queue) and WAITING jobs; RUNNING/LOST return "not implemented yet" until TODO 45. `logs -f` returns "not implemented yet" until TODO 54-55. Scheduler log lines for client actions name the user (`Job N SUBMITTED by alice`, `Job N QUEUED -> CANCELLED (requested by bob)`).
- The job table is `jobs[QUEUE_CAPACITY]` and never frees slots, so after 1024 submissions in total, submit returns `MSG_ERR_TABLE_FULL`.
- `common/log.{h,c}` gained `LOG_CLIENT` (`[Client]` tag); `log_discovery_outcome()` now logs responder outcomes per requester.

## Existing M0 pieces

- `.gitignore` ignores `*.o` and all four binaries.
- `common/net.{h,c}` provides `send_all()`, `recv_all()`, `RECV_OK`, `RECV_EOF`, and `RECV_ERR`.
- `common/protocol.{h,c}` provides all `MSG_*` types, header encode/decode, `send_msg()`, `recv_msg()`, version/type/bounds checks, and `msg_type_name()`.
- M1+ behavior is isolated in named stubs such as `m1_scheduling_pass_stub`, `m2_worker_message_stub`, and `kill_running_jobs_stub`.
- `TODO` tasks 1-14 and 16-22 are marked `[x]`; task 15 (hotspot test on Linux + macOS) is manual and still `[ ]`.

## Verification performed

- `make clean && make all && make test` passed.
- Smoke check against `./scheduler/scheduler 9000 FIFO` printed `HELLO_ACK session 1` and `WORKER_REGISTER_ACK worker 1`.
- Worker smoke check with `-cores 999 -ram 999999999 -gpu 2` logged clamped resource warnings and printed `WORKER_REGISTER_ACK worker 1`.
- Build outputs stay ignored by `.gitignore`.
- Discovery (WSL Debian, `-discovery-port 19999`): worker and client with no fallback found the scheduler; a malformed datagram was logged and ignored; killing the scheduler and restarting it at once on the same ports worked, and the worker rediscovered and re-registered; with no scheduler the fallback line printed after 3 x 2 s; with no fallback the worker exited with an error after 6 s.
- Client/commands (WSL Debian): the prompt re-asked for an empty name, a name with a space, a 32-char name and `semi;colon`, then accepted `alice`; `nodes`, `submit` (missing flag, `-prio 4`, `-cores 0`, `-depends-on`, trailing arg all rejected; valid submits got ids 1 and 2), `status` (existing, missing, non-numeric id), `logs`, `logs -f`, `cancel` (QUEUED, already CANCELLED, missing) all replied correctly. A second idle `alice` session got both cancel events at its prompt and an idle `bob` got none, including when bob cancelled alice's job. A client idle while the scheduler restarted reconnected and logged in again by itself. 1025 submits: 1024 accepted, the 1025th got QUEUE_FULL, in under 1 s.
- Not exercised end to end yet: RUNNING/COMPLETED/FAILED events and worker SUSPECTED/DEAD warnings, because no job runs until the scheduling pass (TODO 26) exists.
