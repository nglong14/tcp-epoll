# TCP Echo Lab

Learn TCP sockets, `poll`, and `epoll` by building a single-threaded C++17
echo server on Linux, using the socket APIs directly.

## Current phase

Phase 1 provides a buildable entry point and socket model notes.
The executable prints the planned lifecycle and exits; it does not open sockets.
Phase 2 will add a blocking server bound to `127.0.0.1:9090`.

## Build and run

Required tools: Linux, `g++`, and `make`. Later phases use Python 3 and `nc`
for client experiments. `strace` is optional for syscall inspection.

```sh
make
./blocking_server
make clean
```

The build uses C++17, compiler warnings, debug symbols, and no optimization:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -O0 -o blocking_server blocking_server.cpp
```

`make` and `make blocking_server` build the same executable. `make clean`
removes the three planned server executables. Poll and epoll build targets
will be added when their source files exist.

## Roadmap

| Phase | Time | Deliverable |
| --- | --- | --- |
| 1 — Setup and socket model | 15 min | Build workflow and lifecycle notes |
| 2 — Blocking TCP baseline | 30 min | Echo bytes for one client at a time |
| 3 — Concurrent connections with poll | 60 min | Nonblocking sockets and bounded output queues |
| 4 — Level-triggered epoll | 40 min | Equivalent behavior using epoll registrations |
| 5 — Edge-triggered epoll | 40 min | Drain readiness and reproduce an unread-data stall |
| 6 — Validation and observations | 55 min | Repeatable client checks and syscall observations |

The server stays in C++; the eventual client harness uses Python's standard
library. No external networking framework is needed.

## Socket model

A file descriptor is a process-local integer used to refer to an open resource.
A listening socket waits for incoming connections. `accept` returns a new
descriptor for one connection; the listener remains available to accept others.
The connected descriptor is used to receive and send that client's bytes.

```text
socket
  -> setsockopt(SO_REUSEADDR)
  -> bind(127.0.0.1:9090)
  -> listen
  -> accept --------------------> connected client descriptor
       ^                            -> recv / send
       |                            -> close client
       +-------------------------------+

close listener when the server exits
```

| Call | Role |
| --- | --- |
| `socket` | Create a TCP socket descriptor |
| `setsockopt` | Configure the socket before binding |
| `bind` | Assign a local address and port |
| `listen` | Make the socket a listener with a pending connection queue |
| `accept` | Obtain a separate descriptor for an incoming connection |
| `recv` / `send` | Receive / send bytes on the connected descriptor |
| `close` | Release a descriptor owned by this process |

### TCP is a byte stream

TCP delivers an ordered stream of bytes, without application message boundaries.
Two sends of `hello` and `world` might be received together or split across
several receives. The application must use the actual byte count each call
returns. A successful `send` does not prove that the peer application has read
those bytes.

### Readiness allows an I/O attempt

`poll` and `epoll` report readiness. They do not complete the application's
reads or writes. Readable can mean incoming bytes, EOF, or an error; writable
does not guarantee room for the entire pending response. Nonblocking calls can
still return `EAGAIN` when an operation would currently block.

### Ownership and failure handling

The server owns the listener and each accepted descriptor. Close each owned
descriptor once when its work ends, including error paths. Closing one client
must leave the listener available for subsequent clients. Descriptor numbers
can be reused after close, so old connection state must be removed.

In Phase 2, check socket setup results, report errors using `perror` or an
equivalent errno message, and exit cleanly on startup failure. Invalid-port and
occupied-port checks become relevant when the listener is implemented.

## Experiment log

Add actual observations after running each experiment; predictions alone do
not count as verification.

| Experiment | Prediction | Observation | Explanation | Relevant syscall |
| --- | --- | --- | --- | --- |
| Phase 1: build and run | Lifecycle text is printed and the process exits successfully | Build completed without warnings; run exited successfully; clean and rebuild passed | The entry point currently demonstrates the model | None yet |

## Phase 1 acceptance checks

- Build with `make` without compiler warnings.
- Run `./blocking_server` and confirm the lifecycle text and successful exit.
- Run `make clean`, then rebuild successfully.
- Explain why `accept` returns a separate descriptor.
- Explain why one send need not match one receive.
- Explain why readiness is followed by an actual I/O call.

## Project documentation

[Notion: Wiki / Projects / tcp](https://app.notion.com/p/41d0df90dde081338243e96c654efcf4)
contains the implementation roadmap and detailed phase plans.
