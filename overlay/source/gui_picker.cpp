#include "gui_picker.hpp"

#include "elm_overlayframe.hpp"
#include "jelly_ovl.hpp"
#include "tune.h"

#include <vector>
#include <cstdio>

namespace {

    constexpr int MAX_TRACKS  = 300;   // cap per selection (InstantMix/Shuffle radius)
    constexpr int MAX_ENTRIES = 400;   // cap on playlists/artists listed

    // Clear the queue, enqueue every resolved track, and start playback.
    void play_tracks(const std::vector<jelly_ovl::Track> &tr, StreamfinOverlayFrame *frame, const char *what) {
        if (tr.empty()) {
            if (frame) frame->setToast(what, "Nothing playable found");
            return;
        }
        tuneClearQueue();
        char path[80];
        for (const auto &t : tr) {
            std::snprintf(path, sizeof path, "jelly://%s/%s", t.fmt, t.id);
            tuneEnqueue(path, TuneEnqueueType_Back);
        }
        tuneSelect(0);
        tunePlay();
        char msg[96];
        std::snprintf(msg, sizeof msg, "Queued %d track%s", (int)tr.size(), tr.size() == 1 ? "" : "s");
        if (frame) frame->setToast(what, msg);
    }

    // Resolve via `fn`, then play. Shows a toast on a network/parse failure.
    template <typename Fn>
    void resolve_and_play(StreamfinOverlayFrame *frame, const char *what, Fn &&fn) {
        std::vector<jelly_ovl::Track> tr(MAX_TRACKS);
        int n = fn(tr.data(), MAX_TRACKS);
        if (n < 0) {
            if (frame) frame->setToast(what, "Couldn't reach server");
            return;
        }
        tr.resize(n);
        play_tracks(tr, frame, what);
    }

}

PickerGui::PickerGui(Mode mode) : m_mode(mode) {}

tsl::elm::Element *PickerGui::createUI() {
    auto frame = new StreamfinOverlayFrame();
    auto list  = new tsl::elm::List();

    if (m_mode == Mode::Root) {
        frame->setDescription("  Back       Select");

        auto shuffle = new tsl::elm::ListItem("Shuffle All");
        shuffle->setClickListener([frame](u64 keys) {
            if (keys & HidNpadButton_A) {
                resolve_and_play(frame, "Shuffle All",
                    [](jelly_ovl::Track *o, int m) { return jelly_ovl::GetShuffleAll(o, m); });
                return true;
            }
            return false;
        });
        list->addItem(shuffle);

        auto playlists = new tsl::elm::ListItem("Playlists", ">");
        playlists->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) { tsl::changeTo<PickerGui>(Mode::Playlists); return true; }
            return false;
        });
        list->addItem(playlists);

        auto artists = new tsl::elm::ListItem("Artists", ">");
        artists->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) { tsl::changeTo<PickerGui>(Mode::Artists); return true; }
            return false;
        });
        list->addItem(artists);

        frame->setContent(list);
        return frame;
    }

    // Playlists / Artists leaf list.
    const bool is_playlists = (m_mode == Mode::Playlists);
    frame->setDescription("  Back      Play      Radio");

    std::vector<jelly_ovl::Item> items(MAX_ENTRIES);
    int n = is_playlists ? jelly_ovl::ListPlaylists(items.data(), MAX_ENTRIES)
                         : jelly_ovl::ListArtists(items.data(), MAX_ENTRIES);

    if (n < 0) {
        list->addItem(new tsl::elm::ListItem("Couldn't reach server"));
    } else if (n == 0) {
        list->addItem(new tsl::elm::ListItem(is_playlists ? "No playlists found" : "No artists found"));
    } else {
        for (int i = 0; i < n; i++) {
            std::string id   = items[i].id;
            std::string name = items[i].name;
            auto item = new tsl::elm::ListItem(name);
            item->setClickListener([frame, id, name, is_playlists](u64 keys) {
                if (keys & HidNpadButton_A) {
                    resolve_and_play(frame, name.c_str(),
                        [&](jelly_ovl::Track *o, int m) {
                            return is_playlists ? jelly_ovl::GetPlaylistTracks(id.c_str(), o, m)
                                                : jelly_ovl::GetArtistTracks(id.c_str(), o, m);
                        });
                    return true;
                }
                if (keys & HidNpadButton_Y) {
                    // InstantMix radio seeded by this artist/playlist.
                    std::string what = std::string("Radio: ") + name;
                    resolve_and_play(frame, what.c_str(),
                        [&](jelly_ovl::Track *o, int m) { return jelly_ovl::GetInstantMix(id.c_str(), o, m); });
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }
    }

    frame->setContent(list);
    return frame;
}
