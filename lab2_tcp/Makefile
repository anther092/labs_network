CXX=clang++
CXXFLAGS=-std=c++17 -Wall -Wextra -O2

all: tcp_server tcp_client

tcp_server: tcp_server.cpp
	$(CXX) $(CXXFLAGS) tcp_server.cpp -o tcp_server

tcp_client: tcp_client.cpp
	$(CXX) $(CXXFLAGS) tcp_client.cpp -o tcp_client

clean:
	rm -f tcp_server tcp_client