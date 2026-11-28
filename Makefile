CC ?= cc
UNAME_S := $(shell uname -s)

CPPFLAGS += -Icommon
CFLAGS += -std=c11 -Wall -Wextra -Werror -pthread
LDFLAGS += -pthread

ifeq ($(UNAME_S),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
else
CFLAGS += -D_POSIX_C_SOURCE=200809L
endif

COMMON_SRCS := $(wildcard common/*.c)
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

SCHEDULER_BIN := scheduler/scheduler
WORKER_BIN := worker/worker
CLIENT_BIN := client/client
TEST_BIN := test/protocol_test

SCHEDULER_OBJS := scheduler/main.o scheduler/state.o scheduler/server.o scheduler/discovery.o
WORKER_OBJS := worker/main.o
CLIENT_OBJS := client/main.o
TEST_OBJS := test/protocol_test.o

ALL_BINS := $(SCHEDULER_BIN) $(WORKER_BIN) $(CLIENT_BIN) $(TEST_BIN)
CLEAN_OBJS := $(wildcard common/*.o scheduler/*.o worker/*.o client/*.o test/*.o)

.PHONY: all scheduler worker client test clean clear

all: $(ALL_BINS)

scheduler: $(SCHEDULER_BIN)
worker: $(WORKER_BIN)
client: $(CLIENT_BIN)
test: $(TEST_BIN)
	./$(TEST_BIN)

$(SCHEDULER_BIN): $(SCHEDULER_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(WORKER_BIN): $(WORKER_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(CLIENT_BIN): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_BIN): $(TEST_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(CLEAN_OBJS) $(ALL_BINS)

clear: clean
