CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  = -lpthread

.PHONY: all server client benchmark clean

all: server client benchmark

server:
	$(CXX) $(CXXFLAGS) -o server src/server/flexql_server.cpp $(LDFLAGS)

client:
	$(CXX) $(CXXFLAGS) -o flexql-client src/client/flexql.cpp src/client/repl.cpp $(LDFLAGS)

benchmark:
	$(CXX) $(CXXFLAGS) -o benchmark benchmark_flexql.cpp src/client/flexql.cpp $(LDFLAGS)

clean:
	rm -f server flexql-client benchmark
