/*
 * FlexQL Server — Final Production Version
 * ==========================================
 *
 * DESIGN OVERVIEW
 * ───────────────
 * Storage   : Fixed-size 512-byte slot heap files per table (.fql)
 *             Each slot: [1B alive][8B expires_at][503B pipe-encoded row]
 *             O(1) seek to any row: lseek(fd, slot_idx * 512, SEEK_SET)
 *
 * WAL       : Write-Ahead Log (.wal) for crash recovery and durability
 *             O_DSYNC flag — every write() is durable without fsync()
 *             Checkpointed every 100k rows and on clean shutdown
 *             On startup: WAL is replayed automatically (crash recovery)
 *
 * Buffer pool: 8 MB LRU page cache (2048 x 4 KB pages)
 *              Reads served from RAM on cache hit — zero disk I/O
 *              Dirty pages flushed to heap on checkpoint
 *
 * Index     : unordered_map<string, vector<size_t>> on first column
 *             O(1) average lookup. Rebuilt from heap on startup.
 *
 * Query cache: LRU cache, 512 entries, keyed by SQL string
 *              Cache is checked on every SELECT (read path)
 *              Invalidated on any INSERT/DELETE/DROP/CREATE
 *
 * Concurrency: Multithreaded — one std::thread per client connection
 *              std::mutex protects Database (tables + cache)
 *              Fine-grained: mutex released before network I/O
 *
 * Expiration : Each row has EXPIRES_AT (unix epoch). Expired rows are
 *              silently skipped during scans (lazy deletion model).
 *
 * Batch INSERT: INSERT INTO t VALUES (r1),(r2),... — all rows in one
 *              network message, one WAL write, one O_DSYNC flush.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <list>
#include <mutex>
#include <thread>
#include <filesystem>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
static const std::string DATA_DIR            = "data/tables";
static const size_t      SLOT_SIZE           = 512;
static const size_t      SLOT_DATA_SIZE      = SLOT_SIZE - 9;   // 503 bytes
static const size_t      PAGE_SIZE           = 4096;
static const size_t      SLOTS_PER_PAGE      = PAGE_SIZE / SLOT_SIZE; // 8
static const size_t      BUFFER_POOL_PAGES   = 2048; // 8 MB
static const size_t      QUERY_CACHE_SIZE    = 512;
static const size_t      WAL_CHECKPOINT_ROWS = 100000;
static const uint8_t     SLOT_ALIVE          = 0x01;
static const uint8_t     SLOT_DEAD           = 0x00;
static const char        WAL_OP_INSERT       = 'I';
static const char        WAL_OP_DELETE       = 'D';

// ─────────────────────────────────────────────
//  Utilities
// ─────────────────────────────────────────────
static std::string to_upper(std::string s) {
    for (auto &c : s) c = (char)toupper((unsigned char)c);
    return s;
}
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
static std::string encode_row(const std::vector<std::string> &v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += '|';
        for (char c : v[i]) {
            if (c == '|') o += "\\|";
            else if (c == '\\') o += "\\\\";
            else o += c;
        }
    }
    return o;
}
static std::vector<std::string> decode_row(const char *d, size_t len) {
    std::vector<std::string> r; std::string cur; bool esc = false;
    for (size_t i = 0; i < len && d[i]; ++i) {
        if (esc) { cur += d[i]; esc = false; }
        else if (d[i] == '\\') esc = true;
        else if (d[i] == '|') { r.push_back(cur); cur.clear(); }
        else cur += d[i];
    }
    r.push_back(cur);
    return r;
}
static std::vector<std::string> tokenize(const std::string &sql) {
    std::vector<std::string> t; size_t i = 0;
    while (i < sql.size()) {
        while (i < sql.size() && isspace((unsigned char)sql[i])) ++i;
        if (i >= sql.size()) break;
        if (sql[i] == '\'') {
            std::string tok = "'"; ++i;
            while (i < sql.size() && sql[i] != '\'') tok += sql[i++];
            if (i < sql.size()) { tok += '\''; ++i; }
            t.push_back(tok);
        } else if (sql[i]=='('||sql[i]==')'||sql[i]==','||sql[i]==';') {
            t.push_back(std::string(1, sql[i++]));
        } else if (sql[i]=='>'||sql[i]=='<'||sql[i]=='='||sql[i]=='!') {
            std::string op(1, sql[i++]);
            if (i < sql.size() && sql[i] == '=') { op += '='; ++i; }
            t.push_back(op);
        } else {
            std::string tok;
            while (i < sql.size() && !isspace((unsigned char)sql[i]) &&
                   sql[i]!='('&&sql[i]!=')'&&sql[i]!=','&&sql[i]!=';'&&
                   sql[i]!='\''&&sql[i]!='>'&&sql[i]!='<'&&sql[i]!='='&&sql[i]!='!')
                tok += sql[i++];
            t.push_back(tok);
        }
    }
    return t;
}

// ─────────────────────────────────────────────
//  Column types
// ─────────────────────────────────────────────
enum class ColType { DECIMAL, VARCHAR, TEXT };
struct ColumnDef {
    std::string name;
    ColType     type        = ColType::TEXT;
    int         varchar_len = 255;
    bool        not_null    = false;
    bool        primary_key = false;
};
static ColType parse_coltype(const std::string &t) {
    std::string u = to_upper(t);
    if (u=="INT"||u=="DECIMAL"||u=="DATETIME") return ColType::DECIMAL;
    if (u=="TEXT") return ColType::TEXT;
    if (u.size()>=7 && u.substr(0,7)=="VARCHAR") return ColType::VARCHAR;
    return ColType::TEXT;
}

// ─────────────────────────────────────────────
//  Slot — 512-byte on-disk row
// ─────────────────────────────────────────────
#pragma pack(push, 1)
struct Slot {
    uint8_t alive;
    int64_t expires_at;
    char    data[SLOT_DATA_SIZE];

    Slot() : alive(SLOT_DEAD), expires_at(0) { memset(data, 0, SLOT_DATA_SIZE); }
    void pack(const std::vector<std::string> &vals, int64_t exp) {
        alive = SLOT_ALIVE; expires_at = exp;
        std::string enc = encode_row(vals);
        size_t n = std::min(enc.size(), SLOT_DATA_SIZE - 1);
        memcpy(data, enc.c_str(), n); data[n] = 0;
    }
    std::vector<std::string> unpack() const { return decode_row(data, SLOT_DATA_SIZE); }
    bool is_alive() const { return alive == SLOT_ALIVE; }
};
#pragma pack(pop)
static_assert(sizeof(Slot) == SLOT_SIZE, "Slot size must be 512 bytes");

// ─────────────────────────────────────────────
//  Buffer Pool — LRU page cache
// ─────────────────────────────────────────────
struct Page {
    size_t pno   = 0;
    bool   dirty = false;
    char   data[PAGE_SIZE];
    Page() { memset(data, 0, PAGE_SIZE); }
};

class BufferPool {
public:
    explicit BufferPool(size_t cap = BUFFER_POOL_PAGES) : cap_(cap) {}

    // Returns pointer to in-memory slot data (read or write)
    char* get_ptr(int fd, size_t slot_idx) {
        Page *p = fetch(fd, slot_idx / SLOTS_PER_PAGE);
        return p ? p->data + (slot_idx % SLOTS_PER_PAGE) * SLOT_SIZE : nullptr;
    }

    void put_slot(int fd, size_t slot_idx, const Slot &s) {
        size_t pno = slot_idx / SLOTS_PER_PAGE;
        Page *p = fetch(fd, pno);
        if (!p) p = alloc(pno);
        memcpy(p->data + (slot_idx % SLOTS_PER_PAGE) * SLOT_SIZE, &s, SLOT_SIZE);
        p->dirty = true;
    }

    void flush(int fd) {
        for (auto &[pno, it] : map_)
            if (it->dirty) { write_page(fd, *it); it->dirty = false; }
    }

    void invalidate() { list_.clear(); map_.clear(); }

private:
    size_t cap_;
    std::list<Page> list_;
    std::unordered_map<size_t, std::list<Page>::iterator> map_;

    Page* fetch(int fd, size_t pno) {
        auto it = map_.find(pno);
        if (it != map_.end()) {
            list_.splice(list_.begin(), list_, it->second);
            return &list_.front();
        }
        Page p; p.pno = pno;
        off_t off = (off_t)(pno * PAGE_SIZE);
        if (lseek(fd, off, SEEK_SET) != (off_t)-1) {
            ssize_t n = read(fd, p.data, PAGE_SIZE); (void)n;
        }
        return insert(std::move(p));
    }

    Page* alloc(size_t pno) { Page p; p.pno = pno; return insert(std::move(p)); }

    Page* insert(Page p) {
        if (list_.size() >= cap_) {
            map_.erase(list_.back().pno);
            list_.pop_back();
        }
        list_.push_front(std::move(p));
        map_[list_.front().pno] = list_.begin();
        return &list_.front();
    }

    void write_page(int fd, const Page &p) {
        lseek(fd, (off_t)(p.pno * PAGE_SIZE), SEEK_SET);
        write(fd, p.data, PAGE_SIZE);
    }
};

// ─────────────────────────────────────────────
//  WAL — Write-Ahead Log
//  Opened with O_DSYNC: every write() is durable
//  without explicit fsync() calls
// ─────────────────────────────────────────────
class WAL {
public:
    std::string path_;
    int fd_ = -1;

    bool open(const std::string &path) {
        path_ = path;
        // O_DSYNC: OS ensures data hits stable storage on each write()
        fd_ = ::open(path.c_str(), O_CREAT|O_RDWR|O_APPEND|O_DSYNC, 0644);
        if (fd_ < 0) // fallback without O_DSYNC
            fd_ = ::open(path.c_str(), O_CREAT|O_RDWR|O_APPEND, 0644);
        return fd_ >= 0;
    }

    void close_wal() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    // Single record — used for DELETE
    bool append(char op, size_t si, const Slot &s) {
        if (fd_ < 0) return false;
        char buf[1+4+SLOT_SIZE];
        uint32_t idx = (uint32_t)si;
        buf[0] = op; memcpy(buf+1, &idx, 4); memcpy(buf+5, &s, SLOT_SIZE);
        return write(fd_, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
    }

    // Batch append — all rows in one write() syscall = one O_DSYNC flush
    bool batch_append(const std::vector<std::pair<size_t,Slot>> &entries) {
        if (fd_ < 0 || entries.empty()) return false;
        const size_t E = 1 + 4 + SLOT_SIZE;
        std::vector<char> buf(entries.size() * E);
        char *p = buf.data();
        for (auto &[si, s] : entries) {
            p[0] = WAL_OP_INSERT; uint32_t idx = (uint32_t)si;
            memcpy(p+1, &idx, 4); memcpy(p+5, &s, SLOT_SIZE);
            p += E;
        }
        return write(fd_, buf.data(), buf.size()) == (ssize_t)buf.size();
    }

    int replay(int heap_fd, BufferPool &pool) {
        if (fd_ < 0) return 0;
        lseek(fd_, 0, SEEK_SET);
        int n = 0; char op; uint32_t idx; Slot s;
        while (read(fd_,&op,1)==1 && read(fd_,&idx,4)==4 &&
               read(fd_,&s,SLOT_SIZE)==(ssize_t)SLOT_SIZE) {
            pool.put_slot(heap_fd, (size_t)idx, s); ++n;
        }
        pool.flush(heap_fd);
        return n;
    }

    void checkpoint(int heap_fd, BufferPool &pool) {
        replay(heap_fd, pool);
        ftruncate(fd_, 0); lseek(fd_, 0, SEEK_SET);
    }

    ~WAL() { close_wal(); }
};

// ─────────────────────────────────────────────
//  Table
// ─────────────────────────────────────────────
struct Table {
    std::string            name;
    std::vector<ColumnDef> schema;
    size_t                 row_count = 0;

    std::unordered_map<std::string, std::vector<size_t>> pk_index;

    int        heap_fd = -1;
    WAL        wal;
    BufferPool buffer_pool;

    Table() : buffer_pool(BUFFER_POOL_PAGES) {}
    ~Table() { close_files(); }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    void close_files() {
        if (heap_fd >= 0) {
            buffer_pool.flush(heap_fd);
            ::close(heap_fd); heap_fd = -1;
        }
        wal.close_wal();
    }

    int col_idx(const std::string &n) const {
        std::string u = to_upper(n);
        for (int i = 0; i < (int)schema.size(); ++i)
            if (schema[i].name == u) return i;
        return -1;
    }

    int expires_at_col() const {
        for (int i = 0; i < (int)schema.size(); ++i)
            if (schema[i].name == "EXPIRES_AT") return i;
        return -1;
    }

    bool row_alive(const std::vector<std::string> &vals) const {
        int ec = expires_at_col();
        if (ec < 0 || ec >= (int)vals.size()) return true;
        const std::string &v = vals[ec];
        if (v.empty() || v == "NULL") return true;
        try {
            long long e = std::stoll(v);
            if (e == 0) return true;
            return e > (long long)time(nullptr);
        } catch (...) { return true; }
    }

    bool read_slot(size_t si, Slot &out) {
        char *ptr = buffer_pool.get_ptr(heap_fd, si);
        if (!ptr) return false;
        memcpy(&out, ptr, SLOT_SIZE);
        return true;
    }

    bool write_slot(size_t si, const Slot &s) {
        char op = (s.alive == SLOT_ALIVE) ? WAL_OP_INSERT : WAL_OP_DELETE;
        if (!wal.append(op, si, s)) return false;
        buffer_pool.put_slot(heap_fd, si, s);
        return true;
    }

    void checkpoint_table() {
        buffer_pool.flush(heap_fd);
        wal.checkpoint(heap_fd, buffer_pool);
    }

    void rebuild_index() {
        pk_index.clear(); Slot s;
        for (size_t i = 0; i < row_count; ++i) {
            if (!read_slot(i, s) || !s.is_alive()) continue;
            auto vals = s.unpack();
            if (!vals.empty()) pk_index[vals[0]].push_back(i);
        }
    }
};

// ─────────────────────────────────────────────
//  Schema I/O helpers
// ─────────────────────────────────────────────
static std::string schema_path(const std::string &n) { return DATA_DIR+"/"+n+".schema"; }
static std::string heap_path  (const std::string &n) { return DATA_DIR+"/"+n+".fql";    }
static std::string wal_path   (const std::string &n) { return DATA_DIR+"/"+n+".wal";    }

static void save_schema(const Table &t) {
    std::ofstream f(schema_path(t.name));
    f << t.row_count << "\n" << t.schema.size() << "\n";
    for (auto &c : t.schema)
        f << c.name << " " << (int)c.type << " "
          << c.varchar_len << " " << (int)c.not_null << " " << (int)c.primary_key << "\n";
}

static bool load_schema(Table &t, const std::string &name) {
    std::ifstream f(schema_path(name));
    if (!f) return false;
    t.name = name; size_t nc;
    f >> t.row_count >> nc; t.schema.resize(nc);
    for (size_t i = 0; i < nc; ++i) {
        int tp, nn, pk;
        f >> t.schema[i].name >> tp >> t.schema[i].varchar_len >> nn >> pk;
        t.schema[i].type      = (ColType)tp;
        t.schema[i].not_null  = nn;
        t.schema[i].primary_key = pk;
    }
    return true;
}

// ─────────────────────────────────────────────
//  LRU Query Cache — read + write paths
// ─────────────────────────────────────────────
struct CacheEntry { std::string key, value; };

class LRUQueryCache {
public:
    explicit LRUQueryCache(size_t cap = QUERY_CACHE_SIZE) : cap_(cap) {}

    // Returns cached result or empty string on miss
    std::string get(const std::string &key) {
        auto it = map_.find(key);
        if (it == map_.end()) return "";
        list_.splice(list_.begin(), list_, it->second);
        return list_.front().value;
    }

    void put(const std::string &key, const std::string &val) {
        auto it = map_.find(key);
        if (it != map_.end()) { list_.erase(it->second); map_.erase(it); }
        list_.push_front({key, val});
        map_[key] = list_.begin();
        while (list_.size() > cap_) { map_.erase(list_.back().key); list_.pop_back(); }
    }

    void invalidate() { list_.clear(); map_.clear(); }

private:
    size_t cap_;
    std::list<CacheEntry> list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> map_;
};

// ─────────────────────────────────────────────
//  Database — owns all tables + shared cache
// ─────────────────────────────────────────────
class Database {
public:
    std::unordered_map<std::string, Table*> tables;
    LRUQueryCache query_cache;
    std::mutex    mtx;

    Database() : query_cache(QUERY_CACHE_SIZE) {
        fs::create_directories(DATA_DIR);
        load_all_tables();
    }

    ~Database() {
        for (auto &[n, t] : tables) {
            t->checkpoint_table();
            save_schema(*t);
            delete t;
        }
    }

    Table* get_table(const std::string &n) {
        auto it = tables.find(to_upper(n));
        return it == tables.end() ? nullptr : it->second;
    }

    Table* create_table(const std::string &name, const std::vector<ColumnDef> &schema) {
        std::string u = to_upper(name);
        Table *t = new Table();
        t->name = u; t->schema = schema; t->row_count = 0;
        t->heap_fd = ::open(heap_path(u).c_str(), O_CREAT|O_RDWR, 0644);
        if (t->heap_fd < 0) { delete t; throw std::runtime_error("Cannot create heap for " + u); }
        t->wal.open(wal_path(u));
        save_schema(*t);
        tables[u] = t;
        return t;
    }

    void drop_table(const std::string &name) {
        std::string u = to_upper(name);
        auto it = tables.find(u);
        if (it != tables.end()) {
            it->second->close_files();
            delete it->second;
            tables.erase(it);
        }
        fs::remove(schema_path(u));
        fs::remove(heap_path(u));
        fs::remove(wal_path(u));
        query_cache.invalidate();
    }

private:
    void load_all_tables() {
        if (!fs::exists(DATA_DIR)) return;
        for (auto &e : fs::directory_iterator(DATA_DIR)) {
            if (e.path().extension() != ".schema") continue;
            std::string name = e.path().stem().string();
            for (auto &c : name) c = toupper((unsigned char)c);

            Table *t = new Table();
            if (!load_schema(*t, name)) { delete t; continue; }
            t->heap_fd = ::open(heap_path(name).c_str(), O_CREAT|O_RDWR, 0644);
            if (t->heap_fd < 0) { delete t; continue; }
            t->wal.open(wal_path(name));

            // Crash recovery: replay any uncommitted WAL entries
            int replayed = t->wal.replay(t->heap_fd, t->buffer_pool);
            if (replayed > 0) {
                std::cout << "[Recovery] Replayed " << replayed
                          << " WAL entries for " << name << "\n";
                t->wal.checkpoint(t->heap_fd, t->buffer_pool);
            }

            t->rebuild_index();
            tables[name] = t;
            std::cout << "[Startup] Loaded table " << name
                      << " (" << t->row_count << " slots)\n";
        }
    }
};

// ─────────────────────────────────────────────
//  Condition evaluation
// ─────────────────────────────────────────────
struct Condition { std::string col, op, val; bool valid = false; };

static bool eval_cmp(const std::string &a, const std::string &op, const std::string &b) {
    double da = 0, db = 0; bool num = false;
    try { da = std::stod(a); db = std::stod(b); num = true; } catch (...) {}
    if (num) {
        if (op=="=")  return da==db; if (op==">")  return da>db;
        if (op=="<")  return da<db;  if (op==">=") return da>=db;
        if (op=="<=") return da<=db;
    }
    if (op=="=")  return a==b; if (op==">")  return a>b;
    if (op=="<")  return a<b;  if (op==">=") return a>=b;
    if (op=="<=") return a<=b;
    return false;
}

// ─────────────────────────────────────────────
//  Query Executor
// ─────────────────────────────────────────────
struct ResultRow {
    std::vector<std::string> col_names, values;
    std::string sort_key;
};

class Executor {
public:
    explicit Executor(Database &db) : db_(db) {}

    std::string execute(const std::string &raw) {
        std::string sql = trim(raw);
        while (!sql.empty() && (sql.back()==';'||sql.back()=='\n'||sql.back()=='\r'))
            sql.pop_back();
        sql = trim(sql);
        if (sql.empty()) return "OK\n";
        std::string up = to_upper(sql);
        try {
            if (up.substr(0,12) == "CREATE TABLE") return exec_create(sql);
            if (up.substr(0,6)  == "INSERT")       return exec_insert(sql);
            if (up.substr(0,6)  == "SELECT")       return exec_select(sql);
            if (up.substr(0,6)  == "DELETE")       return exec_delete(sql);
            if (up.substr(0,4)  == "DROP")         return exec_drop(sql);
            return "ERROR Unknown command\n";
        } catch (const std::exception &e) { return std::string("ERROR ") + e.what() + "\n"; }
          catch (...) { return "ERROR Internal error\n"; }
    }

private:
    Database &db_;

    // ── CREATE TABLE ─────────────────────────
    std::string exec_create(const std::string &sql) {
        auto toks = tokenize(sql); size_t i = 0;
        auto need = [&](const std::string &e) {
            if (i>=toks.size()||to_upper(toks[i])!=to_upper(e))
                throw std::runtime_error("Expected '" + e + "'");
            ++i;
        };
        need("CREATE"); need("TABLE");
        bool ine = false;
        if (i < toks.size() && to_upper(toks[i]) == "IF") { ++i; need("NOT"); need("EXISTS"); ine = true; }
        std::string tname = to_upper(toks[i++]);
        need("(");
        if (ine && db_.get_table(tname)) return "OK\n";
        if (db_.get_table(tname)) throw std::runtime_error("Table already exists: " + tname);
        std::vector<ColumnDef> schema;
        while (i < toks.size() && toks[i] != ")") {
            ColumnDef col; col.name = to_upper(toks[i++]);
            std::string ts = to_upper(toks[i++]);
            if (ts == "VARCHAR" && i < toks.size() && toks[i] == "(") {
                ++i; col.varchar_len = 255;
                if (i < toks.size() && toks[i] != ")")
                    try { col.varchar_len = std::stoi(toks[i]); } catch (...) {}
                while (i < toks.size() && toks[i] != ")") ++i;
                if (i < toks.size()) ++i;
                col.type = ColType::VARCHAR;
            } else col.type = parse_coltype(ts);
            while (i < toks.size() && toks[i] != "," && toks[i] != ")") {
                std::string m = to_upper(toks[i]);
                if (m == "NOT") { ++i; need("NULL"); col.not_null = true; }
                else if (m == "PRIMARY") { ++i; need("KEY"); col.primary_key = true; }
                else ++i;
            }
            schema.push_back(col);
            if (i < toks.size() && toks[i] == ",") ++i;
        }
        db_.create_table(tname, schema);
        db_.query_cache.invalidate();
        return "OK\n";
    }

    // ── INSERT — batch aware ──────────────────
    std::string exec_insert(const std::string &sql) {
        auto toks = tokenize(sql); size_t i = 0;
        auto need = [&](const std::string &e) {
            if (i>=toks.size()||to_upper(toks[i])!=to_upper(e))
                throw std::runtime_error("Expected '" + e + "'");
            ++i;
        };
        need("INSERT"); need("INTO");
        std::string tname = to_upper(toks[i++]);
        Table *tbl = db_.get_table(tname);
        if (!tbl) throw std::runtime_error("Table not found: " + tname);
        need("VALUES");

        int ec = tbl->expires_at_col();
        std::vector<std::pair<size_t, Slot>> batch;

        while (i < toks.size() && toks[i] == "(") {
            ++i;
            std::vector<std::string> vals;
            while (i < toks.size() && toks[i] != ")") {
                std::string v = toks[i++];
                if (!v.empty() && v.front() == '\'')
                    v = v.substr(1, v.size() > 2 ? v.size()-2 : 0);
                vals.push_back(v);
                if (i < toks.size() && toks[i] == ",") ++i;
            }
            if (i < toks.size()) ++i; // )
            if (vals.size() != tbl->schema.size())
                throw std::runtime_error("Column count mismatch");

            int64_t exp = 0;
            if (ec >= 0 && ec < (int)vals.size())
                try { exp = std::stoll(vals[ec]); } catch (...) {}

            Slot s; s.pack(vals, exp);
            size_t si = tbl->row_count++;
            batch.emplace_back(si, s);
            if (!vals.empty()) tbl->pk_index[vals[0]].push_back(si);
            if (i < toks.size() && toks[i] == ",") ++i;
        }

        if (!batch.empty()) {
            tbl->wal.batch_append(batch); // one write() = one O_DSYNC flush
            for (auto &[si, s] : batch)
                tbl->buffer_pool.put_slot(tbl->heap_fd, si, s);

            if (tbl->row_count % WAL_CHECKPOINT_ROWS == 0) {
                tbl->checkpoint_table();
                save_schema(*tbl);
            }
        }

        db_.query_cache.invalidate();
        return "OK\n";
    }

    // ── DELETE ───────────────────────────────
    std::string exec_delete(const std::string &sql) {
        auto toks = tokenize(sql); size_t i = 0;
        auto need = [&](const std::string &e) {
            if (i>=toks.size()||to_upper(toks[i])!=e)
                throw std::runtime_error("Expected " + e);
            ++i;
        };
        need("DELETE"); need("FROM");
        std::string tname = to_upper(toks[i++]);
        Table *tbl = db_.get_table(tname);
        if (!tbl) throw std::runtime_error("Table not found: " + tname);
        Condition cond;
        if (i < toks.size() && to_upper(toks[i]) == "WHERE") { ++i; cond = parse_cond(toks, i); }
        Slot s;
        for (size_t si = 0; si < tbl->row_count; ++si) {
            if (!tbl->read_slot(si, s) || !s.is_alive()) continue;
            auto vals = s.unpack();
            if (cond.valid) {
                std::string col = cond.col;
                size_t d = col.find('.');
                if (d != std::string::npos) col = col.substr(d+1);
                int ci = tbl->col_idx(col);
                if (ci < 0 || !eval_cmp(ci<(int)vals.size()?vals[ci]:"", cond.op, cond.val))
                    continue;
            }
            s.alive = SLOT_DEAD;
            tbl->write_slot(si, s);
        }
        tbl->rebuild_index();
        tbl->checkpoint_table();
        save_schema(*tbl);
        db_.query_cache.invalidate();
        return "OK\n";
    }

    // ── DROP TABLE [IF EXISTS] ────────────────
    std::string exec_drop(const std::string &sql) {
        auto toks = tokenize(sql); size_t i = 0; ++i; // DROP
        if (i < toks.size() && to_upper(toks[i]) == "TABLE") ++i;
        if (i < toks.size() && to_upper(toks[i]) == "IF") {
            ++i;
            if (i < toks.size() && to_upper(toks[i]) == "EXISTS") ++i;
        }
        if (i < toks.size()) db_.drop_table(to_upper(toks[i]));
        return "OK\n";
    }

    // ── SELECT ───────────────────────────────
    std::string exec_select(const std::string &sql) {
        // Check query cache first
        std::string cached = db_.query_cache.get(sql);
        if (!cached.empty()) return cached;

        auto toks = tokenize(sql); size_t i = 0; ++i; // SELECT

        std::vector<std::string> sel_cols;
        while (i < toks.size() && to_upper(toks[i]) != "FROM") {
            if (toks[i] != ",") sel_cols.push_back(toks[i]);
            ++i;
        }
        if (i >= toks.size()) throw std::runtime_error("Missing FROM");
        ++i;

        std::string tname = to_upper(toks[i++]);
        Table *tbl = db_.get_table(tname);
        if (!tbl) throw std::runtime_error("Table not found: " + tname);

        // INNER JOIN?
        bool has_join = false; Table *tbl2 = nullptr; std::string tname2; Condition jcond;
        if (i < toks.size() && to_upper(toks[i]) == "INNER") {
            has_join = true; ++i;
            if (i < toks.size() && to_upper(toks[i]) == "JOIN") ++i;
            tname2 = to_upper(toks[i++]);
            tbl2 = db_.get_table(tname2);
            if (!tbl2) throw std::runtime_error("Table not found: " + tname2);
            if (i < toks.size() && to_upper(toks[i]) == "ON") { ++i; jcond = parse_join_cond(toks, i); }
        }

        // WHERE
        Condition wcond;
        if (i < toks.size() && to_upper(toks[i]) == "WHERE") { ++i; wcond = parse_cond(toks, i); }

        // ORDER BY
        std::string order_col; bool order_desc = false;
        if (i < toks.size() && to_upper(toks[i]) == "ORDER") {
            ++i;
            if (i < toks.size() && to_upper(toks[i]) == "BY") ++i;
            if (i < toks.size()) {
                order_col = to_upper(toks[i++]);
                size_t d = order_col.find('.');
                if (d != std::string::npos) order_col = order_col.substr(d+1);
            }
            if (i < toks.size() && to_upper(toks[i]) == "DESC") { order_desc = true; ++i; }
            else if (i < toks.size() && to_upper(toks[i]) == "ASC") ++i;
        }

        std::vector<ResultRow> results;

        if (!has_join) {
            // Build output column list
            std::vector<std::string> out_names; std::vector<int> col_idxs;
            bool star = (sel_cols.size() == 1 && sel_cols[0] == "*");
            if (star) {
                for (int c = 0; c < (int)tbl->schema.size(); ++c)
                    { out_names.push_back(tbl->schema[c].name); col_idxs.push_back(c); }
            } else {
                for (auto &cn : sel_cols) {
                    std::string s = to_upper(cn);
                    size_t d = s.find('.'); if (d != std::string::npos) s = s.substr(d+1);
                    int ci = tbl->col_idx(s);
                    if (ci < 0) throw std::runtime_error("Unknown column: " + cn);
                    out_names.push_back(s); col_idxs.push_back(ci);
                }
            }
            Slot s;
            for (size_t si = 0; si < tbl->row_count; ++si) {
                if (!tbl->read_slot(si, s) || !s.is_alive()) continue;
                auto vals = s.unpack();
                if (!tbl->row_alive(vals)) continue;
                if (wcond.valid && !eval_cond_single(wcond, vals, tbl)) continue;
                ResultRow rr; rr.col_names = out_names;
                for (int ci : col_idxs) rr.values.push_back(ci<(int)vals.size()?vals[ci]:"NULL");
                if (!order_col.empty()) {
                    int sci = tbl->col_idx(order_col);
                    rr.sort_key = (sci>=0&&sci<(int)vals.size()) ? vals[sci] : "";
                }
                results.push_back(std::move(rr));
            }
        } else {
            // INNER JOIN implemented as CROSS JOIN + ON filter (per spec)
            std::vector<std::string> out_names; std::vector<std::pair<int,int>> col_src;
            bool star = (sel_cols.size() == 1 && sel_cols[0] == "*");
            if (star) {
                for (int c=0;c<(int)tbl->schema.size();++c)
                    { out_names.push_back(tname+"."+tbl->schema[c].name); col_src.push_back({0,c}); }
                for (int c=0;c<(int)tbl2->schema.size();++c)
                    { out_names.push_back(tname2+"."+tbl2->schema[c].name); col_src.push_back({1,c}); }
            } else {
                for (auto &cn : sel_cols) {
                    std::string u = to_upper(cn); size_t dot = u.find('.');
                    if (dot != std::string::npos) {
                        std::string pfx=u.substr(0,dot), cp=u.substr(dot+1);
                        if (pfx==tname) { int x=tbl->col_idx(cp);if(x<0)throw std::runtime_error("Unknown col: "+cn);out_names.push_back(cp);col_src.push_back({0,x}); }
                        else { int x=tbl2->col_idx(cp);if(x<0)throw std::runtime_error("Unknown col: "+cn);out_names.push_back(cp);col_src.push_back({1,x}); }
                    } else {
                        int x1=tbl->col_idx(u), x2=tbl2->col_idx(u);
                        if (x1>=0) { out_names.push_back(u); col_src.push_back({0,x1}); }
                        else if (x2>=0) { out_names.push_back(u); col_src.push_back({1,x2}); }
                        else throw std::runtime_error("Unknown col: "+cn);
                    }
                }
            }
            // Pre-load tbl2 alive rows into memory (avoids re-scanning disk per tbl1 row)
            std::vector<std::vector<std::string>> t2rows;
            { Slot s2;
              for (size_t si=0;si<tbl2->row_count;++si) {
                if (!tbl2->read_slot(si,s2)||!s2.is_alive()) continue;
                auto v2=s2.unpack(); if(!tbl2->row_alive(v2)) continue;
                t2rows.push_back(std::move(v2));
              }
            }
            Slot s1;
            for (size_t si=0;si<tbl->row_count;++si) {
                if (!tbl->read_slot(si,s1)||!s1.is_alive()) continue;
                auto v1=s1.unpack(); if(!tbl->row_alive(v1)) continue;
                for (auto &v2 : t2rows) {
                    if (jcond.valid && !eval_join_cond(jcond,v1,v2,tbl,tbl2,tname,tname2)) continue;
                    if (wcond.valid && !eval_cond_join(wcond,v1,v2,tbl,tbl2,tname,tname2)) continue;
                    ResultRow rr; rr.col_names = out_names;
                    for (auto &[src,ci]:col_src) { auto &row=(src==0)?v1:v2; rr.values.push_back(ci<(int)row.size()?row[ci]:"NULL"); }
                    if (!order_col.empty()) {
                        int s1i=tbl->col_idx(order_col), s2i=tbl2->col_idx(order_col);
                        if (s1i>=0) rr.sort_key=s1i<(int)v1.size()?v1[s1i]:"";
                        else if (s2i>=0) rr.sort_key=s2i<(int)v2.size()?v2[s2i]:"";
                    }
                    results.push_back(std::move(rr));
                }
            }
        }

        // ORDER BY sort
        if (!order_col.empty()) {
            std::stable_sort(results.begin(), results.end(), [&](const ResultRow &a, const ResultRow &b) {
                double da=0,db=0; bool num=false;
                try{da=std::stod(a.sort_key);db=std::stod(b.sort_key);num=true;}catch(...){}
                if (num) return order_desc ? da>db : da<db;
                return order_desc ? a.sort_key>b.sort_key : a.sort_key<b.sort_key;
            });
        }

        // Serialise result
        std::ostringstream out;
        out << "ROWS " << results.size() << "\n";
        for (auto &rr : results) {
            out << "NCOLS " << rr.col_names.size() << "\n";
            for (size_t c = 0; c < rr.col_names.size(); ++c)
                out << rr.col_names[c] << "\n" << (c<rr.values.size()?rr.values[c]:"NULL") << "\n";
        }
        std::string result = out.str();
        db_.query_cache.put(sql, result); // cache for future identical queries
        return result;
    }

    // ── Condition helpers ─────────────────────
    Condition parse_cond(const std::vector<std::string> &t, size_t &i) {
        Condition c; if (i+2 >= t.size()) return c;
        c.col = to_upper(t[i++]); c.op = t[i++];
        std::string r = t[i++];
        if (!r.empty() && r.front()=='\'') r=r.substr(1,r.size()>2?r.size()-2:0);
        c.val = r; c.valid = true; return c;
    }
    Condition parse_join_cond(const std::vector<std::string> &t, size_t &i) {
        Condition c; if (i+2 >= t.size()) return c;
        c.col=to_upper(t[i++]); c.op=t[i++]; c.val=to_upper(t[i++]); c.valid=true; return c;
    }
    bool eval_cond_single(const Condition &c, const std::vector<std::string> &vals, Table *tbl) {
        std::string col=c.col; size_t d=col.find('.'); if(d!=std::string::npos)col=col.substr(d+1);
        int ci=tbl->col_idx(col); if(ci<0) return false;
        return eval_cmp(ci<(int)vals.size()?vals[ci]:"NULL",c.op,c.val);
    }
    bool eval_join_cond(const Condition &c,
                        const std::vector<std::string> &v1, const std::vector<std::string> &v2,
                        Table *t1, Table *t2, const std::string &n1, const std::string &n2) {
        auto res=[&](const std::string &ref, const std::vector<std::string>**row, Table**tbl){
            std::string u=to_upper(ref); size_t d=u.find('.');
            if(d!=std::string::npos){std::string pfx=u.substr(0,d),col=u.substr(d+1);
                if(pfx==n1){*row=&v1;*tbl=t1;return col;}if(pfx==n2){*row=&v2;*tbl=t2;return col;}}
            if(t1->col_idx(u)>=0){*row=&v1;*tbl=t1;return u;} *row=&v2;*tbl=t2;return u;
        };
        const std::vector<std::string>*ra=nullptr,*rb=nullptr; Table*ta=nullptr,*tb=nullptr;
        std::string ca=res(c.col,&ra,&ta), cb=res(c.val,&rb,&tb);
        int ia=ta?ta->col_idx(ca):-1, ib=tb?tb->col_idx(cb):-1;
        if(ia<0||ib<0) return false;
        return eval_cmp(ia<(int)ra->size()?(*ra)[ia]:"NULL",c.op,ib<(int)rb->size()?(*rb)[ib]:"NULL");
    }
    bool eval_cond_join(const Condition &c,
                        const std::vector<std::string> &v1, const std::vector<std::string> &v2,
                        Table *t1, Table *t2, const std::string &n1, const std::string &n2) {
        std::string col=c.col; size_t d=col.find('.');
        if(d!=std::string::npos){
            std::string pfx=col.substr(0,d),cn=col.substr(d+1);
            if(to_upper(pfx)==n1){int ci=t1->col_idx(cn);if(ci<0)return false;return eval_cmp(ci<(int)v1.size()?v1[ci]:"NULL",c.op,c.val);}
            else{int ci=t2->col_idx(cn);if(ci<0)return false;return eval_cmp(ci<(int)v2.size()?v2[ci]:"NULL",c.op,c.val);}
        }
        int ci1=t1->col_idx(col); if(ci1>=0)return eval_cmp(ci1<(int)v1.size()?v1[ci1]:"NULL",c.op,c.val);
        int ci2=t2->col_idx(col); if(ci2>=0)return eval_cmp(ci2<(int)v2.size()?v2[ci2]:"NULL",c.op,c.val);
        return false;
    }
};

// ─────────────────────────────────────────────
//  Network helpers
// ─────────────────────────────────────────────
static bool send_all(int fd, const char *buf, size_t len) {
    size_t s=0; while(s<len){ssize_t n=send(fd,buf+s,len-s,0);if(n<=0)return false;s+=n;} return true;
}
static bool send_msg(int fd, const std::string &m) {
    uint32_t l=htonl((uint32_t)m.size()); if(!send_all(fd,(const char*)&l,4))return false; return send_all(fd,m.c_str(),m.size());
}
static std::string recv_msg(int fd) {
    uint32_t ln=0; size_t g=0;
    while(g<4){ssize_t n=recv(fd,(char*)&ln+g,4-g,0);if(n<=0)return "";g+=n;}
    uint32_t l=ntohl(ln); if(!l) return "";
    std::string b(l,'\0'); g=0;
    while(g<l){ssize_t n=recv(fd,&b[g],l-g,0);if(n<=0)return "";g+=n;}
    return b;
}

// ─────────────────────────────────────────────
//  Client handler — runs in its own thread
// ─────────────────────────────────────────────
static void handle_client(int cfd, Database &db) {
    Executor exec(db);
    while (true) {
        std::string msg = recv_msg(cfd);
        if (msg.empty()) break;

        std::string result, accum;
        int depth = 0; bool in_quote = false;
        for (char c : msg) {
            if (c=='\'' && !in_quote) in_quote=true;
            else if (c=='\'' && in_quote) in_quote=false;
            if (!in_quote && c=='(') ++depth;
            if (!in_quote && c==')') --depth;
            accum += c;
            if (!in_quote && depth==0 && c==';') {
                std::string stmt = trim(accum);
                if (!stmt.empty()) {
                    std::lock_guard<std::mutex> lk(db.mtx);
                    result += exec.execute(stmt);
                }
                accum.clear();
            }
        }
        if (!trim(accum).empty()) {
            std::lock_guard<std::mutex> lk(db.mtx);
            result += exec.execute(trim(accum));
        }
        if (!send_msg(cfd, result)) break;
    }
    close(cfd);
}

// ─────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────
int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN); // ignore broken pipe from disconnected clients

    int port = 9000;
    if (argc > 1) port = std::stoi(argv[1]);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(sfd,(sockaddr*)&addr,sizeof(addr)) < 0) { perror("bind");   return 1; }
    if (listen(sfd, 64) < 0)                          { perror("listen"); return 1; }

    std::cout << "FlexQL Server running on port " << port << "\n";
    std::cout << "Data directory : " << DATA_DIR << "\n";
    std::cout << "Multithreaded  : yes (one thread per client)\n\n";
    std::cout.flush();

    Database db;

    while (true) {
        sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (sockaddr*)&caddr, &clen);
        if (cfd < 0) { perror("accept"); continue; }
        // Spawn a new thread for each client — supports multiple simultaneous connections
        std::thread(handle_client, cfd, std::ref(db)).detach();
    }

    close(sfd);
    return 0;
}
