#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr std::size_t max_clients = 128;
constexpr std::size_t high_water = 64 * 1024; // If unsent data reaches this, stop reading
constexpr std::size_t low_water = 32 * 1024; // If unsent data drains below this, resume reading

struct Connection {
    std::vector<char> pending;
    std::size_t sent_offset = 0;
    bool read_eof = false;
    bool read_paused = false;

    Connection() { pending.reserve(high_water); }
    std::size_t unsent() const { return pending.size() - sent_offset; }
};

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

// Make one file descriptor non-blocking, so recv/send/accept on it return immediately with EAGAIN
// instead of freezing the whole server thread
bool nonblocking(int fd) {
    // Read half
    int flags;
    do {
        flags = fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        std::perror("fcntl(F_GETFL)");
        return false;
    }
    // Write half
    int result;
    do {
        result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        std::perror("fcntl(F_SETFL)");
        return false;
    }
    return true;
}

short interests(const Connection& connection) {
    short events = 0;
    if (!connection.read_eof && !connection.read_paused) {
        events |= POLLIN;
    }
    if (connection.unsent() != 0) {
        events |= POLLOUT;
    }
    return events;
}

bool receive_pending(int fd, Connection& connection) {
    // Reclaim the sent prefix before appending; storage stays within 64 KiB.
    connection.pending.erase(connection.pending.begin(),
                             connection.pending.begin() + connection.sent_offset);
    connection.sent_offset = 0;
    std::array<char, 4096> buffer;
    while (connection.unsent() < high_water) {
        const auto capacity = std::min(buffer.size(), high_water - connection.unsent());
        const ssize_t count = recv(fd, buffer.data(), capacity, 0);
        if (count > 0) {
            connection.pending.insert(connection.pending.end(), buffer.data(),
                                      buffer.data() + count);
        } else if (count == 0) {
            connection.read_eof = true;
            return true;
        } else if (errno == EINTR) {
            continue;
        } else if (would_block()) {
            return true;
        } else {
            std::perror("recv");
            return false;
        }
    }
    connection.read_paused = true;
    return true;
}

bool send_pending(int fd, Connection& connection) {
    while (connection.unsent() != 0) {
        const ssize_t count = send(fd, connection.pending.data() + connection.sent_offset,
                                   connection.unsent(), MSG_NOSIGNAL);
        if (count > 0) {
            connection.sent_offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && would_block()) {
            break;
        } else {
            if (count < 0) {
                std::perror("send");
            } else {
                std::cerr << "send: no progress\n";
            }
            return false;
        }
    }
    if (connection.unsent() == 0) {
        connection.pending.clear();
        connection.sent_offset = 0;
    }
    if (connection.read_paused && connection.unsent() < low_water) {
        connection.read_paused = false;
    }
    return true;
}

bool service_client(int fd, short events, Connection& connection) {
    if (events & POLLNVAL) {
        std::cerr << "poll: invalid client fd " << fd << " (bookkeeping bug)\n";
        return false;
    }
    if (events & POLLERR) {
        std::cerr << "poll: connection error on fd " << fd << '\n';
        return false;
    }
    // HUP can accompany unread bytes. Only recv == 0 establishes read EOF.
    if ((events & (POLLIN | POLLHUP)) && !connection.read_eof
        && !connection.read_paused && !receive_pending(fd, connection)) {
        return false;
    }
    // Try immediately after receiving; POLLOUT is only needed if bytes remain.
    if (!send_pending(fd, connection)) {
        return false;
    }
    return !(connection.read_eof && connection.unsent() == 0);
}

bool accept_clients(int listener, std::unordered_map<int, Connection>& connections) {
    for (;;) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (would_block()) {
                return true;
            }
            std::perror("accept");
            return false;
        }
        // Linux accept does not inherit O_NONBLOCK from the listener.
        if (connections.size() >= max_clients || !nonblocking(client)) {
            close(client);
            continue;
        }
        connections.try_emplace(client);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    int port = 9090;
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        const auto result = std::from_chars(argument.data(),
                                            argument.data() + argument.size(), port);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size()
            || port < 1 || port > 65535) {
            std::cerr << "Invalid port: use an integer from 1 to 65535\n";
            return 1;
        }
    }

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }
    const int reuse = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!nonblocking(listener)) {
        close(listener);
        return 1;
    }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt");
        close(listener);
        return 1;
    }
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        close(listener);
        return 1;
    }
    if (listen(listener, static_cast<int>(max_clients)) < 0) {
        std::perror("listen");
        close(listener);
        return 1;
    }

    std::cout << "Poll echo server listening on 127.0.0.1:" << port << std::endl;
    std::unordered_map<int, Connection> connections;
    std::vector<pollfd> watched; // consists of fd, events, and revents
    // POLLIN -> accept() on listening socket and recv() on client socket
    // POLLOUT -> can accept some outgoing bytes, send() can still write only part of the buffer
    // POLLHUP -> hang up
    // POLLERR/POLLNVAL -> socket error or invalid descriptor
    int status = 0;
    for (;;) {
        // A snapshot keeps traversal valid when the owning map changes.
        watched.clear();
        watched.push_back({listener, POLLIN, 0});
        for (const auto& entry : connections) {
            watched.push_back({entry.first, interests(entry.second), 0});
        }
        if (poll(watched.data(), watched.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            status = 1;
            break;
        }
        for (std::size_t index = 1; index < watched.size(); ++index) {
            const auto& event = watched[index];
            if (event.revents == 0) {
                continue;
            }
            if (!service_client(event.fd, event.revents, connections.at(event.fd))) {
                close(event.fd);
                connections.erase(event.fd);
            }
        }
        // Accept after processing the snapshot: a reused fd cannot see stale events.
        if (watched[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "poll: listener failure\n";
            status = 1;
            break;
        }
        if ((watched[0].revents & POLLIN) && !accept_clients(listener, connections)) {
            status = 1;
            break;
        }
    }
    for (const auto& entry : connections) {
        close(entry.first);
    }
    close(listener);
    return status;
}
