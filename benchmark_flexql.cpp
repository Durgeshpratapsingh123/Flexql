// #include <iostream>
// #include <chrono>
// #include <string>
// #include <sstream>
// #include <vector>
// #include "include/flexql.h"

// using namespace std;
// using namespace std::chrono;

// static const long long DEFAULT_INSERT_ROWS = 1000000LL;
// static const int INSERT_BATCH_SIZE = 5000;

// struct QueryStats {
//     long long rows = 0;
// };

// static int count_rows_callback(void *data, int argc, char **argv, char **azColName) {
//     (void)argc; (void)argv; (void)azColName;
//     QueryStats *stats = static_cast<QueryStats*>(data);
//     if (stats) stats->rows++;
//     return 0;
// }

// static bool run_exec(FlexQL *db, const string &sql, const string &label) {
//     char *errMsg = nullptr;
//     auto start = high_resolution_clock::now();
//     int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
//     auto end = high_resolution_clock::now();
//     long long elapsed = duration_cast<milliseconds>(end - start).count();
//     if (rc != FLEXQL_OK) {
//         cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
//         if (errMsg) flexql_free(errMsg);
//         return false;
//     }
//     cout << "[PASS] " << label << " (" << elapsed << " ms)\n";
//     return true;
// }

// static bool run_query(FlexQL *db, const string &sql, const string &label) {
//     QueryStats stats;
//     char *errMsg = nullptr;
//     auto start = high_resolution_clock::now();
//     int rc = flexql_exec(db, sql.c_str(), count_rows_callback, &stats, &errMsg);
//     auto end = high_resolution_clock::now();
//     long long elapsed = duration_cast<milliseconds>(end - start).count();
//     if (rc != FLEXQL_OK) {
//         cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
//         if (errMsg) flexql_free(errMsg);
//         return false;
//     }
//     cout << "[PASS] " << label << " | rows=" << stats.rows << " | " << elapsed << " ms\n";
//     return true;
// }

// static bool query_rows(FlexQL *db, const string &sql, vector<string> &out_rows) {
//     struct Collector { vector<string> rows; } collector;
//     auto cb = [](void *data, int argc, char **argv, char **azColName) -> int {
//         (void)azColName;
//         Collector *c = static_cast<Collector*>(data);
//         string row;
//         for (int i = 0; i < argc; ++i) {
//             if (i > 0) row += "|";
//             row += (argv[i] ? argv[i] : "NULL");
//         }
//         c->rows.push_back(row);
//         return 0;
//     };
//     char *errMsg = nullptr;
//     int rc = flexql_exec(db, sql.c_str(), cb, &collector, &errMsg);
//     if (rc != FLEXQL_OK) {
//         cout << "[FAIL] " << sql << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
//         if (errMsg) flexql_free(errMsg);
//         return false;
//     }
//     out_rows = collector.rows;
//     return true;
// }

// static bool assert_rows_equal(const string &label, const vector<string> &actual, const vector<string> &expected) {
//     if (actual == expected) { cout << "[PASS] " << label << "\n"; return true; }
//     cout << "[FAIL] " << label << "\n";
//     cout << "Expected (" << expected.size() << "):\n";
//     for (const auto &r : expected) cout << "  " << r << "\n";
//     cout << "Actual (" << actual.size() << "):\n";
//     for (const auto &r : actual) cout << "  " << r << "\n";
//     return false;
// }

// static bool expect_query_failure(FlexQL *db, const string &sql, const string &label) {
//     char *errMsg = nullptr;
//     int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
//     if (rc == FLEXQL_OK) { cout << "[FAIL] " << label << " (expected failure, got success)\n"; return false; }
//     if (errMsg) flexql_free(errMsg);
//     cout << "[PASS] " << label << "\n";
//     return true;
// }

// static bool assert_row_count(const string &label, const vector<string> &rows, size_t expected_count) {
//     if (rows.size() == expected_count) { cout << "[PASS] " << label << "\n"; return true; }
//     cout << "[FAIL] " << label << " (expected " << expected_count << ", got " << rows.size() << ")\n";
//     return false;
// }

// static bool run_data_level_unit_tests(FlexQL *db) {
//     cout << "\n\n[[ Running Unit Tests ]]\n\n";
//     bool all_ok = true;
//     int total_tests = 0, failed_tests = 0;
//     auto record = [&](bool result) { total_tests++; if (!result) { all_ok = false; failed_tests++; } };

//     record(run_exec(db,
//         "CREATE TABLE IF NOT EXISTS TEST_USERS(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);",
//         "CREATE TABLE TEST_USERS"));
//     record(run_exec(db, "DELETE FROM TEST_USERS;", "RESET TEST_USERS"));
//     record(run_exec(db,
//         "INSERT INTO TEST_USERS VALUES "
//         "(1, 'Alice', 1200, 1893456000),"
//         "(2, 'Bob', 450, 1893456000),"
//         "(3, 'Carol', 2200, 1893456000),"
//         "(4, 'Dave', 800, 1893456000);",
//         "INSERT TEST_USERS"));

//     vector<string> rows;
//     bool q1 = query_rows(db, "SELECT NAME, BALANCE FROM TEST_USERS WHERE ID = 2;", rows);
//     record(q1);
//     if (q1) record(assert_rows_equal("Single-row value validation", rows, {"Bob|450"}));

//     bool q2 = query_rows(db, "SELECT NAME FROM TEST_USERS WHERE BALANCE > 1000 ORDER BY NAME;", rows);
//     record(q2);
//     if (q2) record(assert_rows_equal("Filtered rows validation", rows, {"Alice", "Carol"}));

//     bool q3 = query_rows(db, "SELECT NAME FROM TEST_USERS ORDER BY BALANCE DESC;", rows);
//     record(q3);
//     if (q3) record(assert_rows_equal("ORDER BY descending validation", rows, {"Carol", "Alice", "Dave", "Bob"}));

//     bool q4 = query_rows(db, "SELECT ID FROM TEST_USERS WHERE BALANCE > 5000;", rows);
//     record(q4);
//     if (q4) record(assert_row_count("Empty result-set validation", rows, 0));

//     record(run_exec(db,
//         "CREATE TABLE IF NOT EXISTS TEST_ORDERS(ORDER_ID DECIMAL, USER_ID DECIMAL, AMOUNT DECIMAL, EXPIRES_AT DECIMAL);",
//         "CREATE TABLE TEST_ORDERS"));
//     record(run_exec(db, "DELETE FROM TEST_ORDERS;", "RESET TEST_ORDERS"));
//     record(run_exec(db,
//         "INSERT INTO TEST_ORDERS VALUES "
//         "(101, 1, 50, 1893456000),"
//         "(102, 1, 150, 1893456000),"
//         "(103, 3, 500, 1893456000);",
//         "INSERT TEST_ORDERS"));

//     bool q5 = query_rows(db,
//         "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
//         "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID "
//         "WHERE TEST_ORDERS.AMOUNT >= 100 ORDER BY TEST_ORDERS.ORDER_ID;",
//         rows);
//     record(q5);
//     if (q5) record(assert_rows_equal("Join result validation", rows, {"Alice|150", "Carol|500"}));

//     bool q6 = query_rows(db, "SELECT ORDER_ID FROM TEST_ORDERS WHERE USER_ID = 1 ORDER BY ORDER_ID;", rows);
//     record(q6);
//     if (q6) record(assert_rows_equal("Single-condition equality WHERE validation", rows, {"101", "102"}));

//     bool q7 = query_rows(db,
//         "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
//         "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID "
//         "WHERE TEST_ORDERS.AMOUNT > 900;",
//         rows);
//     record(q7);
//     if (q7) record(assert_row_count("Join with no matches validation", rows, 0));

//     record(expect_query_failure(db, "SELECT UNKNOWN_COLUMN FROM TEST_USERS;", "Invalid SQL should fail"));
//     record(expect_query_failure(db, "SELECT * FROM MISSING_TABLE;", "Missing table should fail"));

//     int passed_tests = total_tests - failed_tests;
//     cout << "\nUnit Test Summary: " << passed_tests << "/" << total_tests << " passed, "
//          << failed_tests << " failed.\n\n";
//     return all_ok;
// }

// static bool run_insert_benchmark(FlexQL *db, long long target_rows) {
//     // Drop and recreate so repeated benchmark runs always start fresh
//     run_exec(db, "DROP TABLE BIG_USERS;", "DROP TABLE BIG_USERS (if exists)");
//     if (!run_exec(db,
//         "CREATE TABLE BIG_USERS(ID DECIMAL, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);",
//         "CREATE TABLE BIG_USERS")) return false;

//     cout << "\nStarting insertion benchmark for " << target_rows << " rows...\n";
//     auto bench_start = high_resolution_clock::now();
//     long long inserted = 0;
//     long long progress_step = target_rows / 10;
//     if (progress_step <= 0) progress_step = 1;
//     long long next_progress = progress_step;

//     while (inserted < target_rows) {
//         stringstream ss;
//         ss << "INSERT INTO BIG_USERS VALUES ";
//         int in_batch = 0;
//         while (in_batch < INSERT_BATCH_SIZE && inserted < target_rows) {
//             long long id = inserted + 1;
//             ss << "(" << id
//                << ", 'user" << id << "'"
//                << ", 'user" << id << "@mail.com'"
//                << ", " << (1000.0 + (id % 10000))
//                << ", 1893456000)";
//             inserted++; in_batch++;
//             if (in_batch < INSERT_BATCH_SIZE && inserted < target_rows) ss << ",";
//         }
//         ss << ";";
//         char *errMsg = nullptr;
//         if (flexql_exec(db, ss.str().c_str(), nullptr, nullptr, &errMsg) != FLEXQL_OK) {
//             cout << "[FAIL] INSERT BIG_USERS batch -> " << (errMsg ? errMsg : "unknown error") << "\n";
//             if (errMsg) flexql_free(errMsg);
//             return false;
//         }
//         if (inserted >= next_progress || inserted == target_rows) {
//             cout << "Progress: " << inserted << "/" << target_rows << "\n";
//             next_progress += progress_step;
//         }
//     }

//     auto bench_end = high_resolution_clock::now();
//     long long elapsed = duration_cast<milliseconds>(bench_end - bench_start).count();
//     long long throughput = (elapsed > 0) ? (target_rows * 1000LL / elapsed) : target_rows;
//     cout << "[PASS] INSERT benchmark complete\n";
//     cout << "Rows inserted: " << target_rows << "\n";
//     cout << "Elapsed: " << elapsed << " ms\n";
//     cout << "Throughput: " << throughput << " rows/sec\n";
//     return true;
// }

// int main(int argc, char **argv) {
//     FlexQL *db = nullptr;
//     long long insert_rows = DEFAULT_INSERT_ROWS;
//     bool run_unit_tests_only = false;

//     if (argc > 1) {
//         string arg1 = argv[1];
//         if (arg1 == "--unit-test") run_unit_tests_only = true;
//         else {
//             insert_rows = atoll(argv[1]);
//             if (insert_rows <= 0) { cout << "Invalid row count.\n"; return 1; }
//         }
//     }

//     if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) {
//         cout << "Cannot open FlexQL\n"; return 1;
//     }
//     cout << "Connected to FlexQL\n";

//     if (run_unit_tests_only) {
//         bool ok = run_data_level_unit_tests(db);
//         flexql_close(db);
//         return ok ? 0 : 1;
//     }

//     cout << "Running SQL subset checks plus insertion benchmark...\n";
//     cout << "Target insert rows: " << insert_rows << "\n\n";

//     if (!run_insert_benchmark(db, insert_rows)) { flexql_close(db); return 1; }
//     if (!run_data_level_unit_tests(db)) { flexql_close(db); return 1; }

//     flexql_close(db);
//     return 0;
// }

#include <iostream>
#include <chrono>
#include <string>
#include <sstream>
#include <vector>
#include "flexql.h"

using namespace std;
using namespace std::chrono;

static const long long DEFAULT_INSERT_ROWS = 10LL; // 100k rows by default for insertion benchmark
static const int INSERT_BATCH_SIZE = 1;            // if you implement batch inserts in flexql, you can increase this for better performance

struct QueryStats
{
    long long rows = 0;
};

static int count_rows_callback(void *data, int argc, char **argv, char **azColName)
{
    (void)argc;
    (void)argv;
    (void)azColName;
    QueryStats *stats = static_cast<QueryStats *>(data);
    if (stats)
    {
        stats->rows++;
    }
    return 0;
}

static bool run_exec(FlexQL *db, const string &sql, const string &label)
{
    char *errMsg = nullptr;
    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    auto end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(end - start).count();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    cout << "[PASS] " << label << " (" << elapsed << " ms)\n";
    return true;
}

static bool run_query(FlexQL *db, const string &sql, const string &label)
{
    QueryStats stats;
    char *errMsg = nullptr;
    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), count_rows_callback, &stats, &errMsg);
    auto end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(end - start).count();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    cout << "[PASS] " << label << " | rows=" << stats.rows << " | " << elapsed << " ms\n";
    return true;
}

static bool query_rows(FlexQL *db, const string &sql, vector<string> &out_rows)
{
    struct Collector
    {
        vector<string> rows;
    } collector;

    auto cb = [](void *data, int argc, char **argv, char **azColName) -> int
    {
        (void)azColName;
        Collector *c = static_cast<Collector *>(data);
        string row;
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
            {
                row += "|";
            }
            row += (argv[i] ? argv[i] : "NULL");
        }
        c->rows.push_back(row);
        return 0;
    };

    char *errMsg = nullptr;
    int rc = flexql_exec(db, sql.c_str(), cb, &collector, &errMsg);
    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << sql << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    out_rows = collector.rows;
    return true;
}

static bool assert_rows_equal(const string &label, const vector<string> &actual, const vector<string> &expected)
{
    if (actual == expected)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << "\n";
    cout << "Expected (" << expected.size() << "):\n";
    for (const auto &r : expected)
    {
        cout << "  " << r << "\n";
    }
    cout << "Actual (" << actual.size() << "):\n";
    for (const auto &r : actual)
    {
        cout << "  " << r << "\n";
    }
    return false;
}

static bool expect_query_failure(FlexQL *db, const string &sql, const string &label)
{
    char *errMsg = nullptr;
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc == FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " (expected failure, got success)\n";
        return false;
    }
    if (errMsg)
    {
        flexql_free(errMsg);
    }
    cout << "[PASS] " << label << "\n";
    return true;
}

static bool assert_row_count(const string &label, const vector<string> &rows, size_t expected_count)
{
    if (rows.size() == expected_count)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << " (expected " << expected_count << ", got " << rows.size() << ")\n";
    return false;
}

static bool run_data_level_unit_tests(FlexQL *db)
{
    cout << "\n[[...Running Unit Tests...]]\n\n";

    bool all_ok = true;
    int total_tests = 0;
    int failed_tests = 0;
    int total_weight = 0;
    int earned_weight = 0;

    auto record = [&](bool result, int weight)
    {
        total_tests++;
        total_weight += weight;
        if (!result)
        {
            all_ok = false;
            failed_tests++;
        }
        else
        {
            earned_weight += weight;
        }
    };

    record(run_exec(
               db,
               "CREATE TABLE TEST_USERS(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);",
               "CREATE TABLE TEST_USERS"),
           1);

    auto insert_test_user = [&](long long id, const string &name, long long balance, long long expires_at) -> bool
    {
        stringstream ss;
        ss << "INSERT INTO TEST_USERS VALUES ("
           << id << ", '" << name << "', " << balance << ", " << expires_at << ");";
        return run_exec(db, ss.str(), "INSERT TEST_USERS ID=" + to_string(id));
    };

    record(insert_test_user(1, "Eve", 300, 1893456000), 1);
    record(insert_test_user(2, "Frank", 1800, 1893456000), 1);
    record(insert_test_user(3, "Grace", 950, 1893456000), 1);
    record(insert_test_user(4, "Henry", 3500, 1893456000), 1);
    record(insert_test_user(5, "Iris", 600, 1893456000), 1);

    vector<string> rows;

    query_rows(db, "SELECT * FROM TEST_USERS;", rows);
    record(assert_rows_equal("Basic SELECT * validation", rows, {"1|Eve|300|1893456000", "2|Frank|1800|1893456000", "3|Grace|950|1893456000", "4|Henry|3500|1893456000", "5|Iris|600|1893456000"}), 1);

    query_rows(db, "SELECT NAME, BALANCE FROM TEST_USERS WHERE ID = 1;", rows);
    record(assert_rows_equal("Single-row value validation", rows, {"Eve|300"}), 1);

    query_rows(db, "SELECT NAME FROM TEST_USERS WHERE BALANCE > 1500;", rows);
    record(assert_rows_equal("Filtered rows validation", rows, {"Frank", "Henry"}), 1);

    query_rows(db, "SELECT ID FROM TEST_USERS WHERE BALANCE < 100;", rows);
    record(assert_row_count("Empty result-set validation", rows, 0), 1);

    query_rows(db, "SELECT NAME, BALANCE FROM TEST_USERS WHERE BALANCE >= 950;", rows);
    record(assert_rows_equal("High-balance users validation", rows, {"Frank|1800", "Grace|950", "Henry|3500"}), 2);

    query_rows(db, "SELECT NAME FROM TEST_USERS WHERE BALANCE < 700;", rows);
    record(assert_rows_equal("Low-balance users validation", rows, {"Eve", "Iris"}), 2);

    record(run_exec(
               db,
               "CREATE TABLE TEST_ORDERS(ORDER_ID DECIMAL, USER_ID DECIMAL, AMOUNT DECIMAL, EXPIRES_AT DECIMAL);",
               "CREATE TABLE TEST_ORDERS"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (201, 2, 75, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=201"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (202, 4, 300, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=202"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (203, 4, 1200, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=203"),
           1);

    query_rows(
        db,
        "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
        "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID "
        "WHERE TEST_ORDERS.AMOUNT >= 300;",
        rows);
    record(assert_rows_equal("Join with matching rows validation", rows, {"Henry|300", "Henry|1200"}), 4);

    query_rows(
        db,
        "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
        "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID;",
        rows);
    record(assert_rows_equal("Join all matched rows validation", rows, {"Frank|75", "Henry|300", "Henry|1200"}), 4);

    record(expect_query_failure(db, "SELECT GHOST_COL FROM TEST_USERS;", "Invalid column should fail"), 2);
    record(expect_query_failure(db, "SELECT * FROM NO_SUCH_TABLE;", "Missing table should fail"), 2);

    int passed_tests = total_tests - failed_tests;
    cout << "\nUnit Test Summary: " << passed_tests << "/" << total_tests << " passed, "
         << failed_tests << " failed.\n";
    cout << "Total Marks: " << earned_weight << "/" << total_weight << "\n\n";

    return all_ok;
}

static bool run_insert_benchmark(FlexQL *db, long long target_rows)
{
    if (!run_exec(
            db,
            "CREATE TABLE BIG_USERS(ID DECIMAL, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);",
            "CREATE TABLE BIG_USERS"))
    {
        return false;
    }

    cout << "\nStarting insertion benchmark for " << target_rows << " rows...\n";
    auto bench_start = high_resolution_clock::now();

    long long inserted = 0;
    long long progress_step = target_rows / 10;
    if (progress_step <= 0)
    {
        progress_step = 1;
    }
    long long next_progress = progress_step;

    while (inserted < target_rows)
    {
        stringstream ss;
        ss << "INSERT INTO BIG_USERS VALUES ";

        int in_batch = 0;
        while (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
        {
            long long id = inserted + 1;
            ss << "(" << id
               << ", 'user" << id << "'"
               << ", 'user" << id << "@mail.com'"
               << ", " << (1000.0 + (id % 10000))
               << ", 1893456000)";
            inserted++;
            in_batch++;
            if (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
            {
                ss << ",";
            }
        }
        ss << ";";

        char *errMsg = nullptr;
        if (flexql_exec(db, ss.str().c_str(), nullptr, nullptr, &errMsg) != FLEXQL_OK)
        {
            cout << "[FAIL] INSERT BIG_USERS batch -> " << (errMsg ? errMsg : "unknown error") << "\n";
            if (errMsg)
            {
                flexql_free(errMsg);
            }
            return false;
        }

        if (inserted >= next_progress || inserted == target_rows)
        {
            cout << "Progress: " << inserted << "/" << target_rows << "\n";
            next_progress += progress_step;
        }
    }

    auto bench_end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(bench_end - bench_start).count();
    long long throughput = (elapsed > 0) ? (target_rows * 1000LL / elapsed) : target_rows;

    cout << "[PASS] INSERT benchmark complete\n";
    cout << "Rows inserted: " << target_rows << "\n";
    cout << "Elapsed: " << elapsed << " ms\n";
    cout << "Throughput: " << throughput << " rows/sec\n";

    return true;
}

int main(int argc, char **argv)
{
    FlexQL *db = nullptr;
    long long insert_rows = DEFAULT_INSERT_ROWS;
    bool run_unit_tests_only = false;

    if (argc > 1)
    {
        string arg1 = argv[1];
        if (arg1 == "--unit-test")
        {
            run_unit_tests_only = true;
        }
        else
        {
            insert_rows = atoll(argv[1]);
            if (insert_rows <= 0)
            {
                cout << "Invalid row count. Use a positive integer or --unit-test.\n";
                return 1;
            }
        }
    }

    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK)
    {
        cout << "Cannot open FlexQL\n";
        return 1;
    }

    cout << "Connected to FlexQL\n";

    if (run_unit_tests_only)
    {
        bool ok = run_data_level_unit_tests(db);
        flexql_close(db);
        return ok ? 0 : 1;
    }

    cout << "Running SQL subset checks plus insertion benchmark...\n";
    cout << "Target insert rows: " << insert_rows << "\n\n";

    // if (!run_insert_benchmark(db, insert_rows)) {
    //     flexql_close(db);
    //     return 1;
    // }

    if (!run_data_level_unit_tests(db))
    {
        flexql_close(db);
        return 1;
    }

    flexql_close(db);
    return 0;
}

// Category             Tests       Weight each
// CREATE TABLE         2           1
// INSERT               8           1
// Basic SELECT *       1           1
// WHERE = / > / <      3           1
// WHERE >= / < (multi-col) 2       2
// INNER JOIN + WHERE   2           4
// Error handling       2           2
