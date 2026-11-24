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
- TODO marking: plain `[x]`, no initials, no section split.

## Completed changes

- Added root `Makefile` with `all`, `scheduler`, `worker`, `client`, `test`, and `clean` targets.
- `common/*.c` sources are discovered by wildcard and linked into `scheduler/scheduler`, `worker/worker`, `client/client`, and `test/protocol_test`.
- Added this `CONTEXT.md` summary.

## Existing M0 pieces

- `.gitignore` ignores `*.o` and all four binaries.
- `common/net.{h,c}` provides `send_all()`, `recv_all()`, `RECV_OK`, `RECV_EOF`, and `RECV_ERR`.
- `common/protocol.{h,c}` provides all `MSG_*` types, header encode/decode, `send_msg()`, `recv_msg()`, version/type/bounds checks, and `msg_type_name()`.
- `test/protocol_test.c` covers header round-trip, payload round-trip, empty payload, clean EOF, bad version, oversize length, and partial header.
- `scheduler/main.c`, `worker/main.c`, and `client/main.c` are still empty stubs.
- `TODO` has 9-14 marked `[x]`; 15-20 and all M1+ items remain `[ ]`.

## Verification performed

- `make clean && make all && make test` passed.
- Build outputs stay ignored by `.gitignore`.
