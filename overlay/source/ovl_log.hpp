#pragma once

// Lightweight overlay-side debug log -> sdmc:/jelly_ovl.log.
// Thread-safe (the art worker thread and the UI thread both write). Each line is
// prefixed with a +milliseconds-since-first-line timestamp so you can see how
// long fetches take and when prefetch kicks in. Pull it over FTP at
//   ftp://<switch>:5000/sdmc:/jelly_ovl.log
//
// Compiled out unless JELLY_DEBUG is defined (see overlay/Makefile). Release
// builds get inline no-ops, so there is no SD write and no runtime cost, while
// the printf-format checking still applies at every call site.
namespace ovl_log {
#ifdef JELLY_DEBUG
    void Line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    void Clear();
#else
    __attribute__((format(printf, 1, 2))) inline void Line(const char *, ...) {}
    inline void Clear() {}
#endif
}
