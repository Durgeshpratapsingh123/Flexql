#!/usr/bin/env bash
# FlexQL build script
set -e

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Iinclude"
LDFLAGS="-lpthread"

echo "=== Building FlexQL ==="

echo "[1/3] Compiling server..."
$CXX $CXXFLAGS -o server src/server/flexql_server.cpp $LDFLAGS
echo "      -> ./server"

echo "[2/3] Compiling client library + REPL..."
$CXX $CXXFLAGS -o flexql-client src/client/flexql.cpp src/client/repl.cpp $LDFLAGS
echo "      -> ./flexql-client"

echo "[3/3] Compiling benchmark..."
$CXX $CXXFLAGS -o benchmark benchmark_flexql.cpp src/client/flexql.cpp $LDFLAGS
echo "      -> ./benchmark"

echo ""
echo "=== Build complete ==="
echo ""
echo "Run order:"
echo "  Terminal 1:  ./server"
echo "  Terminal 2:  ./benchmark --unit-test      # 22 correctness tests"
echo "  Terminal 2:  ./benchmark 200000            # perf benchmark (200k rows)"
echo "  Terminal 2:  ./benchmark                  # full 1M row benchmark"
echo "  Terminal 2:  ./flexql-client 127.0.0.1 9000  # interactive REPL"
