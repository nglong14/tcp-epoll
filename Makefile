CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -O0

.PHONY: all clean

all: blocking_server

blocking_server: blocking_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) blocking_server poll_server epoll_server
