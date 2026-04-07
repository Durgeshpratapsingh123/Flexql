/*
 * FlexQL REPL Client
 * Provides an interactive terminal interface for FlexQL.
 *
 * Usage: ./flexql-client <host> <port>
 *        ./flexql-client 127.0.0.1 9000
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include "../../include/flexql.h"

static int print_callback(void* /*data*/, int col_count, char **values, char **col_names) {
    for (int i = 0; i < col_count; ++i) {
        printf("%s = %s\n", col_names[i], values[i] ? values[i] : "NULL");
    }
    printf("\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);

    FlexQL *db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK) {
        fprintf(stderr, "Cannot connect to FlexQL server at %s:%d\n", host, port);
        return 1;
    }
    printf("Connected to FlexQL server\n");

    std::string line, stmt;
    while (true) {
        printf("flexql> ");
        fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        if (line == ".exit" || line == ".quit") break;
        if (line.empty()) continue;

        stmt += " " + line;
        // Execute when we see a semicolon (simple heuristic)
        if (!stmt.empty() && stmt.find(';') != std::string::npos) {
            char *errmsg = nullptr;
            int rc = flexql_exec(db, stmt.c_str(), print_callback, nullptr, &errmsg);
            if (rc != FLEXQL_OK) {
                printf("Error: %s\n", errmsg ? errmsg : "unknown");
                if (errmsg) flexql_free(errmsg);
            }
            stmt.clear();
        }
    }

    flexql_close(db);
    printf("Connection closed\n");
    return 0;
}
