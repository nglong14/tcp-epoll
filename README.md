# TCP Echo Lab

Learn TCP sockets, `poll`, and `epoll` by building a single-threaded C++17
echo server on Linux, using the socket APIs directly.

## Current phase

Phase 2 implements a blocking TCP echo server bound to `127.0.0.1:9090`.
It echoes raw bytes for one connected client at a time. An idle or slow client
can delay every subsequent client; Phase 3 will address this using `poll`.

## Build and run

Required tools: Linux, `g++`, and `make`. Later phases use Python 3 and `nc`
for client experiments. `strace` is optional for syscall inspection.

```sh
make
./blocking_server
```

In a second terminal, connect and type a message:

```sh
nc 127.0.0.1 9090
```

Each line is echoed back. Exit `nc` with Ctrl+C to disconnect, and use Ctrl+C
in the server terminal to stop the server. There is no application shutdown
handler; the OS releases descriptors when the process exits.

An optional port argument selects another port:

```sh
./blocking_server 9091
nc 127.0.0.1 9091
```

Ports must be decimal integers from 1 to 65535. Startup errors, including an
occupied port, print a diagnostic and return a nonzero exit status.

Run the repeatable Phase 2 checks and remove build output with:

```sh
make test
make clean
```

The checks start their own server on a temporary localhost port and stop it
afterward. They require permission to open local sockets.

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

The blocking server checks setup results, reports syscall errors using `perror`,
and exits cleanly on startup failure. Interrupted `accept`, `recv`, and `send`
calls retry on `EINTR`. Connection errors end the current client's session;
the server closes that descriptor and returns to `accept`.

Each receive uses a 4 KiB buffer. A send loop advances by the actual number
of bytes sent, so a short send does not discard the remaining bytes.
`MSG_NOSIGNAL` prevents a failed send from terminating the server via `SIGPIPE`.
On receive EOF, every earlier received byte has already been sent back, so
the server closes the client and accepts the next connection.

## Experiment log

Add actual observations after running each experiment; predictions alone do
not count as verification.

| Experiment | Prediction | Observation | Explanation | Relevant syscall |
| --- | --- | --- | --- | --- |
| Phase 1: build and run | Lifecycle text is printed and the process exits successfully | Build completed without warnings; run exited successfully; clean and rebuild passed | The entry point currently demonstrates the model | None yet |
| Phase 2: binary and large echo | All bytes return in order | Binary payload and 1 MiB transfer matched exactly | Receive counts and send offsets preserve byte lengths | recv, send |
| Phase 2: half-close | Echo is delivered before server EOF | Full response followed by EOF | Client write shutdown leaves its receive direction open | shutdown, recv, close |
| Phase 2: second client | Connection may establish but echo waits | Second receive timed out while first was idle; echo arrived after first sent EOF | Kernel queues connections while the application handles the first client | listen, accept, recv |
| Phase 2: reset recovery | Client failure leaves server available | Echo succeeded on a new connection after a reset | Connection failure returns to the accept loop | recv, send, close |

## Phase 1 acceptance checks

- Build with `make` without compiler warnings.
- The original Phase 1 lifecycle-printing stub built and ran successfully; Phase 2 replaces it with the running server.
- Run `make clean`, then rebuild successfully.
- Explain why `accept` returns a separate descriptor.
- Explain why one send need not match one receive.
- Explain why readiness is followed by an actual I/O call.

## Phase 2 acceptance checks

`make test` verifies invalid and occupied ports, empty connections, short and
binary echoes, a 1 MiB transfer, half-close followed by EOF, second-client delay,
and recovery after a peer reset. The large-transfer client sends and receives
concurrently to avoid creating a client-side deadlock against the blocking echo.

The server handles partial writes explicitly. Local TCP tests do not guarantee
that a positive short `send` or an interrupted syscall will occur on every run.

To reproduce the concurrency limitation manually, open two `nc` clients.
Send text from the first and leave it open. Text from the second will not be
echoed until the first disconnects. A successful TCP connection only confirms
that the kernel established it; the application may still be busy before its
next `accept` call.

## Project documentation

[Notion: Wiki / Projects / tcp](https://app.notion.com/p/41d0df90dde081338243e96c654efcf4)
contains the implementation roadmap and detailed phase plans.
