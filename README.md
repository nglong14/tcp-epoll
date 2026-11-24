# TCP server
- Blocking I/O: A server can serve multiple clients by giving each connection it own thread
- poll(): The server passes an array of sockets to the kernel and waits for any of them to become ready, O(n)
- epoll: The server registers sockets with the Linux kernel and updates those registration as needed. epoll_wait() returns a batch of ready events directly.
