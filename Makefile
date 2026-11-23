CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -O0

.PHONY: all clean test test-poll test-epoll

all: blocking_server poll_server epoll_server

blocking_server: blocking_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

poll_server: poll_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

epoll_server: epoll_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test_poll_state: test_poll_state.cpp poll_server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: blocking_server test-poll test-epoll
	python3 test_blocking.py

test-poll: poll_server test_poll_state
	./test_poll_state
	python3 test_poll.py

test-epoll: epoll_server
	python3 test_poll.py ./epoll_server

clean:
	$(RM) blocking_server poll_server epoll_server test_poll_state
