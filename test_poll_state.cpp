// Exercise queue boundaries with real nonblocking sockets and a full send buffer.
#define main poll_server_main
#include "poll_server.cpp"
#undef main
#include <cassert>

int main() {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const int server = sockets[0];
    const int peer = sockets[1];
    assert(nonblocking(server) && nonblocking(peer));
    const int small_buffer = 4096;
    assert(setsockopt(server, SOL_SOCKET, SO_SNDBUF,
                      &small_buffer, sizeof(small_buffer)) == 0);

    std::array<char, 4096> buffer{};
    while (send(server, buffer.data(), buffer.size(), MSG_NOSIGNAL) > 0) {}
    assert(would_block());

    const std::vector<char> payload(high_water, 'x');
    assert(send(peer, payload.data(), payload.size(), MSG_NOSIGNAL)
           == static_cast<ssize_t>(payload.size()));
    Connection connection;
    assert(receive_pending(server, connection));
    assert(connection.unsent() == high_water && connection.read_paused);
    assert(interests(connection) == POLLOUT);
    assert(send_pending(server, connection));
    assert(connection.unsent() == high_water); // EAGAIN preserves all pending bytes.

    // Discard only the filler used to saturate the kernel send buffer.
    while (recv(peer, buffer.data(), buffer.size(), 0) > 0) {}
    assert(would_block());

    assert(send_pending(server, connection));
    assert(connection.sent_offset > 0 && connection.unsent() > low_water);
    assert(connection.read_paused);
    std::vector<char> echoed;
    while (connection.unsent() != 0) {
        ssize_t count;
        while ((count = recv(peer, buffer.data(), buffer.size(), 0)) > 0) {
            echoed.insert(echoed.end(), buffer.data(), buffer.data() + count);
        }
        assert(would_block());
        assert(send_pending(server, connection));
        assert(connection.unsent() <= high_water);
        if (connection.unsent() < low_water) {
            assert(!connection.read_paused);
        } else {
            assert(connection.read_paused);
        }
    }
    ssize_t count;
    while ((count = recv(peer, buffer.data(), buffer.size(), 0)) > 0) {
        echoed.insert(echoed.end(), buffer.data(), buffer.data() + count);
    }
    assert(echoed == payload);
    assert(interests(connection) == POLLIN); // No idle POLLOUT wakeups.
    assert(connection.pending.capacity() == high_water);

    assert(shutdown(peer, SHUT_WR) == 0);
    assert(receive_pending(server, connection) && connection.read_eof);
    // EOF must not close while bytes remain unsent.
    connection.pending.assign(payload.begin(), payload.end());
    assert(service_client(server, POLLOUT, connection));
    assert(connection.read_eof && connection.unsent() > 0);
    assert(interests(connection) == POLLOUT);
    while (connection.unsent() != 0) {
        while (recv(peer, buffer.data(), buffer.size(), 0) > 0) {}
        const bool keep = service_client(server, POLLOUT, connection);
        assert(keep == (connection.unsent() != 0));
    }
    close(server);
    close(peer);
    std::cout << "PASS: queue limits, partial sends, EAGAIN, read resume, EOF flush\n";
}
