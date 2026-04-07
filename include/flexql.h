#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FLEXQL_OK    0
#define FLEXQL_ERROR 1

/* Opaque database handle */
typedef struct FlexQL FlexQL;

/* Establish connection to server */
int flexql_open(const char *host, int port, FlexQL **db);

/* Close connection */
int flexql_close(FlexQL *db);

/* Execute SQL statement; callback invoked for each result row */
int flexql_exec(
    FlexQL *db,
    const char *sql,
    int (*callback)(void*, int, char**, char**),
    void *arg,
    char **errmsg
);

/* Free memory allocated by FlexQL API */
void flexql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* FLEXQL_H */
