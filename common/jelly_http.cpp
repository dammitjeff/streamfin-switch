#include "jelly_http.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace jelly_http {

    int connect(const char *host, u16 port, int timeout_ms) {
        if (!host || !host[0] || port == 0) return -1;   // not signed in yet

        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port);

        if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
            // Not a numeric IP -> resolve via DNS (flaky in an overlay, so only
            // real hostnames hit this path; numeric IPs took the fast path above).
            struct addrinfo hints;
            std::memset(&hints, 0, sizeof hints);
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo *res = nullptr;
            if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return -1;
            sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        // Non-blocking connect + select() so a dead server can't hang the thread.
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, (struct sockaddr *)&sa, sizeof sa);
        if (rc != 0) {
            if (errno != EINPROGRESS) { close(fd); return -1; }
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv;
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) { close(fd); return -1; }
            int err = 0; socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) { close(fd); return -1; }
        }
        fcntl(fd, F_SETFL, flags);   // back to blocking for send/recv
        return fd;
    }

    std::string auth_header(const char *token, bool with_token) {
        // DeviceId is the overlay's Quick Connect device identity; the sysmodule
        // reuses the same (harmless) string — Jellyfin only needs the Token.
        std::string a = "Authorization: MediaBrowser Client=\"Streamfin\", Device=\"Switch\", "
                        "DeviceId=\"streamfin-overlay\", Version=\"0.1\"";
        if (with_token && token) { a += ", Token=\""; a += token; a += "\""; }
        return a;
    }

    std::string build_request(const char *method, const std::string &path, const char *host,
                              const std::string &extra_headers, const std::string &body) {
        std::string r = std::string(method) + " " + path + " HTTP/1.1\r\n";
        r += "Host: "; r += (host ? host : ""); r += "\r\n";
        r += extra_headers;                 // each line already CRLF-terminated
        r += "User-Agent: Streamfin\r\n";
        if (!body.empty()) {
            r += "Content-Type: application/json\r\n";
            r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        r += "Connection: close\r\n\r\n";
        r += body;
        return r;
    }

    int parse_status(const char *raw) {
        int status = 0;
        std::sscanf(raw, "HTTP/%*d.%*d %d", &status);
        return status;
    }

    long parse_content_range_total(const std::string &headers) {
        auto cr = headers.find("Content-Range:");
        if (cr == std::string::npos) return -1;
        auto sl = headers.find('/', cr);
        if (sl == std::string::npos) return -1;
        return std::strtol(headers.c_str() + sl + 1, nullptr, 10);
    }

}
