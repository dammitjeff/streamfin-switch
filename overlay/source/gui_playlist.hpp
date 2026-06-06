#pragma once

#include <tesla.hpp>

#include <vector>
#include <string>
#include <utility>

class PlaylistGui final : public tsl::Gui {
  private:
    tsl::elm::List *m_list;
    // Rows whose "Title - Artist" is still being resolved in the background.
    // update() polls meta_cache and setText()s them as names arrive.
    std::vector<std::pair<tsl::elm::ListItem *, std::string>> m_pending_names;

  public:
    PlaylistGui();

    tsl::elm::Element *createUI() override;
    void update() override;
};
