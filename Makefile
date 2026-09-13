CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -pthread

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f server client

.PHONY: all clean
