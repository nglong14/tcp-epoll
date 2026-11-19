CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -O0

.PHONY: all clean test

all: blocking_server

blocking_server: blocking_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: blocking_server
	python3 test_blocking.py

clean:
	$(RM) blocking_server poll_server epoll_server
