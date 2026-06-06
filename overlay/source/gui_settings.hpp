#pragma once

#include <tesla.hpp>
#include <string>

// Streamfin sign-in / settings: server URL (via on-screen keyboard), Quick
// Connect pairing, and sign out. Reads/writes the shared SD config.
class SettingsGui final : public tsl::Gui {
  public:
    SettingsGui();
    tsl::elm::Element *createUI() override;
    void update() override;   // polls Quick Connect while active

  private:
    char m_server[160] = {};
    std::string m_status;
    char m_code[16]    = {};
    char m_secret[160] = {};
    bool m_qc_active   = false;
    int  m_tick        = 0;
};
