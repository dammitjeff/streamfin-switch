#pragma once

#include <tesla.hpp>

// Streamfin browse screen ("the Picker"): Shuffle All, Playlists, Artists.
// Selecting a leaf resolves it to tracks and starts playback via the existing
// tune IPC (clear queue -> enqueue jelly:// each -> select 0 -> play).
class PickerGui final : public tsl::Gui {
  public:
    enum class Mode { Root, Playlists, Artists };

    explicit PickerGui(Mode mode = Mode::Root);
    tsl::elm::Element *createUI() override;

  private:
    Mode m_mode;
};
