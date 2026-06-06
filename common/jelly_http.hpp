#pragma once

#include <string>
#include <switch.h>

// Shared low-level Jellyfin HTTP transport, linked into BOTH binaries: the
// overlay (one-shot request/response) and the sysmodule (one long-lived streaming
// connection feeding a ring buffer). Only the connect/build/parse/auth primitives
// are shared here — each caller keeps its own read loop.
namespace jelly_http {

    // Connect a TCP socket to host:port with a connect timeout (ms). Numeric IPs go
    // straight through inet_pton; hostnames resolve via getaddrinfo (DNS). The fd is
    // returned in blocking mode (for send/recv), or -1 on failure — including an
    // empty host or port 0, i.e. "not signed in yet".
    int connect(const char *host, u16 port, int timeout_ms);

    // The "Authorization: MediaBrowser ..." header LINE (no trailing CRLF).
    // with_token appends the saved access token.
    std::string auth_header(const char *token, bool with_token);

    // Assemble a raw HTTP/1.1 request. `extra_headers` carries any request-specific
    // header lines, each already CRLF-terminated (e.g. the auth line, a Range line).
    // When `body` is non-empty, JSON Content-Type + Content-Length are added.
    std::string build_request(const char *method, const std::string &path, const char *host,
                              const std::string &extra_headers, const std::string &body = "");

    // Status code from a raw "HTTP/x.y NNN ..." response (0 if unparseable).
    int parse_status(const char *raw);

    // Total size from a "Content-Range: bytes a-b/TOTAL" header within `headers`,
    // or -1 if absent.
    long parse_content_range_total(const std::string &headers);

}
