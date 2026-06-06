#include "jelly_config.hpp"
#include "config/config.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace jelly_cfg {

    namespace {
        char g_host[160]  = {};
        u16  g_port       = 0;
        char g_token[160] = {};

        // Parse "host:port" (optionally http(s):// prefixed) into g_host/g_port.
        void parse_server(const char *s) {
            if (!std::strncmp(s, "http://", 7))       s += 7;
            else if (!std::strncmp(s, "https://", 8)) s += 8;
            const char *colon = std::strrchr(s, ':');
            if (colon && colon[1]) {
                size_t hl = (size_t)(colon - s);
                if (hl >= sizeof g_host) hl = sizeof g_host - 1;
                std::memcpy(g_host, s, hl);
                g_host[hl] = '\0';
                g_port = (u16)std::atoi(colon + 1);
            } else {
                std::snprintf(g_host, sizeof g_host, "%s", s);
                g_port = 8096;   // default Jellyfin port
            }
        }
    }

    void Reload() {
        char server[160] = {};
        config::get_jelly_server(server, sizeof server);
        if (server[0]) {
            parse_server(server);
        } else {
            g_host[0] = '\0';   // not configured -> connect() bails until sign-in
            g_port    = 0;
        }
        char token[160] = {};
        config::get_jelly_token(token, sizeof token);
        std::snprintf(g_token, sizeof g_token, "%s", token);
    }

    const char *host()  { return g_host; }
    u16         port()  { return g_port; }
    const char *token() { return g_token; }

}
