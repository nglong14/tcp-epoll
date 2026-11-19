#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <string_view>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void echo_client(int client) {
    std::array<char, 4096> buffer;
    for (;;) {
        const ssize_t received = recv(client, buffer.data(), buffer.size(), 0);
        if (received == 0) {
            return; // The peer has finished sending; all earlier bytes were echoed.
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("recv");
            return;
        }

        // A successful send may consume only part of the received bytes.
        std::size_t sent = 0;
        const auto size = static_cast<std::size_t>(received);
        while (sent < size) {
            const ssize_t count = send(client, buffer.data() + sent,
                                       size - sent, MSG_NOSIGNAL);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("send");
                return;
            }
            if (count == 0) {
                std::cerr << "send: no progress\n";
                return;
            }
            sent += static_cast<std::size_t>(count);
        }
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
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt");
        close(listener);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET; // IPv4
    address.sin_port = htons(static_cast<unsigned short>(port)); // Port
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // local machine only
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        close(listener);
        return 1;
    }
    if (listen(listener, 128) < 0) {
        std::perror("listen");
        close(listener);
        return 1;
    }

    std::cout << "Blocking echo server listening on 127.0.0.1:" << port << std::endl;
    for (;;) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            std::perror("accept");
            close(listener);
            return 1;
        }
        echo_client(client);
        close(client);
    }
}
