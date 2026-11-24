# courseproject

Distributed compute cluster in C: a scheduler, workers, and clients over TCP.
Runs on Linux and macOS.

## Build

```sh
make            # builds scheduler/scheduler, worker/worker, client/client, test/protocol_test
make scheduler  # or: make worker / make client
make clean
```

## Test

```sh
make test
```

## Run

Start the scheduler first, then any number of workers and clients.

```sh
# Scheduler: <port> <FIFO|PRIORITY> [-discovery-port N]  (discovery port defaults to 9999)
./scheduler/scheduler 8080 PRIORITY

# Worker: finds the scheduler via UDP discovery; IP/port are only a fallback.
# Run with sudo so jobs are sandboxed as the `nobody` user.
sudo ./worker/worker -cores 4 -ram 2048 -gpu 1
sudo ./worker/worker 127.0.0.1 8080 -cores 4 -ram 2048 -discovery-port 9999

# Client: same discovery + fallback; -user skips the username prompt.
./client/client
./client/client 127.0.0.1 8080 -user lordz -discovery-port 9999
```

Several workers on one machine need separate caches:

```sh
sudo OSN_CACHE_DIR=./cache1 OSN_CACHE_SIZE_MB=512 ./worker/worker -cores 2 -ram 1024
sudo OSN_CACHE_DIR=./cache2 ./worker/worker -cores 2 -ram 1024
```

### Client commands

```
nodes
submit <path|command> -cores N -ram MB -time S -prio 1-3 [-gpu N]
status <job_id>
logs <job_id> [-f]
cancel <job_id>
```
