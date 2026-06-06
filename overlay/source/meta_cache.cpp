#include "meta_cache.hpp"
#include "jelly_ovl.hpp"
#include "ovl_log.hpp"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace meta_cache {

    namespace {
        Thread  g_thread;
        Mutex   g_mtx;
        CondVar g_cv;
        bool    g_run  = false;
        bool    g_have = false;

        std::unordered_map<std::string, std::string> g_cache;     // id -> "Title - Artist"
        std::vector<std::string>                     g_pending;   // FIFO of ids to resolve
        std::unordered_set<std::string>              g_inflight;  // queued or resolving (dedupe)

        // Backstop against unbounded growth (~200 bytes/entry). In practice bounded
        // by library size, so this is basically never hit; if it is, drop everything
        // and let names re-resolve lazily. 5000 * ~200B ~= 1MB ceiling.
        constexpr size_t MAX_ENTRIES = 5000;

        void worker(void *) {
            while (true) {
                std::string id;
                mutexLock(&g_mtx);
                while (g_run && g_pending.empty()) condvarWait(&g_cv, &g_mtx);
                if (!g_run) { mutexUnlock(&g_mtx); return; }
                id = g_pending.front();
                g_pending.erase(g_pending.begin());
                mutexUnlock(&g_mtx);

                // Slow part (HTTP) runs without the lock.
                char nm[160] = {}, ar[160] = {};
                std::string name;
                if (jelly_ovl::GetTrackInfo(id.c_str(), nm, sizeof nm, ar, sizeof ar) && nm[0]) {
                    name = nm;
                    if (ar[0]) { name += "  -  "; name += ar; }
                } else {
                    name = id;   // fallback: the raw id beats a blank row
                }

                mutexLock(&g_mtx);
                if (g_cache.size() >= MAX_ENTRIES) {
                    g_cache.clear();
                    ovl_log::Line("meta: cache cap %zu hit, cleared", MAX_ENTRIES);
                }
                g_cache[id] = name;
                g_inflight.erase(id);
                mutexUnlock(&g_mtx);
            }
        }
    }

    void Init() {
        if (g_have) return;
        mutexInit(&g_mtx);
        condvarInit(&g_cv);
        g_run = true;
        if (R_SUCCEEDED(threadCreate(&g_thread, worker, nullptr, nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&g_thread);
            g_have = true;
            ovl_log::Line("meta: resolver started");
        } else {
            g_run = false;
            ovl_log::Line("meta: resolver FAILED to start");
        }
    }

    void Exit() {
        if (!g_have) return;
        mutexLock(&g_mtx);
        g_run = false;
        condvarWakeAll(&g_cv);
        mutexUnlock(&g_mtx);
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_cache.clear();
        g_pending.clear();
        g_inflight.clear();
        g_have = false;
    }

    bool Get(const char *id, char *out, size_t out_sz) {
        if (out_sz) out[0] = '\0';
        if (!g_have || !id || !id[0]) return false;
        bool found = false;
        mutexLock(&g_mtx);
        auto it = g_cache.find(id);
        if (it != g_cache.end()) {
            std::snprintf(out, out_sz, "%s", it->second.c_str());
            found = true;
        } else if (g_inflight.find(id) == g_inflight.end()) {
            g_inflight.insert(id);
            g_pending.emplace_back(id);
            condvarWakeAll(&g_cv);
        }
        mutexUnlock(&g_mtx);
        return found;
    }

}
