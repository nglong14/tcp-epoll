# TCP echo lab

Build the three C++ servers with `make`. Run the level-triggered epoll server
with `./epoll_server lt 9090`. Both arguments are optional. The server listens
on `127.0.0.1` and echoes raw bytes.

Run `make test` for all servers, or `make test-epoll` for the epoll checks.
The poll and epoll servers run the same integration checks for binary echo,
concurrent clients, backpressure, half-close, and socket cleanup.
