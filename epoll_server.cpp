#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
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

std::uint32_t interests(const Connection& connection) {
    std::uint32_t events = 0;
    if (!connection.read_eof && !connection.read_paused) {
        events |= EPOLLIN | EPOLLRDHUP;
    }
    if (connection.unsent() != 0) {
        events |= EPOLLOUT;
    }
    return events;
}

bool update_registration(int epoll_fd, int operation, int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd, operation, fd, &event) == 0) {
        return true;
    }
    std::perror(operation == EPOLL_CTL_ADD ? "epoll_ctl ADD" : "epoll_ctl MOD");
    return false;
}

void close_client(int epoll_fd, int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        std::perror("epoll_ctl DEL");
    }
    close(fd);
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

bool service_client(int fd, std::uint32_t events, Connection& connection) {
    if (events & EPOLLERR) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            std::perror("getsockopt(SO_ERROR)");
        } else {
            std::cerr << "epoll: connection error on fd " << fd << ": "
                      << std::strerror(error) << '\n';
        }
        return false;
    }
    // HUP can accompany unread bytes. Only recv == 0 establishes read EOF.
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) && !connection.read_eof
        && !connection.read_paused && !receive_pending(fd, connection)) {
        return false;
    }
    // Try sending immediately after reading. Watch EPOLLOUT if bytes remain.
    if (!send_pending(fd, connection)) {
        return false;
    }
    return !(connection.read_eof && connection.unsent() == 0);
}

bool accept_clients(int listener, int epoll_fd,
                    std::unordered_map<int, Connection>& connections) {
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
        const auto entry = connections.try_emplace(client).first;
        if (!update_registration(epoll_fd, EPOLL_CTL_ADD, client, interests(entry->second))) {
            close(client);
            connections.erase(entry);
            return false;
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    int port = 9090;
    int port_index = 1;
    if (argc > 1 && std::string_view(argv[1]) == "lt") {
        ++port_index;
    }
    if (argc > port_index + 1) {
        std::cerr << "Usage: " << argv[0] << " [lt] [port]\n";
        return 1;
    }
    if (argc > port_index) {
        const std::string_view argument(argv[port_index]);
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

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::perror("epoll_create1");
        close(listener);
        return 1;
    }
    if (!update_registration(epoll_fd, EPOLL_CTL_ADD, listener, EPOLLIN)) {
        close(epoll_fd);
        close(listener);
        return 1;
    }

    std::cout << "Epoll echo server listening on 127.0.0.1:" << port << std::endl;
    std::unordered_map<int, Connection> connections;
    std::array<epoll_event, 32> ready;
    int status = 0;
    for (;;) {
        const int count = epoll_wait(epoll_fd, ready.data(), static_cast<int>(ready.size()), -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("epoll_wait");
            status = 1;
            break;
        }
        std::uint32_t listener_events = 0;
        for (int index = 0; index < count; ++index) {
            const auto& event = ready[index];
            const int fd = event.data.fd;
            if (fd == listener) {
                listener_events = event.events;
                continue;
            }
            Connection& connection = connections.at(fd);
            const auto previous = interests(connection);
            if (service_client(fd, event.events, connection)) {
                const auto next = interests(connection);
                if (next == previous || update_registration(epoll_fd, EPOLL_CTL_MOD, fd, next)) {
                    continue;
                }
            }
            close_client(epoll_fd, fd);
            connections.erase(fd);
        }
        // Process the full batch before accepting sockets that could reuse a closed fd.
        if (listener_events & (EPOLLERR | EPOLLHUP)) {
            std::cerr << "epoll: listener failure\n";
            status = 1;
            break;
        }
        if ((listener_events & EPOLLIN) && !accept_clients(listener, epoll_fd, connections)) {
            status = 1;
            break;
        }
    }
    for (const auto& entry : connections) {
        close_client(epoll_fd, entry.first);
    }
    close(epoll_fd);
    close(listener);
    return status;
}
