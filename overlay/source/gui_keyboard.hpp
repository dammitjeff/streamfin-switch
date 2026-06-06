#pragma once

#include <tesla.hpp>
#include <functional>
#include <string>
#include <vector>

// A self-contained on-screen QWERTY keyboard for the overlay (overlays run as
// AppletType_None, so the system swkbd is unavailable — we draw our own).
// d-pad moves the key cursor, A types, B backspaces, plus CANCEL/DONE keys.
// On DONE it invokes the callback with the typed string and pops back.
class KeyboardGui final : public tsl::Gui {
  public:
    using DoneCb = std::function<void(const std::string &)>;
    KeyboardGui(const std::string &title, const std::string &initial, DoneCb on_done);
    tsl::elm::Element *createUI() override;

  private:
    std::string m_title;
    std::string m_initial;
    DoneCb m_on_done;
};
