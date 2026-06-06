#include "ovl_log.hpp"

#ifdef JELLY_DEBUG

#include "sdmc/sdmc.hpp"

#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

namespace ovl_log {

    namespace {
        Mutex  g_mtx;
        bool   g_init  = false;
        u64    g_start = 0;
        std::string g_buf;
        constexpr size_t MAX = 24 * 1024;   // keep the tail; rewrite each line

        void ensure() {
            if (!g_init) { mutexInit(&g_mtx); g_init = true; }
        }
    }

    void Line(const char *fmt, ...) {
        ensure();

        char body[224];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(body, sizeof body, fmt, ap);
        va_end(ap);

        mutexLock(&g_mtx);
        if (g_start == 0) g_start = armGetSystemTick();
        const u64 ms = armTicksToNs(armGetSystemTick() - g_start) / 1'000'000ULL;

        char line[256];
        std::snprintf(line, sizeof line, "[%6llu] %s\n", (unsigned long long)ms, body);
        g_buf += line;
        if (g_buf.size() > MAX) g_buf.erase(0, g_buf.size() - MAX);
        sdmc::WriteFile("/jelly_ovl.log", g_buf.data(), g_buf.size());
        mutexUnlock(&g_mtx);
    }

    void Clear() {
        ensure();
        mutexLock(&g_mtx);
        g_buf.clear();
        sdmc::WriteFile("/jelly_ovl.log", "", 0);
        mutexUnlock(&g_mtx);
    }

}

#endif // JELLY_DEBUG
