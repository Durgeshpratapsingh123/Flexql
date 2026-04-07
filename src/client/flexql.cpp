/*
 * FlexQL Client Library (flexql.cpp)
 */
#include "../../include/flexql.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

struct FlexQL {
    int sockfd;
};

static char* safe_strdup(const char *s) {
    if (!s) return nullptr;
    size_t len = strlen(s);
    char *buf = (char*)malloc(len + 1);
    if (buf) memcpy(buf, s, len + 1);
    return buf;
}

static bool net_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool net_send_msg(int fd, const std::string &msg) {
    uint32_t len = htonl((uint32_t)msg.size());
    if (!net_send_all(fd, (const char*)&len, 4)) return false;
    return net_send_all(fd, msg.c_str(), msg.size());
}

static std::string net_recv_msg(int fd) {
    uint32_t len_net = 0;
    size_t got = 0;
    while (got < 4) {
        ssize_t n = recv(fd, (char*)&len_net + got, 4 - got, 0);
        if (n <= 0) return "";
        got += n;
    }
    uint32_t len = ntohl(len_net);
    if (len == 0) return "";
    std::string buf(len, '\0');
    got = 0;
    while (got < (size_t)len) {
        ssize_t n = recv(fd, &buf[got], len - got, 0);
        if (n <= 0) return "";
        got += n;
    }
    return buf;
}

extern "C" int flexql_open(const char *host, int port, FlexQL **db) {
    if (!host || !db) return FLEXQL_ERROR;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return FLEXQL_ERROR;
    struct hostent *he = gethostbyname(host);
    if (!he) { close(sockfd); return FLEXQL_ERROR; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    if (connect(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd); return FLEXQL_ERROR;
    }
    FlexQL *handle = (FlexQL*)malloc(sizeof(FlexQL));
    if (!handle) { close(sockfd); return FLEXQL_ERROR; }
    handle->sockfd = sockfd;
    *db = handle;
    return FLEXQL_OK;
}

extern "C" int flexql_close(FlexQL *db) {
    if (!db) return FLEXQL_ERROR;
    close(db->sockfd);
    free(db);
    return FLEXQL_OK;
}

extern "C" int flexql_exec(
    FlexQL *db,
    const char *sql,
    int (*callback)(void*, int, char**, char**),
    void *arg,
    char **errmsg
) {
    if (!db || !sql) {
        if (errmsg) *errmsg = safe_strdup("Invalid database handle or SQL");
        return FLEXQL_ERROR;
    }
    if (!net_send_msg(db->sockfd, std::string(sql))) {
        if (errmsg) *errmsg = safe_strdup("Network send failed");
        return FLEXQL_ERROR;
    }
    std::string resp = net_recv_msg(db->sockfd);
    if (resp.empty()) {
        if (errmsg) *errmsg = safe_strdup("Network receive failed");
        return FLEXQL_ERROR;
    }

    std::istringstream ss(resp);
    std::string line;
    bool had_error = false;
    std::string err_text;
    bool abort_cb = false;

    while (!abort_cb && std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line.substr(0,2) == "OK") continue;
        if (line.size() >= 5 && line.substr(0,5) == "ERROR") {
            had_error = true;
            err_text = line.size() > 6 ? line.substr(6) : "Unknown error";
            continue;
        }
        if (line.size() >= 4 && line.substr(0,4) == "ROWS") {
            long long row_count = 0;
            try { row_count = std::stoll(line.substr(5)); } catch (...) {}
            for (long long r = 0; r < row_count && !abort_cb; ++r) {
                std::string ncols_line;
                if (!std::getline(ss, ncols_line)) break;
                int ncols = 0;
                if (ncols_line.size() >= 6)
                    try { ncols = std::stoi(ncols_line.substr(6)); } catch (...) {}
                std::vector<std::string> cnames(ncols), cvals(ncols);
                for (int c = 0; c < ncols; ++c) {
                    std::getline(ss, cnames[c]);
                    std::getline(ss, cvals[c]);
                }
                if (callback) {
                    std::vector<char*> names_c(ncols), vals_c(ncols);
                    for (int c = 0; c < ncols; ++c) {
                        names_c[c] = (char*)cnames[c].c_str();
                        vals_c[c]  = (char*)cvals[c].c_str();
                    }
                    int cb_ret = callback(arg, ncols, vals_c.data(), names_c.data());
                    if (cb_ret != 0) abort_cb = true;
                }
            }
        }
    }
    if (had_error) {
        if (errmsg) *errmsg = safe_strdup(err_text.c_str());
        return FLEXQL_ERROR;
    }
    return FLEXQL_OK;
}

extern "C" void flexql_free(void *ptr) {
    free(ptr);
}
