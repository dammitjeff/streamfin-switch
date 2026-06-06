#include "elm_status_bar.hpp"

#include "symbol.hpp"
#include "config/config.hpp"
#include "jelly_ovl.hpp"
#include "art_loader.hpp"
#include "ovl_log.hpp"

#include <cstdlib>
#include <cstdio>
#include <vector>

namespace {

    char path_buffer[FS_MAX_PATH] = "";
    char current_buffer[0x20] = "";
    char total_buffer[0x20] = "";

    void NullLastDot(char *str) {
        char *end = str + strlen(str) - 1;
        while (str != end) {
            if (*end == '.') {
                *end = '\0';
                return;
            }
            end--;
        }
    }

}

StatusBar::StatusBar() {
    if (R_FAILED(tuneGetRepeatMode(&this->m_repeat)))
        this->m_repeat = TuneRepeatMode_Off;
    if (R_FAILED(tuneGetShuffleMode(&this->m_shuffle)))
        this->m_shuffle = TuneShuffleMode_Off;
    if (R_FAILED(tuneGetCurrentQueueItem(path_buffer, FS_MAX_PATH, &this->m_stats))) {
        path_buffer[0] = '\0';
        this->m_stats = {};
    }
}

StatusBar::~StatusBar() {
    // m_art is borrowed from art_loader's cache; it owns/frees it. Nothing to do.
}

tsl::elm::Element *StatusBar::requestFocus(tsl::elm::Element *oldFocus, tsl::FocusDirection direction) {
    return this;
}

void StatusBar::draw(tsl::gfx::Renderer *renderer) {
    if (this->m_touched && Element::getInputMode() == tsl::InputMode::Touch) {
        renderer->drawRect(ELEMENT_BOUNDS(this), a(tsl::style::color::ColorClickAnimation));
    }

    const s32 cx = CenterX();

    /* Album art, centered on top (PS5-style). Placeholder box until it loads. */
    if (this->m_art) {
        renderer->drawBitmap(cx - this->m_art_w / 2, ArtY(), this->m_art_w, this->m_art_h, this->m_art);
    } else {
        renderer->drawRect(cx - ART_PX / 2, ArtY(), ART_PX, ART_PX, a(tsl::style::color::ColorFrame));
    }

    /* Title (+ artist), centered; scrolls if too wide. */
    const s32 title_avail = this->getWidth() - 30;
    if (this->m_text_width == 0) {
        auto [width, height] = renderer->drawString(this->m_current_track.data(), false, 0, 0, 23, tsl::style::color::ColorTransparent);
        this->m_truncated = static_cast<s32>(width) > title_avail;
        if (this->m_truncated) {
            this->m_scroll_text = std::string(m_current_track).append("       ");
            auto [ex_width, ex_height] = renderer->drawString(this->m_scroll_text.c_str(), false, 0, 0, 23, tsl::style::color::ColorTransparent);
            this->m_scroll_text.append(m_current_track);
            this->m_text_width = ex_width;
        } else {
            this->m_text_width = width;
        }
    }
    if (this->m_truncated) {
        renderer->drawString(this->m_scroll_text.c_str(), false, this->getX() + 15 - this->m_scroll_offset, TitleY(), 23, tsl::style::color::ColorText);
        if (this->m_counter == 120) {
            if (this->m_scroll_offset == this->m_text_width) { this->m_scroll_offset = 0; this->m_counter = 0; }
            else { this->m_scroll_offset++; }
        } else {
            this->m_counter++;
        }
    } else {
        renderer->drawString(this->m_current_track.data(), false, cx - this->m_text_width / 2, TitleY(), 23, tsl::style::color::ColorText);
    }

    /* Seek bar + times. */
    const s32 bar_x   = this->getX() + 15;
    const u32 bar_len = this->getWidth() - 30;
    const s32 bar_y   = SeekY();
    renderer->drawRect(bar_x, bar_y, bar_len, 3, 0xF666);   // dim track
    if (this->m_percentage > 0) {
        // Filled portion as a Jellyfin blue->purple gradient (RGBA4444: 0x(a)(b)(g)(r)).
        // blue #00A4DC -> r0 gA bD,  purple #AA5CC3 -> rA g5 bC.
        const u32 fill = (u32)(bar_len * this->m_percentage);
        for (u32 i = 0; i < fill; i++) {
            const float t = bar_len > 0 ? (float)i / (float)bar_len : 0.f;
            const u8 r = (u8)(0x0 + 0xA * t);
            const u8 g = (u8)(0xA - 0x5 * t);
            const u8 b = (u8)(0xD - 0x1 * t);
            renderer->drawRect(bar_x + (s32)i, bar_y - 2, 1, 7, (u16)((0xF << 12) | (b << 8) | (g << 4) | r));
        }
        const float tp = this->m_percentage;
        const u8 r = (u8)(0x0 + 0xA * tp), g = (u8)(0xA - 0x5 * tp), b = (u8)(0xD - 0x1 * tp);
        renderer->drawCircle(bar_x + (s32)fill, bar_y + 1, 4, true, (u16)((0xF << 12) | (b << 8) | (g << 4) | r));
    }
    renderer->drawString(current_buffer, false, bar_x, bar_y + 26, 18, 0xffff);
    renderer->drawString(total_buffer, false, this->getX() + this->getWidth() - 55, bar_y + 26, 18, 0xffff);

    /* Subtle faded square behind the selected control, centered on the glyph
       (AlphaSymbol::draw centers on x,y). */
    {
        const s32 hs = 40;
        renderer->drawRect(FocusX(this->m_focus) - hs / 2, ControlsY() - hs / 2, hs, hs, 0x4FFF);
    }

    /* Repeat / shuffle / transport controls. */
    auto repeat_color = this->m_repeat ? tsl::style::color::ColorHighlight : tsl::style::color::ColorHeaderBar;
    if (this->m_repeat == TuneRepeatMode_One)
        symbol::repeat::one::symbol.draw(GetRepeatX(), GetRepeatY(), renderer, repeat_color);
    else
        symbol::repeat::all::symbol.draw(GetRepeatX(), GetRepeatY(), renderer, repeat_color);

    auto shuffle_color = this->m_shuffle ? tsl::style::color::ColorHighlight : tsl::style::color::ColorHeaderBar;
    symbol::shuffle::symbol.draw(GetShuffleX(), GetShuffleY(), renderer, shuffle_color);

    symbol::prev::symbol.draw(GetPrevX(), GetPrevY(), renderer, tsl::style::color::ColorText);
    this->GetPlaybackSymbol().draw(GetPlayStateX(), GetPlayStateY(), renderer, tsl::style::color::ColorText);
    symbol::next::symbol.draw(GetNextX(), GetNextY(), renderer, tsl::style::color::ColorText);
}

void StatusBar::layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) {
    this->setBoundaries(this->getX(), this->getY(), this->getWidth(), HEIGHT);
}

bool StatusBar::onClick(u64 keys) {
    u8 handled = 0;
    /* Left/Right move the control cursor; A activates the selected control.
       X/Y stay as quick repeat/shuffle shortcuts. */
    if (keys & HidNpadButton_Left)  { if (this->m_focus > 0) this->m_focus--; handled++; }
    if (keys & HidNpadButton_Right) { if (this->m_focus < 4) this->m_focus++; handled++; }
    if (keys & HidNpadButton_A)     { this->ActivateControl(this->m_focus); handled++; }
    if (keys & HidNpadButton_X)     { this->CycleRepeat();  handled++; }
    if (keys & HidNpadButton_Y)     { this->CycleShuffle(); handled++; }
    return handled;
}

void StatusBar::ActivateControl(int index) {
    switch (index) {
        case 0: this->CycleRepeat();  break;
        case 1: this->Prev();         break;
        case 2: this->CyclePlay();    break;
        case 3: this->Next();         break;
        case 4: this->CycleShuffle(); break;
    }
}

#define TOUCHED(button) (currX > (Get##button##X() - 30) && currX < (Get##button##X() + 30) && prevY > (Get##button##Y() - 30) && prevY < (Get##button##Y() + 30))

bool StatusBar::onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) {
    if (event == tsl::elm::TouchEvent::Touch)
        this->m_touched = this->inBounds(currX, currY);

    if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
        this->m_touched = false;

        if (Element::getInputMode() == tsl::InputMode::Touch) {
            u16 handled = 0;
            if (TOUCHED(Repeat)) {
                this->m_focus = 0;
                this->CycleRepeat();
                handled++;
            }
            if (TOUCHED(Shuffle)) {
                this->m_focus = 4;
                this->CycleShuffle();
                handled++;
            }
            if (TOUCHED(PlayState)) {
                this->m_focus = 2;
                this->CyclePlay();
                handled++;
            }
            if (TOUCHED(Prev)) {
                this->m_focus = 1;
                this->Prev();
                handled++;
            }
            if (TOUCHED(Next)) {
                this->m_focus = 3;
                this->Next();
                handled++;
            }

            if (handled > 0) {
                this->m_clickAnimationProgress = 0;
                return true;
            }
        }
    }

    return false;
}

void StatusBar::update() {
    if (R_FAILED(tuneGetStatus(&this->m_playing)))
        this->m_playing = false;

    if (R_SUCCEEDED(tuneGetCurrentQueueItem(path_buffer, FS_MAX_PATH, &this->m_stats))) {
        this->m_phase = UiPhase::Playing;   // the "now playing" line logs the transition
        if (std::strncmp(path_buffer, "jelly://", 8) == 0)
            this->ResolveJellyTrack();
        else
            this->ResolveSdFile();
    } else {
        this->HandleNoItem();
    }
    this->RefreshArt();

    /* Progress text and bar */
    u32 current = this->m_stats.current_frame / this->m_stats.sample_rate;
    u32 total = this->m_stats.total_frames / this->m_stats.sample_rate;
    this->m_percentage = std::clamp(float(this->m_stats.current_frame) / float(this->m_stats.total_frames), 0.0f, 1.0f);

    std::snprintf(current_buffer, sizeof(current_buffer), "%d:%02d", current / 60, current % 60);
    std::snprintf(total_buffer, sizeof(total_buffer), "%d:%02d", total / 60, total % 60);
}

void StatusBar::ResolveJellyTrack() {
    /* Jellyfin track: resolve "Title - Artist" once per id (HTTP). */
    const char *id = std::strrchr(path_buffer, '/');
    id = id ? id + 1 : path_buffer + 8;
    if (std::strcmp(id, this->m_last_id) == 0)
        return;   // same track -> nothing to refetch

    std::strncpy(this->m_last_id, id, sizeof(this->m_last_id) - 1);
    char nm[128] = {}, ar[128] = {};
    if (jelly_ovl::GetTrackInfo(id, nm, sizeof nm, ar, sizeof ar) && nm[0]) {
        if (ar[0])
            std::snprintf(this->m_name_buf, sizeof(this->m_name_buf), "%s  -  %s", nm, ar);
        else
            std::snprintf(this->m_name_buf, sizeof(this->m_name_buf), "%s", nm);
    } else {
        std::snprintf(this->m_name_buf, sizeof(this->m_name_buf), "%s", id);
    }
    this->m_current_track = this->m_name_buf;
    ovl_log::Line("ui: now playing %.8s \"%s\"", id, this->m_name_buf);

    /* Don't blank art here — the old cover holds until the new one decodes. */
    ResetScroll();
}

void StatusBar::ResolveSdFile() {
    /* SD file: show file name without path/extension. */
    size_t length = std::strlen(path_buffer);
    NullLastDot(path_buffer);
    this->m_last_id[0] = '\0';
    ClearArt();   // SD file: no jelly art
    /* Last path component = filename. Scan back for '/' with a guarded index: a
       plain `for (size_t i = length; i >= 0; i--)` is always-true and underflows
       to SIZE_MAX (OOB read) when the path has no '/'. */
    const char *name = path_buffer;
    for (size_t i = length; i-- > 0; ) {
        if (path_buffer[i] == '/') { name = path_buffer + i + 1; break; }
    }
    if (this->m_current_track != name) {
        this->m_current_track = name;
        ResetScroll();
    }
}

void StatusBar::HandleNoItem() {
    /* No current item right now. Two cases:
       - transient GAP between tracks (skip / track end): the sysmodule is swapping
         streams. Keep the OUTGOING track + its art on screen.
       - genuinely STOPPED: blank everything. */
    u32 qsize = 0;
    const bool have_q  = R_SUCCEEDED(tuneGetPlaylistSize(&qsize)) && qsize > 0;
    const bool loading = this->m_playing || have_q;
    const bool holding = loading && this->m_last_id[0] != '\0';   // outgoing track to keep
    const UiPhase phase = loading ? UiPhase::Loading : UiPhase::Stopped;
    if (phase != this->m_phase) {
        ovl_log::Line("ui: %s (playing=%d qsize=%u)", holding ? "GAP-hold-art" : (loading ? "LOADING" : "stopped"),
                      (int)this->m_playing, qsize);
        this->m_phase = phase;
    }
    if (holding)
        return;   // keep prior art/title/last_id; RefreshArt() holds the outgoing art

    this->m_current_track = loading ? "Loading..." : "Stopped!";
    this->m_stats = {};
    this->m_last_id[0] = '\0';
    ClearArt();        // release pins
    ResetScroll();
}

void StatusBar::RefreshArt() {
    /* Drive the loader with the current track and borrow the art to draw: the
       current track's cover once decoded, else the previously-shown cover held
       across the swap (no grey flash). Only two images stay resident. */
    if (!this->m_last_id[0])
        return;   // nothing playing: ResolveSdFile/HandleNoItem already blanked m_art
    art_loader::Track(this->m_last_id, ART_PX);
    art_loader::Borrow(&this->m_art, &this->m_art_w, &this->m_art_h);
}

void StatusBar::ResetScroll() {
    this->m_text_width = 0;
    this->m_scroll_offset = 0;
    this->m_counter = 0;
}

void StatusBar::ClearArt() {
    this->m_art = nullptr; this->m_art_w = this->m_art_h = 0;   // borrowed; never free
    art_loader::Clear();
}

void StatusBar::CycleRepeat() {
    this->m_repeat = static_cast<TuneRepeatMode>((this->m_repeat + 1) % TuneRepeatMode_Count);
    config::set_repeat(this->m_repeat);
    tuneSetRepeatMode(this->m_repeat);
}

void StatusBar::CycleShuffle() {
    this->m_shuffle = static_cast<TuneShuffleMode>((this->m_shuffle + 1) % TuneShuffleMode_Count);
    config::set_shuffle(this->m_shuffle);
    tuneSetShuffleMode(this->m_shuffle);
}

void StatusBar::CyclePlay() {
    if (this->m_playing) {
        ovl_log::Line("play: pause");
        tunePause();
        return;
    }

    /* If the queue is empty, hitting play should still DO something: start a
       shuffle of the whole Jellyfin library (matches user expectation that the
       play button always plays). */
    u32 count = 0;
    if (R_FAILED(tuneGetPlaylistSize(&count)) || count == 0) {
        ovl_log::Line("play: queue empty -> shuffle all (BLOCKING fetch on UI thread)");
        constexpr int MAX_TRACKS = 300;
        std::vector<jelly_ovl::Track> tr(MAX_TRACKS);
        const u64 t0 = armGetSystemTick();
        int n = jelly_ovl::GetShuffleAll(tr.data(), MAX_TRACKS);
        const u64 dt = armTicksToNs(armGetSystemTick() - t0) / 1'000'000ULL;
        ovl_log::Line("play: shuffle fetch -> %d tracks in %llums", n, (unsigned long long)dt);
        if (n > 0) {
            tuneClearQueue();
            char path[80];
            for (int i = 0; i < n; i++) {
                std::snprintf(path, sizeof path, "jelly://%s/%s", tr[i].fmt, tr[i].id);
                tuneEnqueue(path, TuneEnqueueType_Back);
            }
            tuneSelect(0);
            tunePlay();
            ovl_log::Line("play: enqueued %d, select 0, play", n);
        } else {
            ovl_log::Line("play: shuffle returned nothing (server unreachable?)");
        }
        return;
    }

    ovl_log::Line("play: resume (qsize=%u)", count);
    tunePlay();
}

void StatusBar::Prev() {
    tunePrev();
}

void StatusBar::Next() {
    tuneNext();
}

void StatusBar::Forward() {
    u32 next = std::min(this->m_stats.current_frame + (this->m_stats.total_frames / 10), this->m_stats.total_frames);
    tuneSeek(next);
}

void StatusBar::Backward() {
    u32 next = std::max(s64(this->m_stats.current_frame) - s64(this->m_stats.total_frames / 10), s64(0));
    tuneSeek(next);
}

const AlphaSymbol &StatusBar::GetPlaybackSymbol() {
    return this->m_playing ? symbol::pause::symbol : symbol::play::symbol;
}
