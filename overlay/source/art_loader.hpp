#pragma once

// Background cover-art loader for the now-playing view.
//
// jelly_ovl::GetCoverArt() does a blocking HTTP GET + JPEG decode; running it on
// the overlay's UI thread stutters the menu every time the track changes. This
// moves the work onto a worker thread.
//
// Memory note: the overlay heap is small, so we deliberately do NOT prefetch /
// cache ahead. Only two images are ever resident — the one on screen and the one
// being fetched — which keeps decode mallocs from failing under pressure.
namespace art_loader {

    void Init();   // start the worker thread (call once, after jelly_ovl::Init)
    void Exit();   // stop the worker + free the cache

    // Set the current track to fetch+display art for, at `size` px. The loader
    // keeps the previously-SHOWN art pinned until `id`'s art is decoded, so a track
    // change swaps with no grey flash. Call every tick while a track is current.
    void Track(const char *id, int size);

    // Nothing is playing: release all pins; Borrow() then reports no art.
    void Clear();

    // The art to draw right now: the current track's art once decoded (which the
    // loader then promotes to "shown"), otherwise the previously-shown art held
    // across the swap. *px is a BORROWED RGBA buffer (do NOT free) or null, valid
    // only until the next tick (both the current and shown ids stay pinned).
    void Borrow(unsigned char **px, int *w, int *h);

}
