.PHONY: all debug test clean

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
DEBUG_CXXFLAGS = -std=c++17 -Wall -Wextra -g -O1 -fsanitize=address,undefined

all: server

server: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o server

server_asan: main.cpp
	$(CXX) $(DEBUG_CXXFLAGS) main.cpp -o server_asan

debug: server_asan

test: server_asan
	python3 tests/test_server.py ./server_asan

clean:
	rm -f server server_asan
