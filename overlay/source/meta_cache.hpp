#pragma once

#include <cstddef>

// Background track-name resolver + cache.
//
// Resolving a jelly:// id to "Title - Artist" is an HTTP round-trip
// (jelly_ovl::GetTrackInfo). The Queue used to do this synchronously for EVERY
// item on the UI thread, so a 297-track queue meant ~297 sequential requests and
// the overlay froze for ~30s. This moves resolution onto a worker thread with a
// cache, so the Queue opens instantly and fills names in as they arrive.
namespace meta_cache {

    void Init();   // start the resolver thread (after jelly_ovl::Init)
    void Exit();

    // If `id`'s name is cached, copy "Title - Artist" into `out` and return true.
    // Otherwise schedule a background fetch (deduped) and return false — call again
    // on a later tick to pick it up.
    bool Get(const char *id, char *out, size_t out_sz);

}
