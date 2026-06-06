#include "gui_playlist.hpp"

#include "elm_overlayframe.hpp"
#include "config/config.hpp"
#include "jelly_ovl.hpp"
#include "meta_cache.hpp"
#include "tune.h"

#include <algorithm>

namespace {

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

    class ButtonListItem final : public tsl::elm::ListItem {
      public:
        template <typename Text, typename Value>
        ButtonListItem(Text &text, Value &value) : ListItem(std::forward<Text>(text), std::forward<Value>(value)) {}

        bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
            if (event == tsl::elm::TouchEvent::Touch)
                this->m_touched = this->inBounds(currX, currY);

            if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
                this->m_touched = false;

                if (Element::getInputMode() == tsl::InputMode::Touch) {
                    bool handled = false;
                    if (currX > this->getLeftBound() && currX < (this->getRightBound() - this->getHeight()) && currY > this->getTopBound() && currY < this->getBottomBound())
                        handled = this->onClick(HidNpadButton_A);

                    if (currX > (this->getRightBound() - this->getHeight()) && currX < this->getRightBound() && currY > this->getTopBound() && currY < this->getBottomBound())
                        handled = this->onClick(HidNpadButton_Y);

                    this->m_clickAnimationProgress = 0;
                    return handled;
                }
            }

            return false;
        }
    };

    ButtonListItem* g_focus_item;

}

PlaylistGui::PlaylistGui() {
    g_focus_item = nullptr;
    m_list = new tsl::elm::List();

    u32 count;
    Result rc = tuneGetPlaylistSize(&count);
    if (R_FAILED(rc)) {
        char result_buffer[0x10];
        std::snprintf(result_buffer, 0x10, "2%03X-%04X", R_MODULE(rc), R_DESCRIPTION(rc));
        this->m_list->addItem(new tsl::elm::ListItem("something went wrong :/"));
        this->m_list->addItem(new tsl::elm::ListItem(result_buffer));
        return;
    }

    if (count == 0) {
        m_list->addItem(new tsl::elm::ListItem("Playlist empty."));
        return;
    }

    char current_path[FS_MAX_PATH];
    TuneCurrentStats current_stats;
    rc = tuneGetCurrentQueueItem(current_path, sizeof(current_path), &current_stats);
    if (R_FAILED(rc)) {
        char result_buffer[0x10];
        std::snprintf(result_buffer, 0x10, "2%03X-%04X", R_MODULE(rc), R_DESCRIPTION(rc));
        this->m_list->addItem(new tsl::elm::ListItem("failed to get current item"));
        this->m_list->addItem(new tsl::elm::ListItem(result_buffer));
        return;
    }

    m_list->addItem(new tsl::elm::CategoryHeader("\uE0E2  To remove all      \uE0E7  Play on start up", true));

    char path[FS_MAX_PATH];
    for (u32 i = 0; i < count; i++) {
        rc = tuneGetPlaylistItem(i, path, sizeof(path));
        if (R_FAILED(rc))
            break;

        bool found = false;
        if (!g_focus_item && !strcasecmp(current_path, path)) {
            found = true;
        }

        // Resolve a display name: Jellyfin tracks -> "Title - Artist" (via metadata),
        // SD files -> filename without path/extension.
        char display[256];
        bool needs_resolve = false;
        std::string resolve_id;
        if (!std::strncmp(path, "jelly://", 8)) {
            // Resolve "Title - Artist" via the background cache. Cache miss ->
            // show a placeholder now and fill it in from update() when it lands
            // (NEVER block the UI thread; a 297-track queue would freeze for ~30s).
            const char *id = std::strrchr(path, '/');
            id = id ? id + 1 : path + 8;
            if (!(meta_cache::Get(id, display, sizeof display) && display[0])) {
                std::snprintf(display, sizeof display, "Loading...");
                needs_resolve = true;
                resolve_id = id;
            }
        } else {
            char *p = path;
            size_t length = std::strlen(p);
            NullLastDot(p);
            for (size_t i = length; i + 1 > 0; i--) {
                if (p[i] == '/') { p = p + i + 1; break; }
            }
            std::snprintf(display, sizeof display, "%s", p);
        }
        char *str = display;
        auto item = new ButtonListItem(str, "\uE098");
        if (needs_resolve) this->m_pending_names.emplace_back(item, resolve_id);
        item->setClickListener([this, item](u64 keys) -> bool {
            // adjust index for above CategoryHeader.
            const auto index = this->m_list->getIndexInList(item);
            const auto tune_index = index - 1;

            if (keys & HidNpadButton_A) {
                tuneSelect(tune_index);
                return true;
            }
            else if (keys & HidNpadButton_Y) {
                if (R_SUCCEEDED(tuneRemove(tune_index))) {
                    // Drop this row from the pending-name list before it's freed.
                    this->m_pending_names.erase(
                        std::remove_if(this->m_pending_names.begin(), this->m_pending_names.end(),
                                       [item](const std::pair<tsl::elm::ListItem *, std::string> &e) { return e.first == item; }),
                        this->m_pending_names.end());
                    this->removeFocus();
                    this->m_list->removeIndex(index);
                    auto element = this->m_list->getItemAtIndex(index + 1);
                    if (element != nullptr) {
                        this->requestFocus(element, tsl::FocusDirection::Down);
                        this->m_list->setFocusedIndex(index + 1);
                    } else if (index > 0) {
                        element = this->m_list->getItemAtIndex(index - 1);
                        this->requestFocus(element, tsl::FocusDirection::Up);
                        this->m_list->setFocusedIndex(index - 1);
                    }
                }
                return true;
            }
            else if (keys & HidNpadButton_X) {
                if (R_SUCCEEDED(tuneClearQueue())) {
                    this->m_pending_names.clear();   // all rows about to be freed
                    this->removeFocus();
                    this->m_list->clear();
                    m_list->addItem(new tsl::elm::ListItem("Playlist empty."));
                }
                return true;
            } else if (keys & HidNpadButton_ZR) {
                char path[FS_MAX_PATH];
                if (R_SUCCEEDED(tuneGetPlaylistItem(tune_index, path, sizeof(path)))) {
                    config::set_load_path(path);
                    // todo: toast
                    // m_frame->setToast("Set start up file", item->getText().c_str());
                }
                return true;
            }
            return false;
        });

        if (found) {
            g_focus_item = item;
        }

        m_list->addItem(item);
    }
}

tsl::elm::Element *PlaylistGui::createUI() {
    auto rootFrame = new StreamfinOverlayFrame();

    rootFrame->setContent(this->m_list);
    rootFrame->setDescription("\uE0E1  Back     \uE0E0  Play   \uE0E3  Remove");

    return rootFrame;
}

void PlaylistGui::update()  {
    /* Fill in any track names the background resolver has finished. */
    if (!this->m_pending_names.empty()) {
        char buf[256];
        for (size_t i = 0; i < this->m_pending_names.size(); ) {
            if (meta_cache::Get(this->m_pending_names[i].second.c_str(), buf, sizeof buf) && buf[0]) {
                this->m_pending_names[i].first->setText(buf);
                this->m_pending_names[i] = this->m_pending_names.back();
                this->m_pending_names.pop_back();
            } else {
                ++i;
            }
        }
    }

    if (g_focus_item) {
        // wait until its added to the list.
        const auto index = m_list->getIndexInList(g_focus_item);
        if (index >= 0) {
            this->removeFocus();
            this->requestFocus(g_focus_item, tsl::FocusDirection::Down);
            m_list->setFocusedIndex(index);
            g_focus_item = nullptr;
        }
    }
}
