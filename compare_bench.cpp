#include <iostream>
#include <chrono>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <numeric>
#include <algorithm>
#include "include/flexql.h"

using namespace std;
using namespace std::chrono;

static bool exec_sql(FlexQL *db, const string &sql) {
    char *err = nullptr;
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != FLEXQL_OK) {
        cout << "  [ERROR] " << sql.substr(0, 60) << " -> " << (err ? err : "?") << "\n";
        if (err) flexql_free(err);
        return false;
    }
    return true;
}

static int noop_cb(void*, int, char**, char**) { return 0; }

static double timed_exec(FlexQL *db, const string &sql) {
    auto t0 = high_resolution_clock::now();
    char *err = nullptr;
    flexql_exec(db, sql.c_str(), noop_cb, nullptr, &err);
    if (err) flexql_free(err);
    auto t1 = high_resolution_clock::now();
    return duration<double, std::milli>(t1 - t0).count();
}

static double median(vector<double> v) {
    sort(v.begin(), v.end());
    return v[v.size()/2];
}

// ---------------------------------------------------------------
// 1) Query cache: cold (first run) vs warm (cached) SELECT latency
// ---------------------------------------------------------------
void bench_query_cache(FlexQL *db) {
    cout << "\n=== 1. Query Cache: cold vs cached SELECT ===\n";
    exec_sql(db, "DROP TABLE IF EXISTS QC_TEST;");
    exec_sql(db, "CREATE TABLE QC_TEST(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);");
    stringstream ss;
    ss << "INSERT INTO QC_TEST VALUES ";
    for (int i = 1; i <= 20000; i++) {
        ss << "(" << i << ", 'user" << i << "', " << (1000 + i % 500) << ", 1893456000)";
        if (i < 20000) ss << ",";
    }
    ss << ";";
    exec_sql(db, ss.str());

    string q = "SELECT NAME, BALANCE FROM QC_TEST WHERE BALANCE > 1250 ORDER BY BALANCE DESC;";

    double cold = timed_exec(db, q); // first run: populates cache

    vector<double> warm_runs;
    for (int i = 0; i < 20; i++) warm_runs.push_back(timed_exec(db, q));
    double warm = median(warm_runs);

    cout << "  Cold (uncached) SELECT:  " << cold << " ms\n";
    cout << "  Warm (cached) SELECT:    " << warm << " ms (median of 20 runs)\n";
    if (warm > 0)
        cout << "  Speedup: " << (cold / warm) << "x\n";
}

// ---------------------------------------------------------------
// 2) JOIN with hash index available vs a linear-scan equivalent
//    FlexQL only indexes the first column, so we compare:
//    (a) equality lookup on indexed first column
//    (b) equality lookup on a non-indexed column (forces full scan)
// ---------------------------------------------------------------
void bench_index_lookup(FlexQL *db, int n_rows) {
    cout << "\n=== 2. Indexed vs Non-indexed Equality Lookup (" << n_rows << " rows) ===\n";
    exec_sql(db, "DROP TABLE IF EXISTS IDX_TEST;");
    exec_sql(db, "CREATE TABLE IDX_TEST(ID DECIMAL, TAG VARCHAR(64), VAL DECIMAL, EXPIRES_AT DECIMAL);");

    const int BATCH = 5000;
    for (int start = 1; start <= n_rows; start += BATCH) {
        stringstream ss;
        ss << "INSERT INTO IDX_TEST VALUES ";
        int end = min(start + BATCH - 1, n_rows);
        for (int i = start; i <= end; i++) {
            ss << "(" << i << ", 'tag" << i << "', " << (i % 1000) << ", 1893456000)";
            if (i < end) ss << ",";
        }
        ss << ";";
        exec_sql(db, ss.str());
    }

    int target_id = n_rows / 2;
    string indexed_q = "SELECT * FROM IDX_TEST WHERE ID = " + to_string(target_id) + ";";
    string unindexed_q = "SELECT * FROM IDX_TEST WHERE VAL = 500;"; // non-indexed column, many matches

    vector<double> idx_runs, noidx_runs;
    for (int i = 0; i < 10; i++) idx_runs.push_back(timed_exec(db, indexed_q));
    for (int i = 0; i < 10; i++) noidx_runs.push_back(timed_exec(db, unindexed_q));

    double idx_med = median(idx_runs);
    double noidx_med = median(noidx_runs);

    cout << "  Indexed lookup (ID=X, first column):     " << idx_med << " ms\n";
    cout << "  Non-indexed lookup (VAL=X, full scan):    " << noidx_med << " ms\n";
    if (idx_med > 0)
        cout << "  Speedup: " << (noidx_med / idx_med) << "x\n";
}

// ---------------------------------------------------------------
// 3) Buffer pool: cold read (first touch) vs warm read (in RAM)
// ---------------------------------------------------------------
void bench_buffer_pool(FlexQL *db) {
    cout << "\n=== 3. Buffer Pool: cold vs warm page read ===\n";
    exec_sql(db, "DROP TABLE IF EXISTS BP_TEST;");
    exec_sql(db, "CREATE TABLE BP_TEST(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);");
    stringstream ss;
    ss << "INSERT INTO BP_TEST VALUES ";
    for (int i = 1; i <= 5000; i++) {
        ss << "(" << i << ", 'user" << i << "', " << i << ", 1893456000)";
        if (i < 5000) ss << ",";
    }
    ss << ";";
    exec_sql(db, ss.str());

    // Use different WHERE values each time to defeat the *query* cache,
    // so we isolate buffer-pool (page-level) caching instead.
    vector<double> first_touch, repeat_touch;
    for (int i = 1; i <= 30; i++) {
        string q = "SELECT NAME FROM BP_TEST WHERE ID = " + to_string(i) + ";";
        first_touch.push_back(timed_exec(db, q));   // first time this row/page is read
        repeat_touch.push_back(timed_exec(db, q));  // second time -> page now in buffer pool
    }
    double cold = median(first_touch);
    double warm = median(repeat_touch);
    cout << "  First touch (page may be cold):  " << cold << " ms (median)\n";
    cout << "  Repeat touch (page cached):       " << warm << " ms (median)\n";
    if (warm > 0)
        cout << "  Speedup: " << (cold / warm) << "x\n";
    cout << "  Note: query-cache dominates repeat identical SELECTs (see test 1);\n";
    cout << "  this test varies the predicate to isolate page-level caching.\n";
}

// ---------------------------------------------------------------
// 4) Concurrency scaling: throughput vs number of simultaneous clients
// ---------------------------------------------------------------
void bench_concurrency(int n_clients, int queries_per_client, double &out_qps) {
    FlexQL *setup_db = nullptr;
    flexql_open("127.0.0.1", 9000, &setup_db);
    exec_sql(setup_db, "DROP TABLE IF EXISTS CONC_TEST;");
    exec_sql(setup_db, "CREATE TABLE CONC_TEST(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);");
    stringstream ss;
    ss << "INSERT INTO CONC_TEST VALUES ";
    for (int i = 1; i <= 2000; i++) {
        ss << "(" << i << ", 'user" << i << "', " << i << ", 1893456000)";
        if (i < 2000) ss << ",";
    }
    ss << ";";
    exec_sql(setup_db, ss.str());
    flexql_close(setup_db);

    atomic<long long> total_queries{0};
    vector<thread> threads;
    auto t0 = high_resolution_clock::now();

    for (int c = 0; c < n_clients; c++) {
        threads.emplace_back([&, c]() {
            FlexQL *db = nullptr;
            if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) return;
            for (int q = 0; q < queries_per_client; q++) {
                int id = 1 + ((c * queries_per_client + q) % 2000);
                string sql = "SELECT NAME FROM CONC_TEST WHERE ID = " + to_string(id) + ";";
                char *err = nullptr;
                flexql_exec(db, sql.c_str(), noop_cb, nullptr, &err);
                if (err) flexql_free(err);
                total_queries++;
            }
            flexql_close(db);
        });
    }
    for (auto &t : threads) t.join();
    auto t1 = high_resolution_clock::now();
    double elapsed_s = duration<double>(t1 - t0).count();
    out_qps = total_queries.load() / elapsed_s;
    cout << "  " << n_clients << " clients x " << queries_per_client << " queries: "
         << total_queries.load() << " total in " << elapsed_s << "s -> "
         << out_qps << " qps\n";
}

int main(int argc, char **argv) {
    string mode = argc > 1 ? argv[1] : "all";

    if (mode == "cache" || mode == "all") {
        FlexQL *db = nullptr;
        if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) { cout << "Cannot connect\n"; return 1; }
        bench_query_cache(db);
        flexql_close(db);
    }
    if (mode == "index" || mode == "all") {
        FlexQL *db = nullptr;
        if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) { cout << "Cannot connect\n"; return 1; }
        bench_index_lookup(db, 50000);
        flexql_close(db);
    }
    if (mode == "buffer" || mode == "all") {
        FlexQL *db = nullptr;
        if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) { cout << "Cannot connect\n"; return 1; }
        bench_buffer_pool(db);
        flexql_close(db);
    }
    if (mode == "concurrency" || mode == "all") {
        cout << "\n=== 4. Concurrency Scaling: QPS vs simultaneous clients ===\n";
        vector<int> client_counts = {1, 4, 16, 32};
        vector<double> qps_results;
        for (int n : client_counts) {
            double qps = 0;
            bench_concurrency(n, 100, qps);
            qps_results.push_back(qps);
        }
        cout << "\nSummary:\n";
        for (size_t i = 0; i < client_counts.size(); i++) {
            cout << "  " << client_counts[i] << " client(s): " << qps_results[i] << " qps\n";
        }
    }
    return 0;
}
