# TCP Echo Lab

Learn TCP sockets, `poll`, and `epoll` by building a single-threaded C++17
echo server on Linux, using the socket APIs directly.

## Current phase

Phase 3 adds a single-threaded, nonblocking TCP echo server using `poll`.
It serves up to 128 clients with at most 64 KiB of queued output per client.
The Phase 2 blocking server remains available to reproduce the single-client
bottleneck. Both default to `127.0.0.1:9090`; run them separately or use
different port arguments.

## Build and run

Required tools: Linux, `g++`, and `make`. Later phases use Python 3 and `nc`
for client experiments. `strace` is optional for syscall inspection.

```sh
make
./blocking_server
```

For the concurrent Phase 3 server, run `./poll_server` instead. It also accepts
an optional port, for example `./poll_server 9091`.

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

Run all repeatable checks and remove build output with:

```sh
make test
make test-poll  # Phase 3 only
make clean
```

The checks start their own server on a temporary localhost port and stop it
afterward. They require permission to open local sockets.

The build uses C++17, compiler warnings, debug symbols, and no optimization:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -O0 -o blocking_server blocking_server.cpp
```

`make` builds both servers. `make blocking_server` and `make poll_server` build
individual variants. `make clean` removes server and test executables. The
epoll build target will be added when its source exists.

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
| Phase 3: idle, slow, active clients | Active clients progress while others wait | Ten short binary echoes completed while one client was idle and another delayed reads; the 8 MiB slow-client echo matched exactly | Nonblocking I/O yields to the event loop; each client retains its own pending output | poll, recv, send |
| Phase 3: queue limits | Full queues pause reads and retain unsent bytes | Socket-pair checks filled the 64 KiB queue, forced EAGAIN and positive short sends, and verified read resumption below 32 KiB | Interest masks follow queue state and send offsets | recv, send, poll interest calculation |
| Phase 3: EOF and cleanup | Pending output survives EOF and resources are released | EOF flush checks passed; descriptor count settled after 100 connect/disconnect cycles and reset recovery; excess client was rejected at 128 clients | Read EOF and connection close are separate transitions | shutdown, recv, send, close |
| Phase 3: idle CPU | The event loop sleeps when no client has work | Server CPU ticks remained within the check's five-tick allowance over 300 ms | Empty output queues do not request POLLOUT; poll uses an infinite timeout | poll |

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

## Phase 3 connection model

`poll_server.cpp` keeps connection state in `std::unordered_map<int, Connection>`.
A `std::vector<pollfd>` is rebuilt from that map before each `poll(..., -1)`.
The vector is a snapshot, so removing connections does not invalidate traversal.
New clients are accepted after processing the snapshot, preventing newly reused
descriptor numbers from receiving stale readiness events.

The listener and every accepted socket use `O_NONBLOCK`, preserving existing
`fcntl` flags. `accept` and `recv` drain available work until `EAGAIN`, an error,
EOF, or a queue bound. Each read pass queues at most 64 KiB before trying sends,
so one streaming client cannot keep the receive loop running indefinitely.

Readiness is permission to attempt I/O. `events` is the requested mask;
`revents` is the returned mask. Check flags with bitwise `&`, because several
can occur together. `EAGAIN`/`EWOULDBLOCK` means yield and wait for readiness;
`EINTR` means retry the interrupted operation.

| State | Meaning |
| --- | --- |
| `pending` | Bytes received for echo, including a possibly sent prefix |
| `sent_offset` | Prefix already accepted by successful sends |
| `read_eof` | `recv` returned zero; no more reads are needed |
| `read_paused` | Stop reading until queued output drops below the low-water mark |

Unsent output is `pending.size() - sent_offset`. The server attempts sends
after queuing a read batch and watches `POLLOUT` only if bytes remain. A short
send advances the offset; `EAGAIN` preserves the suffix for a later pass.
Leaving `POLLOUT` enabled on an empty queue would repeatedly wake `poll`.

At 64 KiB of unsent output, reads pause. Below 32 KiB they resume. Each receive
is limited to remaining capacity, and consumed prefixes are compacted before
appending. The output vector reserves 64 KiB per accepted client. These bounds
cover application output storage; kernel socket buffers are separate. Pausing
reads eventually pushes pressure back through TCP to a sender whose peer is
not reading fast enough.

`recv == 0` stops reads but does not discard queued output: the connection
closes only after that output drains. `POLLHUP` may coexist with unread data,
so it triggers a receive attempt rather than immediate close. Fatal client
errors close only that connection; listener failures stop the server.

## Phase 3 acceptance checks

`make test-poll` runs two checks:

- `test_poll_state.cpp` uses nonblocking socket pairs and a small send buffer
  to force `EAGAIN` and positive partial sends. It checks exact bytes, queue
  limits, read pause/resume, conditional `POLLOUT`, and output flushing after EOF.
- `test_poll.py` runs the actual TCP server and checks startup errors, binary
  echo, concurrent idle/slow/active clients, an 8 MiB payload with delayed reads,
  half-close, idle CPU use, repeated connection cleanup, reset recovery, and the
  128-client limit. It reads Linux `/proc` for descriptor and CPU observations.

To observe concurrency manually, open two `nc` clients against `poll_server`.
Leave the first idle and type in the second: its echo should arrive immediately.
The same experiment against `blocking_server` waits for the first to disconnect.

These checks do not force signal interruption or constitute a load benchmark.
There is no application shutdown handler; Ctrl+C lets the OS release descriptors.

## Project documentation

[Notion: Wiki / Projects / tcp](https://app.notion.com/p/41d0df90dde081338243e96c654efcf4)
contains the implementation roadmap and detailed phase plans.
