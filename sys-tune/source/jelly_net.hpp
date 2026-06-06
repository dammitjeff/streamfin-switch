#pragma once

#include <cstddef>
#include <string>
#include <switch.h>

namespace jelly {

    // Persistent HTTP streaming reader with a background prefetch thread.
    // A reader thread pulls bytes from one connection into a ring buffer; the
    // decoder reads from that buffer (memory — never blocks on the network).
    // This decouples network jitter from audio decode so playback stays smooth
    // even while a game contends for the system CPU core. Reopens only on seek.
    class Stream {
        static constexpr size_t RING = 1024 * 1024;   // 1 MB prefetch buffer

        std::string m_path;        // server request path
        int  m_fd     = -1;        // current connection (-1 = none)
        long m_pos    = -1;        // file offset the consumer will read next
        long m_total  = -1;        // total file size (from Content-Range)

        u8     *m_ring  = nullptr;  // prefetch ring (compressed bytes)
        size_t  m_head  = 0;        // consumer index
        size_t  m_tail  = 0;        // producer index
        size_t  m_count = 0;        // bytes currently buffered
        Mutex   m_mtx;
        CondVar m_cv_data;          // signalled when data is available
        CondVar m_cv_space;         // signalled when space frees up
        Thread  m_thread;
        bool    m_have_thread = false;
        bool    m_eof  = false;
        bool    m_stop = false;

        bool open(long offset);     // connect, request [offset,end), parse headers, spawn reader
        void shutdown();            // stop reader, close fd, reset ring
        void reader();              // prefetch thread body
        static void reader_trampoline(void *self) { static_cast<Stream *>(self)->reader(); }

        // Ring-buffer byte moves with wraparound (mutex held; caller adjusts m_count).
        void ring_put(const u8 *src, size_t n);   // src -> ring at m_tail
        void ring_get(u8 *dst, size_t n);         // ring at m_head -> dst

      public:
        explicit Stream(const char *path);
        ~Stream();
        long total();                                  // file size (opens at 0 if needed)
        long read(long offset, void *buf, long size);  // bytes read, or -1 on failure
    };

    // Re-read server + token from the SD config (call before opening a stream).
    void LoadConfig();

}
