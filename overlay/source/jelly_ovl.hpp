#pragma once

#include <cstddef>

// Overlay-side Jellyfin client: resolves metadata (and later cover art) over HTTP
// so the now-playing UI can show real song/artist names instead of jelly:// ids.
namespace jelly_ovl {

    void Init();   // socketInitialize (minimal, TCP-only)
    void Exit();

    // Re-read server/token/userid from the SD config (call after sign-in changes).
    void ReloadConfig();

    // ---- Quick Connect pairing ----
    // Begin: checks enabled + initiates; fills the 6-digit code + secret. false on error.
    bool QcBegin(char *code, size_t code_sz, char *secret, size_t secret_sz);
    // Poll: 0 = still waiting, 1 = authorized (token+userid filled & SAVED to config), -1 = error.
    int  QcPoll(const char *secret, char *token, size_t token_sz, char *uid, size_t uid_sz);

    // GET /Users/{uid}/Items/{id} and pull Name + primary artist.
    // Returns true on success; name/artist are filled (NUL-terminated).
    bool GetTrackInfo(const char *id, char *name, size_t name_sz, char *artist, size_t artist_sz);

    // ---- Browse (the Picker) ----
    // A browsable entry (playlist or artist).
    struct Item {
        char id[48];
        char name[160];
    };
    // A resolved, playable track: id + decoder fmt ("flac"/"mp3"/"wav").
    struct Track {
        char id[48];
        char fmt[8];
    };

    // List the user's playlists / artists. Returns count written (<= max), -1 on error.
    int ListPlaylists(Item *out, int max);
    int ListArtists(Item *out, int max);

    // Resolve a selection to playable tracks (unsupported containers are skipped).
    // Returns count written (<= max), -1 on error.
    int GetPlaylistTracks(const char *playlistId, Track *out, int max);
    int GetArtistTracks(const char *artistId, Track *out, int max);  // the artist's own songs (shuffled)
    int GetShuffleAll(Track *out, int max);                          // random across library
    // InstantMix "radio" seeded by any item (artist, playlist, song, ...): a mix of
    // similar music. Used for the Y-button "Start Radio" action.
    int GetInstantMix(const char *itemId, Track *out, int max);

    // Fetch the track's primary image (falling back to the album's) as a `size`x
    // `size` thumbnail, decoded to RGBA8888. Returns a malloc'd buffer (free with
    // free()) of *w * *h * 4 bytes, or nullptr.
    // *definitive (optional): true if the nullptr is authoritative (genuine no-art);
    // false if a transient failure (timeout/truncation) — caller should retry.
    unsigned char *GetCoverArt(const char *id, int size, int *w, int *h, bool *definitive = nullptr);

}
