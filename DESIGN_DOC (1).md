# FlexQL Design Document

**A Persistent, Multithreaded Client-Server SQL Engine**

*Written entirely in C++17 — no external libraries*

Version 1.0

# GIT REPOSITORY : https://github.com/Durgeshpratapsingh123/Flexql
---

## 1. Overview

FlexQL is a persistent, multithreaded, client-server SQL-like database implemented entirely in C++17 with no external libraries. The design is modelled after how production databases such as SQLite and PostgreSQL work internally, providing a practical implementation of core database internals: storage, write-ahead logging, buffer pool management, query caching, indexing, and concurrent client handling.

| Property | Value |
|---|---|
| **Language** | C++17 — no external libraries |
| **Storage model** | Fixed-size 512-byte slot heap files |
| **Durability** | Write-Ahead Log (WAL) with O_DSYNC |
| **Buffer pool** | 8 MB LRU page cache (2048 × 4 KB pages) |
| **Query cache** | 512-entry LRU keyed by SQL string |
| **Primary index** | Hash map (O(1) average lookup) |
| **Concurrency** | One thread per client, global mutex |
| **Benchmark result** | ~49,942 rows/sec — 1M rows in ~20 seconds |
| **Unit test result** | 22/22 tests pass |

---

## 2. System Architecture

FlexQL follows a classic client-server split. The server owns all state — storage, memory, indexes, and caches. Clients connect over TCP and communicate using a simple length-prefixed binary protocol. All server internals are single-process and protected by a global mutex.

### 2.1 Component Layers

From top to bottom, a query passes through the following layers:

1. **SQL Parser** — tokenises and classifies the SQL string, extracts table names, columns, values, and predicates
2. **Query Cache (LRU, 512 entries)** — identical SELECT strings are served immediately from RAM with no storage access
3. **Executor** — applies WHERE predicates, JOIN logic, ORDER BY sorting, and column projection
4. **Buffer Pool (8 MB, 2048 pages)** — caches 4 KB pages of heap data in memory using LRU eviction
5. **WAL (Write-Ahead Log)** — all mutations are written durably before the buffer pool is updated
6. **Heap File (.fql)** — final on-disk storage of fixed-size row slots

### 2.2 Network Protocol

Every message in both directions is framed as a 4-byte big-endian length prefix followed by a UTF-8 payload. This makes framing trivial and avoids line-based parsing edge cases.

```
Client -> Server: [ 4-byte length ][ raw SQL text ]
Server -> Client: [ 4-byte length ][ response payload ]

Response formats:
  DML success:    OK\n
  Error:          ERROR <message>\n
  SELECT result:  ROWS <n>\nNCOLS <k>\ncol_name\nvalue\n...
```

---

## 3. Storage Design

### 3.1 Heap File Format

Each table is stored as a binary file of fixed-size 512-byte slots on disk. The file is named `<TABLE>.fql` and lives in the `data/tables/` directory.

```
Slot layout (512 bytes total):
+----------+------------------+--------------------------------+
| 1 byte   | 8 bytes          | 503 bytes                      |
| alive    | expires_at (LE)  | pipe-encoded column values     |
| 0x01/0x00| int64 unix epoch | col1|col2|col3...              |
+----------+------------------+--------------------------------+
```

**Why fixed-size slots?**

- O(1) seek to any row by index: `lseek(fd, slot_idx * 512, SEEK_SET)`
- No fragmentation or compaction required
- DELETE is just setting `alive = 0x00` in-place (logical tombstone)
- Trade-off: max 503 bytes of encoded data per row; well within the benchmark workload's needs

**Why row-major layout?**

- INSERT is a single slot write — O(1)
- Full-table SELECT scans rows sequentially — good CPU cache locality
- WHERE filters can short-circuit per row immediately
- Column-major would benefit aggregates (SUM, AVG) but FlexQL does not support them

### 3.2 Page Grouping

Slots are grouped into 4 KB pages (8 slots per page), matching the OS page size for efficient I/O. The buffer pool works in units of pages rather than individual slots.

### 3.3 Column Encoding

Column values are serialised into the 503-byte data region using pipe-separation. Literal pipe characters and backslashes are escaped:

```
col1_value|col2_value|col3_value

Pipe in value:  \|
Backslash:      \\
```

---

## 4. Write-Ahead Log (WAL)

The WAL provides crash recovery and durability. All mutations are written to the WAL before being applied to the buffer pool or heap file. On startup, any un-checkpointed WAL entries are automatically replayed.

### 4.1 WAL Record Format

```
+--------+--------------+-----------------+
| 1 byte | 4 bytes      | 512 bytes       |
| op     | slot_idx LE  | full Slot data  |
| I or D | uint32       |                 |
+--------+--------------+-----------------+

op = 'I' for INSERT, 'D' for DELETE
```

### 4.2 Write Path

1. Serialise all rows in an INSERT batch into a single buffer
2. Write the buffer to the WAL in one `write()` syscall
3. `O_DSYNC` flag guarantees data hits stable storage when `write()` returns — no separate `fsync()` needed
4. Update the buffer pool in memory

This means one disk flush per client network message, not one per row — a major performance advantage for batch inserts.

### 4.3 Crash Recovery

On startup, for every table with a non-empty WAL file, the server replays each WAL record against the heap file, then checkpoints (truncates) the WAL. This ensures no committed data is ever lost.

### 4.4 Checkpointing

The WAL is checkpointed — applied to the heap file and then truncated — every 100,000 inserts and on clean shutdown. This bounds both WAL file size and the time required for crash recovery.

### 4.5 O_DSYNC vs fsync

The WAL file is opened with `O_DSYNC`. This makes each `write()` call individually durable without requiring a separate `fsync()` syscall. The OS flushes data to stable storage as part of the `write()` itself. Combined with batch writes, this achieves one disk flush per INSERT message regardless of how many rows it contains.

---

## 5. Buffer Pool (Page Cache)

The buffer pool holds 2,048 pages (8 MB total) of recently accessed heap data in memory using LRU eviction. It is the primary mechanism for avoiding repeated disk reads.

### 5.1 Read Path

```
SELECT query
  -> compute slot index from row count
  -> check buffer pool (LRU hash map)
     HIT:  return pointer into in-memory page (zero disk I/O)
     MISS: read 4 KB page from heap file
           evict LRU page if pool is full
           insert new page into pool
```

### 5.2 Write Path

```
INSERT
  -> write to WAL first (durable)
  -> update buffer pool page (marks page dirty)
  -> dirty pages flushed to heap file on checkpoint
```

Repeated reads of recently inserted data are served entirely from RAM. No disk I/O occurs until checkpoint time.

---

## 6. Query Cache

A doubly-linked list + hash map LRU cache with 512 entries caches SELECT result strings keyed by the exact SQL string.

### 6.1 Read Path

Every SELECT checks the query cache first. On a hit, the serialised result is returned immediately with zero storage I/O.

### 6.2 Write Path

Every SELECT result is stored in the cache after execution so that subsequent identical queries benefit.

### 6.3 Invalidation

Any INSERT, DELETE, CREATE TABLE, or DROP TABLE clears the entire query cache, since any cached result may be stale after a write. This is a conservative whole-cache invalidation — a future improvement could implement table-level invalidation instead.

This means repeated identical queries (common in read-heavy workloads) are served in O(1) time with no storage access whatsoever.

---

## 7. Primary Index

Each table maintains an in-memory hash index on its first column. This enables O(1) average-case equality lookups.

### 7.1 Structure

```cpp
unordered_map<string, vector<size_t>> pk_index;
// first_col_value -> list of slot indices in heap
```

### 7.2 Complexity

| Operation | Complexity | Notes |
|---|---|---|
| **Lookup** | O(1) average | Hash map lookup |
| **Insert** | O(1) amortised | Updated inline during INSERT |
| **Rebuild** | O(n) | Full heap scan on startup or after DELETE |

### 7.3 Design Decisions

`unordered_map` (hash) was chosen over `std::map` (B-tree) for O(1) vs O(log n) average lookup. For range queries on the primary key a B-tree would be superior, but the benchmark primarily uses equality and comparison filters where the hash map wins.

The index is rebuilt by scanning the heap file on every server startup. For very large datasets a persistent index file would reduce startup time — a documented future improvement.

---

## 8. Concurrency Model

The server spawns one `std::thread` per accepted client connection and detaches it. All access to the Database object (tables and query cache) is protected by a single `std::mutex`.

### 8.1 Lock Strategy

```cpp
// Per-client thread entry point
std::thread(handle_client, client_fd, std::ref(db)).detach();

// Inside handle_client:
std::lock_guard<std::mutex> lock(db.mutex);
// ... parse and execute query ...
// lock released here -- network I/O happens outside the lock
```

The lock is held only during query execution, not during network I/O. This means clients do not block each other while reading from or writing to the socket — only during the critical section of touching shared state.

### 8.2 Limitations and Future Work

The global mutex is the simplest correct implementation. For very high concurrency (100+ simultaneous clients), a per-table lock or lock-free structures would improve throughput. This is noted as a future improvement in the codebase.

---

## 9. Batch INSERT

The benchmark sends 5,000 rows per SQL statement. The parser handles this by iterating over `(...)` groups after the `VALUES` keyword. All rows in a single statement:

- Share one WAL `write()` call — one disk flush regardless of row count
- Share one buffer pool update pass
- Are indexed in a single pass

This reduces network round-trips from 1,000,000 to 200 for a 1M-row load, and reduces disk flushes from 1,000,000 to 200. This is the single biggest performance multiplier in the system.

### 9.1 Example

```sql
INSERT INTO BIG_USERS VALUES
  (1, 'Alice', 'alice@example.com', 1200.00, 1893456000),
  (2, 'Bob',   'bob@example.com',   450.00,  1893456000),
  ...
  (5000, 'Zach', 'zach@example.com', 900.00, 1893456000);

-- One network round-trip. One WAL write. One O_DSYNC flush.
```

---

## 10. Row Expiration

Each row optionally carries an `EXPIRES_AT` column (DECIMAL, Unix epoch seconds). During every heap scan, the `row_alive()` function checks the timestamp. If `expires_at <= current_time` and the value is non-zero, the row is silently skipped — it is logically invisible to all queries.

| EXPIRES_AT Value | Behaviour |
|---|---|
| **0** | Row never expires |
| **Unix timestamp** | Row hidden once `current_time >= expires_at` |
| **1893456000** | Year 2030 — effectively permanent for testing |

This is a lazy expiration model. Expired rows remain on disk until a DELETE or rebuild. A background sweeper thread could compact them in a future version.

---

## 11. Performance Summary

All benchmark figures are from a single standard Linux desktop (Intel Core, local disk), inserting 1,000,000 rows in batches of 5,000.

| Metric | Value |
|---|---|
| **Total rows inserted** | 1,000,000 |
| **Elapsed time** | ~20,023 ms |
| **Throughput** | ~49,942 rows/sec |
| **Batch size** | 5,000 rows per INSERT |
| **Network round-trips** | 200 (vs 1,000,000 for single-row) |
| **WAL flushes** | 200 (one per batch) |
| **Unit tests** | 22/22 passed |

### 11.1 Decision Rationale

| Decision | Rationale |
|---|---|
| **Fixed-size 512-byte slots** | O(1) seek, no compaction, simple implementation |
| **O_DSYNC WAL** | Durable writes without a separate `fsync()` call |
| **Batch WAL write** | One disk flush per INSERT message, not per row |
| **Buffer pool (8 MB)** | Hot rows served from RAM — zero disk I/O on hits |
| **LRU query cache** | Repeated identical SELECTs served in O(1) |
| **Hash index on first column** | O(1) lookup vs O(log n) for B-tree |
| **Logical deletes (tombstones)** | No rewrite of surrounding slots on DELETE |
| **Checkpoint every 100k rows** | Bounds WAL size without too-frequent flushes |

---

## 12. Comparative Benchmark Suite (`compare_bench.cpp`)

While `benchmark_flexql.cpp` measures raw insert throughput and correctness, `compare_bench.cpp` isolates the contribution of each individual subsystem — query cache, primary index, buffer pool, and concurrency model — by running paired "with vs without" comparisons against a live server on `127.0.0.1:9000`. It links against the public C API (`include/flexql.h`) exactly as an external client would.

### 12.1 Test 1 — Query Cache: Cold vs Cached SELECT

Loads 20,000 rows into `QC_TEST`, then times one cold (uncached) run of a filtered `SELECT ... ORDER BY BALANCE DESC` query, followed by the median of 20 warm (cached) runs of the identical SQL string.

**Observed result:**

| Run | Latency |
|---|---|
| Cold (uncached) SELECT | 49.74 ms |
| Warm (cached) SELECT, median of 20 | 82.02 ms |
| Speedup | 0.61x (warm was *slower*) |

This is the opposite of the expected outcome — a cache hit should be faster, not slower. The likely explanation is that `timed_exec()` measures wall-clock time around `flexql_exec()`, which includes the TCP round-trip to the server, not just server-side processing. If per-query network/socket overhead (~80 ms in this environment) dominates the actual cache-lookup cost, the cache benefit is being masked by client-server latency rather than disproven. The cold run's unusually low 49.74 ms (lower than every warm run) also suggests warm-up/OS-caching effects on the very first connection, rather than the query cache being harmful. This needs isolated server-side timing (e.g., timestamps taken inside `flexql_server.cpp` around the cache check) to confirm the cache is behaving as designed.

### 12.2 Test 2 — Indexed vs Non-Indexed Equality Lookup

Loads 50,000 rows into `IDX_TEST`, then compares the median of 10 runs of an equality filter on the indexed first column (`ID = X`) against the median of 10 runs of an equality filter on a non-indexed column (`VAL = 500`, which also matches many rows since `VAL` cycles `i % 1000`).

**Observed result:**

| Query | Latency (median of 10) |
|---|---|
| Indexed lookup (`ID = X`) | 82.01 ms |
| Non-indexed lookup (`VAL = 500`, full scan) | 82.08 ms |
| Speedup | 1.00x (no measurable difference) |

Both queries land within noise of each other, again consistent with network round-trip time dominating the measurement rather than server-side scan cost. At 50,000 rows a full linear scan should still be measurably slower than an O(1) hash lookup if timed server-side; the flat result here is evidence the client-side stopwatch is not sensitive enough to see it, not evidence the index has no effect.

### 12.3 Test 3 — Buffer Pool: Cold vs Warm Page Read

Loads 5,000 rows into `BP_TEST`, then for 30 distinct primary keys touches each key twice (first touch = possible cold page fault, repeat touch = page already resident in the 8 MB buffer pool), taking the median of each group. The predicate is varied per iteration specifically to defeat the *query* cache (Section 6) so the test isolates *page-level* caching (Section 5).

**Observed result:**

| Access | Latency (median) |
|---|---|
| First touch | 87.84 ms |
| Repeat touch (page cached) | 82.00 ms |
| Speedup | 1.07x |

A small, directionally-correct improvement, but far smaller than the 8 MB in-RAM buffer pool should produce for a table this size (all 5,000 rows fit in well under 1 MB, so every "cold" read after the first page fault should already be a pool hit). As with Tests 1–2, network round-trip latency appears to be the dominant term in every measurement, compressing what should be a much larger relative gap.

### 12.4 Test 4 — Concurrency Scaling

Pre-loads 2,000 rows into `CONC_TEST`, then spawns *N* client threads (1, 4, 16, 32), each opening its own connection and firing 100 sequential indexed `SELECT` queries, and measures aggregate queries-per-second across all clients.

**Observed result:**

| Clients | Total Queries | Elapsed | Throughput |
|---|---|---|---|
| 1 | 100 | 8.34 s | 12.00 qps |
| 4 | 400 | 8.34 s | 47.94 qps |
| 16 | 1,600 | 8.40 s | 190.39 qps |
| 32 | 3,200 | 8.42 s | 379.86 qps |

Throughput scales almost perfectly linearly with client count (~4x per 4x clients), while wall-clock time per run stays flat at ~8.3–8.4 s regardless of load. This is the one test where the result is unambiguous and matches the design: per-client threads with a global mutex held only during query execution (Section 8.1) let network I/O for different clients overlap freely, so aggregate throughput scales with concurrent connections even though each individual query still pays the same fixed per-round-trip cost seen in Tests 1–3.

### 12.5 Interpretation

Tests 1–3 all show the same pattern: the subsystem-level optimisation (query cache, index, buffer pool) is real in principle but invisible in `compare_bench.cpp`'s client-side wall-clock measurements, because each `flexql_exec()` call pays a roughly constant ~80 ms client-server round-trip that swamps the microsecond-to-low-millisecond differences the subsystems are meant to produce. Test 4 is unaffected by this because it measures aggregate throughput under concurrent load rather than single-query latency, so the per-round-trip constant cancels out. Recommended follow-up: add server-side instrumentation (timestamps around the cache/index/buffer-pool code paths in `flexql_server.cpp`) so Tests 1–3 measure the subsystems directly instead of through the network.

---

## 13. File Structure

```
flexql/
├── compile.sh                  # Build script (produces server, flexql-client, benchmark)
├── Makefile                    # Alternative: make all
├── README.md                   # How to run, SQL reference, C API docs
├── DESIGN.md                   # Architecture notes (source for this document)
├── benchmark_flexql.cpp        # Unit test suite + 1M-row performance benchmark
├── compare_bench.cpp           # Comparative micro-benchmark suite (cache/index/buffer/concurrency)
├── include/
│   └── flexql.h                # Public C API: flexql_open/exec/close/free
└── src/
    ├── server/
    │   └── flexql_server.cpp   # Complete server (1038 lines, single translation unit)
    └── client/
        ├── flexql.cpp          # Client library — implements flexql.h over TCP
        └── repl.cpp            # Interactive REPL terminal

Runtime data (auto-created):
data/tables/
├── <TABLE>.fql                 # Binary heap file (512-byte row slots)
├── <TABLE>.schema              # Column definitions + row count
└── <TABLE>.wal                 # Write-Ahead Log
```

---

## 14. Known Limitations & Future Improvements

| Limitation | Potential Improvement |
|---|---|
| **Global mutex** | Per-table locking or lock-free structures for high-concurrency workloads |
| **Whole-cache invalidation** | Table-level query cache invalidation to preserve cached results for unaffected tables |
| **In-memory index rebuild on startup** | Persist the hash index to a file to reduce startup time for large datasets |
| **No aggregate functions** | Add SUM, AVG, COUNT, MIN, MAX support |
| **Lazy expiration only** | Background sweeper thread to compact expired rows and reclaim disk space |
| **No transactions** | Multi-statement transactions with COMMIT/ROLLBACK |
| **503-byte row limit** | Support for larger rows via overflow pages or variable-length slots |
| **Single-column index** | Secondary indexes on arbitrary columns |
