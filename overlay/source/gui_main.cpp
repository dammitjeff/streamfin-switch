#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "elm_volume.hpp"
#include "gui_browser.hpp"
#include "gui_playlist.hpp"
#include "gui_keyboard.hpp"
#include "gui_settings.hpp"
#include "gui_picker.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"

namespace {
    constexpr const size_t num_steps = 20;
}

MainGui::MainGui() {
    m_status_bar    = new StatusBar();
}

tsl::elm::Element *MainGui::createUI() {
    auto frame = new StreamfinOverlayFrame();
    auto list  = new tsl::elm::List();

    u64 pid{}, tid{};
    pm::getCurrentPidTid(&pid, &tid);

    /* Current track. */
    list->addItem(this->m_status_bar, StatusBar::HEIGHT);

    /* Queue (current playback queue). */
    auto queue_button = new tsl::elm::ListItem("Queue");
    queue_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tsl::changeTo<PlaylistGui>();
            return true;
        }
        return false;
    });
    list->addItem(queue_button);

    /* Streamfin: browse the Jellyfin library (Shuffle All / Playlists / Artists)
       and start playback through the existing tune IPC. */
    auto browse_button = new tsl::elm::ListItem("Browse Jellyfin");
    browse_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tsl::changeTo<PickerGui>();
            return true;
        }
        return false;
    });
    list->addItem(browse_button);

    /* Settings / sign-in. */
    auto settings_button = new tsl::elm::ListItem("Settings");
    settings_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tsl::changeTo<SettingsGui>();
            return true;
        }
        return false;
    });
    list->addItem(settings_button);

    /* Get initial volume. */
    float tune_volume = 1.f;
    float title_volume = 1.f;
    float default_title_volume = 1.f;

    tuneGetVolume(&tune_volume);
    tuneGetTitleVolume(&title_volume);
    tuneGetDefaultTitleVolume(&default_title_volume);

    auto tune_volume_slider = new ElmVolume("\uE13C", "Music Volume", num_steps);
    tune_volume_slider->setProgress(tune_volume * num_steps);
    tune_volume_slider->setValueChangedListener([](u8 value){
        const float volume = float(value) / float(num_steps);
        tuneSetVolume(volume);
    });
    list->addItem(tune_volume_slider);

    /* Game Volume \u2014 how loud the running game/app is while music plays over it. */
    auto game_volume_slider = new ElmVolume("\uE13C", "Game Volume", num_steps);
    game_volume_slider->setProgress(default_title_volume * num_steps);
    game_volume_slider->setValueChangedListener([](u8 value){
        const float volume = float(value) / float(num_steps);
        tuneSetDefaultTitleVolume(volume);
    });
    list->addItem(game_volume_slider);

    auto exit_button = new tsl::elm::ListItem("Close Streamfin");
    exit_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tuneQuit();
            tsl::goBack();
            return true;
        }
        return false;
    });
    list->addItem(exit_button);

    frame->setContent(list);

    return frame;
}

void MainGui::update() {
    static u8 tick = 0;
    /* Update status 4 times per second. */
    if ((tick % 15) == 0)
        this->m_status_bar->update();
    tick++;
}
