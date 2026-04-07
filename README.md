# FlexQL

**A persistent, multithreaded, client-server SQL database engine — written in pure C++17, zero external libraries.**

FlexQL implements production database internals from scratch: Write-Ahead Log, buffer pool page cache, LRU query cache, hash index, batch INSERT, and a TCP wire protocol — all in a single self-contained codebase.

> **Benchmark:** 1,000,000 rows inserted in ~20 seconds (~49,942 rows/sec). 22/22 unit tests pass.

---

## Table of Contents

- [Features](#features)
- [Architecture Overview](#architecture-overview)
- [Building](#building)
- [Quick Start](#quick-start)
- [SQL Reference](#sql-reference)
- [C/C++ Client API](#cc-client-api)
- [Performance & Benchmarks](#performance--benchmarks)
- [Data Files & Persistence](#data-files--persistence)
- [Troubleshooting](#troubleshooting)

---

## Features

| Feature | Details |
|---|---|
| **Persistent storage** | Fixed-size 512-byte slot heap files (`.fql`) |
| **Crash recovery** | Write-Ahead Log (WAL) with `O_DSYNC` per-write durability |
| **Buffer pool** | 8 MB LRU page cache — 2048 x 4 KB pages, zero disk I/O on cache hits |
| **Query cache** | 512-entry LRU cache keyed by exact SQL string |
| **Primary index** | Hash index (`unordered_map`) on first column — O(1) average lookup |
| **Concurrency** | One thread per client; mutex-guarded database object |
| **Batch INSERT** | Thousands of rows per message, single WAL flush |
| **Expiring rows** | Per-row `EXPIRES_AT` Unix timestamp with lazy deletion |
| **Interactive REPL** | Full terminal client with `flexql>` prompt |
| **C/C++ API** | SQLite-compatible `flexql_open` / `flexql_exec` / `flexql_close` |

---

## Architecture Overview

```
Client (flexql.cpp + repl.cpp)
        |  TCP: 4-byte length-prefixed messages
        v
Server (flexql_server.cpp)
  +------------------------------------------+
  |  SQL Parser + Query Planner              |
  |              |                           |
  |  LRU Query Cache (512 entries)           |
  |              |  cache miss               |
  |  Buffer Pool (8 MB, 2048 x 4 KB pages)  |
  |              |  page miss                |
  |  WAL (Write-Ahead Log, O_DSYNC)         |
  |              |                           |
  |  Heap File (.fql) -- disk storage        |
  +------------------------------------------+
```

---

## Building

**Requirements:** g++ with C++17 support (GCC >= 7), Linux or macOS.

```bash
chmod +x compile.sh
./compile.sh
```

This produces three binaries:

| Binary | Purpose |
|---|---|
| `./server` | FlexQL database server |
| `./flexql-client` | Interactive REPL terminal |
| `./benchmark` | Performance benchmark + unit tests |

**Alternative:** `make all`

---

## Quick Start

### 1 — Start the server

```bash
./server
```

```
FlexQL Server running on port 9000
Data directory : data/tables
Multithreaded  : yes (one thread per client)
```

Use a custom port with: `./server 8080`

---

### 2 — Run unit tests

```bash
./benchmark --unit-test
```

Expected output:
```
Connected to FlexQL

[[ Running Unit Tests ]]

[PASS] CREATE TABLE TEST_USERS (41 ms)
[PASS] RESET TEST_USERS (81 ms)
[PASS] INSERT TEST_USERS (86 ms)
[PASS] Single-row value validation
[PASS] Filtered rows validation
[PASS] ORDER BY descending validation
[PASS] Empty result-set validation
[PASS] CREATE TABLE TEST_ORDERS (81 ms)
[PASS] RESET TEST_ORDERS (82 ms)
[PASS] INSERT TEST_ORDERS (87 ms)
[PASS] Join result validation
[PASS] Single-condition equality WHERE validation
[PASS] Join with no matches validation
[PASS] Invalid SQL should fail
[PASS] Missing table should fail
...
Unit Test Summary: 22/22 passed, 0 failed.
```

---

### 3 — Run the performance benchmark

```bash
# 200,000 rows (quick test)
./benchmark 200000

# Full 1,000,000-row load
./benchmark
```

The benchmark drops and recreates `BIG_USERS` on each run — safe to run multiple times without restarting the server.

---

### 4 — Interactive REPL

```bash
./flexql-client 127.0.0.1 9000
```

```
Connected to FlexQL server
flexql> CREATE TABLE DEMO (ID DECIMAL, NAME TEXT, EXPIRES_AT DECIMAL);
flexql> INSERT INTO DEMO VALUES (1, 'Alice', 1893456000);
flexql> SELECT * FROM DEMO;

ID = 1
NAME = Alice
EXPIRES_AT = 1893456000

flexql> .exit
Connection closed
```

Type `.exit` or `.quit` to disconnect.

---

### 5 — Verify persistence across restarts

```bash
# Insert some data
./flexql-client 127.0.0.1 9000
# flexql> CREATE TABLE T (ID DECIMAL, NAME TEXT, EXPIRES_AT DECIMAL);
# flexql> INSERT INTO T VALUES (1, 'Alice', 1893456000);
# flexql> .exit

# Kill and restart the server
pkill -f ./server
./server &

# Data survives restart
./flexql-client 127.0.0.1 9000
# flexql> SELECT * FROM T;
# ID = 1
# NAME = Alice
```

---

## SQL Reference

### CREATE TABLE

```sql
CREATE TABLE STUDENT (
    ID          DECIMAL,
    FIRST_NAME  TEXT,
    LAST_NAME   VARCHAR(64),
    BALANCE     DECIMAL,
    EXPIRES_AT  DECIMAL
);

-- Safe re-creation:
CREATE TABLE IF NOT EXISTS STUDENT (...);
```

**Supported types:** `DECIMAL` (integers and decimals; also accepts `INT`), `TEXT`, `VARCHAR(n)`

> The `EXPIRES_AT` column is a Unix timestamp. Rows with `EXPIRES_AT <= current_time` are silently hidden from all queries. Use `1893456000` (~year 2030) for data that should always be visible, or `0` for never-expires.

---

### INSERT

```sql
-- Single row
INSERT INTO STUDENT VALUES (1, 'Alice', 'Smith', 1200.00, 1893456000);

-- Batch (strongly recommended for large loads)
INSERT INTO STUDENT VALUES
    (1, 'Alice', 'Smith',  1200.00, 1893456000),
    (2, 'Bob',   'Jones',   450.00, 1893456000),
    (3, 'Carol', 'White',  2200.00, 1893456000);
```

Batch inserts share a single WAL write call and a single disk flush — critical for throughput at scale.

---

### SELECT

```sql
-- All columns
SELECT * FROM STUDENT;

-- Specific columns
SELECT FIRST_NAME, BALANCE FROM STUDENT;

-- WHERE (operators: =  >  <  >=  <=)
SELECT * FROM STUDENT WHERE ID = 1;
SELECT * FROM STUDENT WHERE BALANCE > 1000;
SELECT * FROM STUDENT WHERE FIRST_NAME = 'Alice';

-- ORDER BY
SELECT * FROM STUDENT ORDER BY BALANCE DESC;
SELECT * FROM STUDENT ORDER BY FIRST_NAME ASC;

-- Combined
SELECT FIRST_NAME, BALANCE FROM STUDENT
WHERE BALANCE > 500
ORDER BY BALANCE DESC;
```

---

### INNER JOIN

```sql
CREATE TABLE ORDERS (
    ORDER_ID   DECIMAL,
    STUDENT_ID DECIMAL,
    AMOUNT     DECIMAL,
    EXPIRES_AT DECIMAL
);

INSERT INTO ORDERS VALUES
    (101, 1, 50,  1893456000),
    (102, 1, 150, 1893456000),
    (103, 2, 200, 1893456000);

-- Basic join
SELECT STUDENT.FIRST_NAME, ORDERS.AMOUNT
FROM STUDENT
INNER JOIN ORDERS ON STUDENT.ID = ORDERS.STUDENT_ID;

-- Join with WHERE and ORDER BY
SELECT STUDENT.FIRST_NAME, ORDERS.AMOUNT
FROM STUDENT
INNER JOIN ORDERS ON STUDENT.ID = ORDERS.STUDENT_ID
WHERE ORDERS.AMOUNT >= 150
ORDER BY ORDERS.AMOUNT DESC;
```

---

### DELETE

```sql
-- Delete all rows
DELETE FROM STUDENT;

-- Conditional delete
DELETE FROM STUDENT WHERE ID = 99;
DELETE FROM STUDENT WHERE BALANCE < 100;
```

DELETE is implemented as a logical tombstone (`alive = 0x00`) — no disk rewrite of surrounding slots.

---

### DROP TABLE

```sql
DROP TABLE STUDENT;
DROP TABLE IF EXISTS STUDENT;   -- no error if table doesn't exist
```

---

## C/C++ Client API

The public API mirrors SQLite's interface for easy integration.

```c
#include "include/flexql.h"

// Connect
FlexQL *db = NULL;
int rc = flexql_open("127.0.0.1", 9000, &db);
if (rc != FLEXQL_OK) { printf("Connection failed\n"); return 1; }

// Execute SQL -- callback invoked for each result row
char *errmsg = NULL;
rc = flexql_exec(db, "SELECT * FROM STUDENT;", my_callback, NULL, &errmsg);
if (rc != FLEXQL_OK) {
    printf("Error: %s\n", errmsg);
    flexql_free(errmsg);
}

// Close
flexql_close(db);
```

**Callback signature:**

```c
int my_callback(void *data, int col_count, char **values, char **col_names) {
    for (int i = 0; i < col_count; i++)
        printf("%s = %s\n", col_names[i], values[i] ? values[i] : "NULL");
    printf("\n");
    return 0;  // return 1 to stop iteration early
}
```

**Error codes:** `FLEXQL_OK = 0`, `FLEXQL_ERROR = 1`

---

## Performance & Benchmarks

Measured on a standard Linux desktop (Intel Core, local disk):

| Metric | Result |
|---|---|
| Rows inserted | 1,000,000 |
| Elapsed time | ~20,023 ms |
| **Throughput** | **~49,942 rows/sec** |
| Batch size | 5,000 rows per INSERT statement |
| WAL flushes | 200 (one per batch message) |
| Unit tests | **22/22 passed** |

**Key performance techniques:**

- **Batch INSERT:** 5,000 rows per SQL statement = 200 network round-trips instead of 1,000,000
- **Single WAL flush per batch:** one `O_DSYNC` disk write per message, not per row
- **Buffer pool:** hot reads served entirely from RAM — zero disk I/O on cache hits
- **LRU query cache:** repeated identical SELECTs returned in O(1) with no storage access
- **Hash index:** O(1) average lookup on primary column vs O(log n) for a B-tree

---

## Data Files & Persistence

All data is stored in `data/tables/` (auto-created on first run):

```
data/tables/
├── STUDENT.fql       <- binary heap file (fixed 512-byte row slots)
├── STUDENT.schema    <- column definitions + row count
└── STUDENT.wal       <- Write-Ahead Log (crash recovery)
```

Files persist across server restarts. On startup, any uncommitted WAL entries are automatically replayed before the server accepts connections.

**Wipe all data:**

```bash
rm -rf data/tables
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Cannot connect` | Ensure `./server` is running first |
| `Address already in use` | `pkill -f ./server` then restart |
| `Column count mismatch` | Every INSERT must supply a value for every column, including `EXPIRES_AT` |
| Fresh database | `rm -rf data/tables && mkdir -p data/tables` then restart server |
| Unit tests fail after 1M benchmark | Expected state — restart server before `--unit-test` |
| Multi-line SQL in REPL | FlexQL executes on seeing `;` — ensure `;` terminates the last line |
