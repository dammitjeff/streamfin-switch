#include "gui_settings.hpp"

#include "gui_keyboard.hpp"
#include "jelly_ovl.hpp"
#include "config/config.hpp"

#include <cstring>
#include <cstdio>
#include <functional>

namespace {

    // A tappable text-field-looking box that opens the keyboard on select.
    class ServerField final : public tsl::elm::Element {
        const char *m_server;
        std::function<void()> m_on_edit;

      public:
        ServerField(const char *server, std::function<void()> on_edit)
            : m_server(server), m_on_edit(std::move(on_edit)) {}

        tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }

        bool onClick(u64 keys) override {
            if (keys & HidNpadButton_A) { if (m_on_edit) m_on_edit(); return true; }
            return false;
        }
        bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
            if (e == tsl::elm::TouchEvent::Release && this->inBounds(cx, cy)) {
                if (m_on_edit) m_on_edit();
                return true;
            }
            return false;
        }

        void draw(tsl::gfx::Renderer *r) override {
            const s32 x = getX(), y = getY(), w = getWidth(), h = getHeight();
            const s32 bx = x + 20, bw = w - 40, bh = 46, by = y + (h - bh) / 2;
            r->drawString("Server URL", false, bx + 2, by - 8, 14, r->a(tsl::style::color::ColorDescription));
            r->drawRect(bx, by, bw, bh, r->a(tsl::style::color::ColorFrame));
            const char *txt = (m_server && m_server[0]) ? m_server : "Enter server (host:port)...";
            r->drawString(txt, false, bx + 16, by + bh / 2 + 7, 20, r->a(tsl::style::color::ColorText));
        }

        void layout(u16, u16, u16, u16) override {
            this->setBoundaries(this->getX(), this->getY(), this->getWidth(), this->getHeight());
        }
    };

}

SettingsGui::SettingsGui() {
    // Empty until the user types their server — the input field shows a
    // "Enter server (host:port)..." placeholder when blank.
    config::get_jelly_server(m_server, sizeof m_server);

    char tok[8] = {};
    config::get_jelly_token(tok, sizeof tok);
    m_status = tok[0] ? "Signed in" : "Not signed in";
}

tsl::elm::Element *SettingsGui::createUI() {
    auto frame = new tsl::elm::OverlayFrame("Streamfin", "Settings");
    auto list  = new tsl::elm::List();

    /* Server URL input box — opens the keyboard on select. */
    list->addItem(new ServerField(m_server, [this]() {
        tsl::changeTo<KeyboardGui>(std::string("Server URL (host:port)"), std::string(this->m_server),
            [this](const std::string &result) {
                std::snprintf(this->m_server, sizeof this->m_server, "%s", result.c_str());
                config::set_jelly_server(this->m_server);
                jelly_ovl::ReloadConfig();
            });
    }), 78);

    /* Status + Quick Connect code (server now lives in the field above). */
    list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
        r->drawString(this->m_status.c_str(), false, x + 22, y + 30, 19, r->a(tsl::style::color::ColorHighlight));
        if (this->m_qc_active) {
            char line[64];
            std::snprintf(line, sizeof line, "Quick Connect code:  %s", this->m_code);
            r->drawString(line, false, x + 22, y + 64, 23, r->a(tsl::style::color::ColorText));
            r->drawString("Approve it in Jellyfin: avatar -> Quick Connect", false,
                          x + 22, y + 90, 14, r->a(tsl::style::color::ColorDescription));
        }
    }), 120);

    auto qc = new tsl::elm::ListItem("Quick Connect (sign in)");
    qc->setClickListener([this](u64 keys) {
        if (keys & HidNpadButton_A) {
            if (this->m_server[0] == '\0') {
                this->m_status = "Enter a server URL first";
                return true;
            }
            if (jelly_ovl::QcBegin(this->m_code, sizeof this->m_code, this->m_secret, sizeof this->m_secret)) {
                this->m_qc_active = true;
                this->m_status = "Waiting for approval...";
            } else {
                this->m_qc_active = false;
                this->m_status = "Couldn't reach server - check the URL";
            }
            return true;
        }
        return false;
    });
    list->addItem(qc);

    auto out = new tsl::elm::ListItem("Sign Out");
    out->setClickListener([this](u64 keys) {
        if (keys & HidNpadButton_A) {
            config::set_jelly_token("");
            config::set_jelly_userid("");
            jelly_ovl::ReloadConfig();
            this->m_qc_active = false;
            this->m_status = "Signed out";
            return true;
        }
        return false;
    });
    list->addItem(out);

    frame->setContent(list);
    return frame;
}

void SettingsGui::update() {
    if (!m_qc_active) return;
    if (++m_tick < 40) return;   // poll roughly once a second
    m_tick = 0;

    char token[160] = {}, uid[80] = {};
    int r = jelly_ovl::QcPoll(m_secret, token, sizeof token, uid, sizeof uid);
    if (r == 1) {
        m_qc_active = false;
        m_status = "Signed in!";
    } else if (r < 0) {
        m_qc_active = false;
        m_status = "Quick Connect failed / expired";
    }
}
