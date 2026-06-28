#pragma once

#include "tune.h"
#include "symbol.hpp"

#include <tesla.hpp>

class StatusBar final : public tsl::elm::Element {
  private:
    enum class UiPhase { Stopped, Loading, Playing };

    bool m_playing;
    TuneRepeatMode m_repeat;
    TuneShuffleMode m_shuffle;
    TuneCurrentStats m_stats;

    float m_percentage;

    std::string_view m_current_track;
    char m_name_buf[160]{};   // resolved "Title - Artist" for jelly:// tracks
    char m_last_id[48]{};     // the current track id (one source of truth; "" when none)
    bool m_name_resolved = false;
    u8 *m_art = nullptr;      // BORROWED RGBA cover art (owned by art_loader; never free)
    int m_art_w = 0, m_art_h = 0;
    UiPhase m_phase = UiPhase::Playing;   // last logged phase (for transition logging)
    int m_focus = 2;          // 0=repeat 1=prev 2=play 3=next 4=shuffle 5=seek bar
    std::string m_scroll_text;
    u32 m_text_width;
    u32 m_scroll_offset;
    bool m_truncated;
    u8 m_counter;

    bool m_touched = false;

  public:
    StatusBar();
    ~StatusBar();

    tsl::elm::Element *requestFocus(tsl::elm::Element *oldFocus, tsl::FocusDirection direction) override;
    void draw(tsl::gfx::Renderer *renderer) override;
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override;
    bool onClick(u64 keys) override;
    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override;

    void update();

    void CycleRepeat();
    void CyclePlay();
    void CycleShuffle();
    void Prev();
    void Next();
    void Backward();
    void Forward();

  public:
    static constexpr s32 ART_PX = 256;   // kept small so decodes fit the tight overlay heap
    static constexpr s32 HEIGHT = 450;

  private:
    ALWAYS_INLINE s32 CenterX()    { return this->getX() + this->getWidth() / 2; }
    ALWAYS_INLINE s32 ArtW()       { return this->m_art ? this->m_art_w : ART_PX; }
    ALWAYS_INLINE s32 ArtH()       { return this->m_art ? this->m_art_h : ART_PX; }
    ALWAYS_INLINE s32 ArtY()       { return this->getY() + 16; }
    ALWAYS_INLINE s32 ArtBottom()  { return ArtY() + ArtH(); }
    ALWAYS_INLINE s32 TitleY()     { return ArtBottom() + 32; }
    ALWAYS_INLINE s32 SeekY()      { return ArtBottom() + 66; }
    ALWAYS_INLINE s32 SeekBarX()   { return this->getX() + 15; }
    ALWAYS_INLINE u32 SeekBarLen() { return this->getWidth() - 30; }
    ALWAYS_INLINE s32 ControlsY()  { return ArtBottom() + 128; }
    ALWAYS_INLINE s32 CtlHalf()    { return ArtW() / 2 - 8; }

    ALWAYS_INLINE s32 GetRepeatX()    { return CenterX() - CtlHalf(); }
    ALWAYS_INLINE s32 GetRepeatY()    { return ControlsY(); }
    ALWAYS_INLINE s32 GetShuffleX()   { return CenterX() + CtlHalf(); }
    ALWAYS_INLINE s32 GetShuffleY()   { return ControlsY(); }
    ALWAYS_INLINE s32 GetPrevX()      { return CenterX() - CtlHalf() / 2; }
    ALWAYS_INLINE s32 GetPrevY()      { return ControlsY(); }
    ALWAYS_INLINE s32 GetPlayStateX() { return CenterX(); }
    ALWAYS_INLINE s32 GetPlayStateY() { return ControlsY(); }
    ALWAYS_INLINE s32 GetNextX()      { return CenterX() + CtlHalf() / 2; }
    ALWAYS_INLINE s32 GetNextY()      { return ControlsY(); }
    ALWAYS_INLINE s32 FocusX(int i) {
        switch (i) { case 0: return GetRepeatX(); case 1: return GetPrevX();
                     case 3: return GetNextX();   case 4: return GetShuffleX();
                     default: return GetPlayStateX(); }
    }
    void ActivateControl(int index);
    void SeekToPercentage(float pct);
    void SeekBySeconds(int delta_sec);
    const AlphaSymbol &GetPlaybackSymbol();

    // update() helpers — one branch of "what is the current item?" each.
    void ResolveJellyTrack();   // jelly:// current track -> resolve name, drive art
    void ResolveSdFile();       // local SD-file current track -> filename
    void HandleNoItem();        // no current item: transient gap-hold vs stopped
    void RefreshArt();          // borrow the current/held art from art_loader
    void ResetScroll();         // reset the title scroll-animation state
    void ClearArt();            // drop the borrowed cover + release art_loader pins
};
