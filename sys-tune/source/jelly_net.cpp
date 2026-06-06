#include "jelly_net.hpp"
#include "jelly_http.hpp"
#include "jelly_config.hpp"

#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <new>

namespace jelly {

    // Re-read the live server/token from the SD config (delegates to the shared
    // jelly_cfg state). Called per track open so a sign-in via the overlay takes
    // effect without a reboot.
    void LoadConfig() {
        jelly_cfg::Reload();
    }

    // ---- persistent streaming reader ----
    Stream::Stream(const char *path) : m_path(path) {
        m_ring = new (std::nothrow) u8[RING];
        mutexInit(&m_mtx);
        condvarInit(&m_cv_data);
        condvarInit(&m_cv_space);
    }

    Stream::~Stream() {
        shutdown();
        delete[] m_ring;
    }

    // Ring-buffer byte moves with wraparound. Caller holds m_mtx and adjusts
    // m_count; these just shuttle bytes and advance the head/tail index.
    void Stream::ring_put(const u8 *src, size_t n) {
        for (size_t i = 0; i < n; i++) {
            m_ring[m_tail] = src[i];
            m_tail = (m_tail + 1) % RING;
        }
    }
    void Stream::ring_get(u8 *dst, size_t n) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = m_ring[m_head];
            m_head = (m_head + 1) % RING;
        }
    }

    // Background producer: pull bytes off the socket into the ring buffer.
    void Stream::reader() {
        const int fd = m_fd;
        u8 tmp[16384];
        while (true) {
            ssize_t n = recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) {  // EOF (Connection: close), error, or socket closed by shutdown()
                mutexLock(&m_mtx);
                m_eof = true;
                condvarWakeAll(&m_cv_data);
                mutexUnlock(&m_mtx);
                return;
            }
            size_t off = 0;
            while (off < (size_t)n) {
                mutexLock(&m_mtx);
                while (m_count == RING && !m_stop) condvarWait(&m_cv_space, &m_mtx);
                if (m_stop) { mutexUnlock(&m_mtx); return; }
                size_t put = RING - m_count;
                if (put > (size_t)n - off) put = (size_t)n - off;
                ring_put(tmp + off, put);
                m_count += put;
                off += put;
                condvarWakeAll(&m_cv_data);
                mutexUnlock(&m_mtx);
            }
        }
    }

    void Stream::shutdown() {
        if (m_have_thread) {
            mutexLock(&m_mtx);
            m_stop = true;
            condvarWakeAll(&m_cv_space);
            condvarWakeAll(&m_cv_data);
            mutexUnlock(&m_mtx);
            if (m_fd >= 0) { close(m_fd); m_fd = -1; }  // interrupt a blocking recv
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_have_thread = false;
        } else if (m_fd >= 0) {
            close(m_fd); m_fd = -1;
        }
        m_head = m_tail = m_count = 0;
        m_eof = m_stop = false;
    }

    bool Stream::open(long offset) {
        shutdown();
        if (!m_ring) return false;

        m_fd = jelly_http::connect(jelly_cfg::host(), jelly_cfg::port(), 5000);
        if (m_fd < 0) return false;

        // Open-ended range so the server streams from `offset` to EOF on one connection.
        char range[48];
        std::snprintf(range, sizeof range, "Range: bytes=%ld-\r\n", offset);
        const std::string extra = jelly_http::auth_header(jelly_cfg::token(), true) + "\r\n" + range;
        const std::string req = jelly_http::build_request("GET", m_path, jelly_cfg::host(), extra, "");
        if (send(m_fd, req.data(), req.size(), 0) < 0) { close(m_fd); m_fd = -1; return false; }

        // Read until the end of headers.
        std::string hdr;
        char buf[8192];
        size_t he;
        while ((he = hdr.find("\r\n\r\n")) == std::string::npos) {
            ssize_t n = recv(m_fd, buf, sizeof buf, 0);
            if (n <= 0) { close(m_fd); m_fd = -1; return false; }
            hdr.append(buf, n);
        }

        const int status = jelly_http::parse_status(hdr.c_str());
        if (status != 200 && status != 206) { close(m_fd); m_fd = -1; return false; }

        if (m_total < 0) {
            long total = jelly_http::parse_content_range_total(hdr);
            if (total >= 0) {
                m_total = total;
            } else {
                auto cl = hdr.find("Content-Length:");
                if (cl != std::string::npos) m_total = offset + std::strtol(hdr.c_str() + cl + 15, nullptr, 10);
            }
        }

        // Seed the ring with body bytes already received past the headers.
        std::string residual = hdr.substr(he + 4);
        size_t seed = residual.size() < RING ? residual.size() : RING;
        std::memcpy(m_ring, residual.data(), seed);
        m_tail = seed % RING;
        m_count = seed;
        m_head = 0;
        m_pos = offset;
        m_eof = m_stop = false;

        if (R_FAILED(threadCreate(&m_thread, reader_trampoline, this, nullptr, 0x8000, 0x20, -2))) {
            close(m_fd); m_fd = -1; return false;
        }
        threadStart(&m_thread);
        m_have_thread = true;
        return true;
    }

    long Stream::total() {
        if (m_total >= 0) return m_total;
        if (open(0)) return m_total < 0 ? 0 : m_total;
        return 0;
    }

    long Stream::read(long offset, void *buf, long size) {
        // (Re)open on first use or on a non-sequential seek.
        if (!m_have_thread || offset != m_pos) {
            if (!open(offset)) return -1;
        }

        u8 *out = (u8 *)buf;
        long got = 0;
        while (got < size) {
            mutexLock(&m_mtx);
            while (m_count == 0 && !m_eof) condvarWait(&m_cv_data, &m_mtx);
            if (m_count == 0 && m_eof) { mutexUnlock(&m_mtx); break; }
            size_t take = m_count;
            if (take > (size_t)(size - got)) take = (size_t)(size - got);
            ring_get(out + got, take);
            m_count -= take;
            got += take;
            condvarWakeAll(&m_cv_space);
            mutexUnlock(&m_mtx);
        }
        m_pos += got;
        return got;
    }

}
