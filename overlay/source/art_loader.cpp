#include "art_loader.hpp"
#include "jelly_ovl.hpp"
#include "ovl_log.hpp"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace art_loader {

    namespace {
        // A decoded (or known-empty) art entry, keyed by track id.
        struct Entry {
            char id[48] = {};
            unsigned char *px = nullptr;   // RGBA, or null for "no art"
            int w = 0, h = 0;
            bool valid = false;
        };

        // We deliberately keep this TINY: only the image on screen and the one
        // being fetched. No look-ahead prefetch — the overlay's heap is small and
        // caching several 256KB images caused decode mallocs to fail. Reliability
        // over readiness.
        constexpr int CACHE = 2;
        Entry g_cache[CACHE];

        char g_want[48]  = {};   // current track id (the one to fetch)
        char g_keep[48]  = {};   // also-pinned: mirrors g_shown so on-screen art holds during swap
        char g_shown[48] = {};   // id whose art Borrow() last handed out (what's actually on screen)
        int  g_size = 256;

        bool   g_run = false;
        bool   g_have_thread = false;
        Thread g_thread;
        Mutex  g_mtx;
        CondVar g_cv;

        // Transient-failure backoff: a fetch that fails non-authoritatively
        // (timeout / truncation / out-of-heap) is retried shortly, up to a cap,
        // instead of being cached as a permanent grey.
        struct Cool { u64 until = 0; int attempts = 0; };
        std::unordered_map<std::string, Cool> g_cooldown;
        constexpr int RETRY_CAP      = 4;
        constexpr u64 RETRY_DELAY_NS = 3'000'000'000ULL;   // 3s between attempts

        u64 cooldown_remaining(const char *id) {
            auto it = g_cooldown.find(id);
            if (it == g_cooldown.end()) return 0;
            const u64 now = armGetSystemTick();
            return it->second.until > now ? (it->second.until - now) : 0;
        }

        bool pinned(const char *id) {
            return !std::strcmp(id, g_want) || !std::strcmp(id, g_keep);
        }

        // (lock held) free every decoded buffer that isn't the on-screen or wanted
        // image — maximizes free heap before a decode.
        void evict_unpinned() {
            for (auto &e : g_cache) {
                if (e.valid && !pinned(e.id)) {
                    if (e.px) std::free(e.px);
                    e = Entry{};
                }
            }
        }

        bool cache_has(const char *id) {
            if (!id || !id[0]) return true;
            for (auto &e : g_cache)
                if (e.valid && !std::strcmp(e.id, id)) return true;
            return false;
        }

        // (lock held) store a finished result, never evicting a pinned id.
        void cache_put(const char *id, unsigned char *px, int w, int h) {
            int slot = -1;
            for (int i = 0; i < CACHE; i++)
                if (!g_cache[i].valid) { slot = i; break; }
            if (slot < 0) {
                for (int i = 0; i < CACHE; i++)
                    if (!pinned(g_cache[i].id)) { slot = i; break; }
                if (slot < 0) slot = 0;
                if (g_cache[slot].px) std::free(g_cache[slot].px);
                g_cache[slot] = Entry{};
            }
            std::snprintf(g_cache[slot].id, sizeof g_cache[slot].id, "%s", id);
            g_cache[slot].px = px;
            g_cache[slot].w = w;
            g_cache[slot].h = h;
            g_cache[slot].valid = true;
        }

        void worker(void *) {
            while (true) {
                char job[48] = {};
                int  size = 256;

                mutexLock(&g_mtx);
                while (g_run) {
                    if (g_want[0] && !cache_has(g_want)) {
                        const u64 rem = cooldown_remaining(g_want);
                        if (rem == 0) { std::snprintf(job, sizeof job, "%s", g_want); break; }
                        condvarWaitTimeout(&g_cv, &g_mtx, armTicksToNs(rem) + 1'000'000ULL);
                        continue;
                    }
                    condvarWait(&g_cv, &g_mtx);
                }
                if (!g_run) { mutexUnlock(&g_mtx); return; }
                size = g_size;
                evict_unpinned();   // free heap for the decode
                mutexUnlock(&g_mtx);

                ovl_log::Line("art: fetch %.8s", job);
                const u64 t0 = armGetSystemTick();
                int w = 0, h = 0;
                bool definitive = false;
                unsigned char *px = jelly_ovl::GetCoverArt(job, size, &w, &h, &definitive);
                const u64 dt = armTicksToNs(armGetSystemTick() - t0) / 1'000'000ULL;

                mutexLock(&g_mtx);
                if (px) {
                    ovl_log::Line("art:  -> %.8s %dx%d in %llums", job, w, h, (unsigned long long)dt);
                    cache_put(job, px, w, h);
                    g_cooldown.erase(job);
                } else if (definitive) {
                    ovl_log::Line("art:  -> %.8s NO ART (definitive) in %llums", job, (unsigned long long)dt);
                    cache_put(job, nullptr, 0, 0);
                    g_cooldown.erase(job);
                } else {
                    Cool &c = g_cooldown[job];
                    c.attempts++;
                    if (c.attempts >= RETRY_CAP) {
                        ovl_log::Line("art:  -> %.8s gave up after %d tries", job, c.attempts);
                        cache_put(job, nullptr, 0, 0);
                        g_cooldown.erase(job);
                    } else {
                        ovl_log::Line("art:  -> %.8s transient (try %d), retry in 3s", job, c.attempts);
                        c.until = armGetSystemTick() + armNsToTicks(RETRY_DELAY_NS);
                    }
                }
                mutexUnlock(&g_mtx);
            }
        }
    }

    void Init() {
        if (g_have_thread) return;
        mutexInit(&g_mtx);
        condvarInit(&g_cv);
        for (auto &e : g_cache) e = Entry{};
        g_want[0] = g_keep[0] = '\0';
        g_run = true;
        if (R_SUCCEEDED(threadCreate(&g_thread, worker, nullptr, nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&g_thread);
            g_have_thread = true;
            ovl_log::Line("art: loader started");
        } else {
            g_run = false;
            ovl_log::Line("art: loader FAILED to start thread");
        }
    }

    void Exit() {
        if (!g_have_thread) return;
        mutexLock(&g_mtx);
        g_run = false;
        condvarWakeAll(&g_cv);
        mutexUnlock(&g_mtx);
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        for (auto &e : g_cache) { if (e.px) std::free(e.px); e = Entry{}; }
        g_cooldown.clear();
        g_have_thread = false;
    }

    void Track(const char *id, int size) {
        if (!g_have_thread) return;
        mutexLock(&g_mtx);
        const bool want_changed = std::strcmp(g_want, id ? id : "") != 0;
        g_size = size;
        std::snprintf(g_want, sizeof g_want, "%s", id ? id : "");
        std::snprintf(g_keep, sizeof g_keep, "%s", g_shown);   // hold whatever's on screen
        if (g_want[0]) g_cooldown.erase(g_want);   // landing on a track -> retry its art now
        const bool want_cached = cache_has(g_want);
        condvarWakeAll(&g_cv);
        mutexUnlock(&g_mtx);

        if (want_changed && id && id[0])
            ovl_log::Line("art: request want=%.8s%s", id, want_cached ? " [HIT cache]" : "");
    }

    void Clear() {
        if (!g_have_thread) return;
        mutexLock(&g_mtx);
        g_want[0] = g_keep[0] = g_shown[0] = '\0';   // release all pins; nothing shown
        condvarWakeAll(&g_cv);
        mutexUnlock(&g_mtx);
    }

    void Borrow(unsigned char **px, int *w, int *h) {
        if (px) *px = nullptr;
        if (w)  *w  = 0;
        if (h)  *h  = 0;
        if (!g_have_thread) return;
        mutexLock(&g_mtx);
        // Prefer the current track's art once it's decoded; promote it to "shown".
        if (g_want[0]) {
            for (auto &e : g_cache) {
                if (e.valid && !std::strcmp(e.id, g_want)) {
                    std::snprintf(g_shown, sizeof g_shown, "%s", g_want);
                    if (px) *px = e.px;   // BORROW — cache keeps ownership
                    if (w)  *w  = e.w;
                    if (h)  *h  = e.h;
                    mutexUnlock(&g_mtx);
                    return;
                }
            }
        }
        // Not ready yet: keep showing the previously-shown art (held pinned).
        if (g_shown[0]) {
            for (auto &e : g_cache) {
                if (e.valid && !std::strcmp(e.id, g_shown)) {
                    if (px) *px = e.px;
                    if (w)  *w  = e.w;
                    if (h)  *h  = e.h;
                    break;
                }
            }
        }
        mutexUnlock(&g_mtx);
    }

}
