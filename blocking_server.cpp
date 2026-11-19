#include <iostream>

int main() {
    std::cout << "TCP echo lab — Phase 1: setup and socket model\n"
                 "socket -> setsockopt(SO_REUSEADDR) -> bind -> listen\n"
                 "       -> accept -> recv/send -> close client\n"
                 "Repeat accept for the next client; close listener on exit.\n"
                 "Phase 2 will implement sockets at 127.0.0.1:9090.\n";
}
