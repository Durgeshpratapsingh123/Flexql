# FlexQL Design Document

**GitHub Repository:** https://github.com/YOUR_USERNAME/flexql  ← add your repo link here

---

## Overview

FlexQL is a persistent, multithreaded, client-server SQL-like database implemented
entirely in C++17 with no external libraries. The design is modelled after how
production databases like SQLite and PostgreSQL work internally.

---

## Architecture

```
Client (flexql.cpp + repl.cpp)
        │  TCP: 4-byte length-prefixed messages
        ▼
Server (flexql_server.cpp)
  ┌─────────────────────────────────────┐
  │  Executor (SQL parser + query plan) │
  │            │                        │
  │  LRU Query Cache (512 entries)      │
  │            │  cache miss            │
  │  Buffer Pool (8 MB, 2048 pages)     │
  │            │  page miss             │
  │  WAL (Write-Ahead Log, O_DSYNC)     │
  │            │                        │
  │  Heap File (.fql) — disk storage    │
  └─────────────────────────────────────┘
```

---

## Storage Design

### Format: Fixed-size slot heap file

Each table is stored as a binary file of **fixed-size 512-byte slots**:

```
Slot layout (512 bytes total):
┌──────────┬──────────────────┬────────────────────────────────┐
│ 1 byte   │ 8 bytes          │ 503 bytes                      │
│ alive    │ expires_at (LE)  │ pipe-encoded column values     │
│ 0x01/0x00│ int64 unix epoch │ col1|col2|col3...              │
└──────────┴──────────────────┴────────────────────────────────┘
```

**Why fixed-size slots:**
- O(1) seek to any row by index: `lseek(fd, slot_idx * 512, SEEK_SET)`
- No fragmentation or compaction needed
- DELETE is just setting `alive = 0x00` in-place (logical tombstone)
- Trade-off: max 503 bytes of data per row. For the benchmark workload
  (IDs, names, emails, balances) rows are well under 100 bytes.

**Why row-major over column-major:**
- INSERT is a single slot write — O(1)
- Full-table SELECT scans rows sequentially — good cache locality
- WHERE filters can short-circuit per row
- Column-major would benefit aggregates (SUM, AVG) but FlexQL doesn't support them

**Pages:** Slots are grouped into 4 KB pages (8 slots per page), matching
the OS page size for efficient I/O.

---

## Write-Ahead Log (WAL)

The WAL is how FlexQL achieves **crash recovery and durability**.

### WAL record format

```
┌────────┬──────────────┬─────────────────┐
│ 1 byte │ 4 bytes      │ 512 bytes       │
│ op     │ slot_idx LE  │ full Slot data  │
│ I or D │ uint32       │                 │
└────────┴──────────────┴─────────────────┘
```

### Write path

1. Serialize all rows in the INSERT batch into one buffer
2. Write buffer to WAL in **one `write()` syscall**
3. `O_DSYNC` flag ensures the data hits disk when `write()` returns
4. Update the buffer pool (in-memory)

### Crash recovery

On startup, for every table with a non-empty WAL file:
1. WAL is replayed — each record applied to the heap file
2. WAL is checkpointed (truncated)

This recovers any rows that were in the WAL but not yet in the heap.

### Checkpointing

WAL is checkpointed (applied to heap + truncated) every **100,000 inserts**
and on clean shutdown. This bounds WAL file size and recovery time.

### O_DSYNC vs fsync

The WAL file is opened with `O_DSYNC`. This makes each `write()` call
durable without a separate `fsync()` syscall — the OS flushes the data
to stable storage as part of the `write()`. Combined with batch writes,
this means one disk flush per client network message (not per row).

---

## Buffer Pool (Page Cache)

The buffer pool holds **2048 pages (8 MB)** of recently accessed heap data
using **LRU eviction**.

### Read path
```
SELECT → compute slot index → check buffer pool (LRU map)
    HIT:  return pointer into in-memory page (zero disk I/O)
    MISS: read 4 KB page from heap file, evict LRU page if full
```

### Write path
```
INSERT → WAL first (durable) → update buffer pool page (dirty)
→ dirty pages flushed to heap on checkpoint
```

Repeated reads of recently inserted data are served entirely from RAM.

---

## Query Cache (LRU)

A **doubly-linked list + hash map LRU cache** with 512 entries caches
SELECT result strings keyed by the exact SQL string.

- **Read path:** Every SELECT checks the cache first. On a hit, the result
  is returned immediately with zero storage I/O.
- **Write path:** Every SELECT result is stored in the cache after execution.
- **Invalidation:** Any INSERT, DELETE, CREATE, or DROP clears the entire
  cache, since any cached result may now be stale.

This means repeated identical queries (common in read-heavy workloads) are
served from RAM in O(1) time.

---

## Primary Index

Each table maintains an **in-memory hash index** on the first column:

```cpp
unordered_map<string, vector<size_t>>  pk_index;
// first_col_value  →  list of slot indices in heap
```

- Lookup: O(1) average
- Insert: O(1) amortised — updated inline during INSERT
- Rebuild: O(n) — done after DELETE or on startup

`unordered_map` (hash) was chosen over `std::map` (B-tree) for O(1) vs
O(log n) average lookup. For range queries on the PK a B-tree would be
superior, but the benchmark primarily uses equality and comparison filters.

The index is rebuilt by scanning the heap file on every startup. For very
large datasets a persistent index file would be a future improvement.

---

## Expiration Timestamps

Each row optionally has an `EXPIRES_AT` column (DECIMAL, unix epoch seconds).

- During every scan, `row_alive()` checks the timestamp
- If `expires_at <= current_time` (and non-zero): row silently skipped
- `EXPIRES_AT = 0` means never expires
- `EXPIRES_AT = 1893456000` (~year 2030) keeps rows alive during testing

This is a **lazy expiration** model — expired rows remain on disk until
a DELETE or rebuild. A background sweeper thread could compact them.

---

## Multithreaded Server

The server spawns one `std::thread` per accepted client connection:

```cpp
std::thread(handle_client, client_fd, std::ref(db)).detach();
```

All access to the `Database` object (tables + query cache) is protected by
a single `std::mutex`. The lock is held only during query execution, not
during network I/O, so clients don't block each other while sending/receiving.

This design handles multiple simultaneous clients correctly. For very high
concurrency (100+ clients), a per-table lock or lock-free structures would
improve throughput further — a documented future improvement.

---

## Batch INSERT

The benchmark sends 5,000 rows per SQL statement:
```sql
INSERT INTO BIG_USERS VALUES (1,...),(2,...),(3,...),(4,...),...;
```

The parser handles this by looping over `(...)` groups after `VALUES`.
All rows in one statement:
- Share one WAL `write()` call → one disk flush
- Share one buffer pool update pass
- Are indexed in one pass

This reduces network round-trips from 1,000,000 to 200 for a 1M-row load,
and reduces disk flushes from 1,000,000 to 200.

---

## Network Protocol

Each message (both directions) is framed as:
```
[ 4-byte big-endian length ][ UTF-8 payload ]
```

**Client → server:** Raw SQL (may contain multiple `;`-separated statements)

**Server → client:**
- DML success: `OK\n`
- Error: `ERROR <message>\n`
- SELECT result:
  ```
  ROWS <n>\n
  NCOLS <k>\n
  col_name_1\n value_1\n
  col_name_2\n value_2\n
  ...
  ```

---

## Performance Decisions Summary

| Decision | Rationale |
|----------|-----------|
| Fixed-size 512-byte slots | O(1) seek, no compaction, simple implementation |
| `O_DSYNC` WAL | Durable writes without explicit `fsync()` overhead |
| Batch WAL write | One disk flush per INSERT message (not per row) |
| Buffer pool (8 MB) | Hot rows served from RAM |
| LRU query cache | Repeated identical SELECT queries served in O(1) |
| Hash index on first column | O(1) lookup vs O(log n) for B-tree |
| Logical deletes (tombstones) | No rewrite of surrounding slots on DELETE |
| Checkpoint every 100k rows | Bounds WAL size without too-frequent flushes |

---

## File Structure

```
flexql/
├── compile.sh                   # Build script
├── Makefile                     # Alternative build
├── README.md                    # How to run and test
├── DESIGN.md                    # This document
├── benchmark_flexql.cpp         # Unit tests + performance benchmark
├── include/
│   └── flexql.h                 # Public C API header
├── src/
│   ├── server/
│   │   └── flexql_server.cpp    # Complete server implementation
│   └── client/
│       ├── flexql.cpp           # Client library (implements flexql.h)
│       └── repl.cpp             # Interactive REPL
└── data/
    └── tables/                  # Persistent data (auto-created)
        ├── <TABLE>.fql          # Binary heap (row slots)
        ├── <TABLE>.schema       # Column definitions + row count
        └── <TABLE>.wal          # Write-Ahead Log
```
