#include "gui_keyboard.hpp"

#include <cctype>

namespace {

    // Key rows. Special keys: SHIFT, DEL, SPACE, CANCEL, DONE.
    const std::vector<std::vector<std::string>> g_rows = {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
        {"a", "s", "d", "f", "g", "h", "j", "k", "l", ":"},
        {"z", "x", "c", "v", "b", "n", "m", ".", "/", "-"},
        {"↑", "_", "~", "@", "?", "=", "&", "%", "DEL"},
        {"CANCEL", "SPACE", "DONE"},
    };

    class KeyboardElement final : public tsl::elm::Element {
        std::string m_text;
        KeyboardGui::DoneCb m_on_done;
        int m_row = 1, m_col = 0;
        bool m_shift = false;

      public:
        KeyboardElement(const std::string &initial, KeyboardGui::DoneCb on_done)
            : m_text(initial), m_on_done(std::move(on_done)) {}

        tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }

        void clampCol() {
            const int n = (int)g_rows[m_row].size();
            if (m_col >= n) m_col = n - 1;
            if (m_col < 0) m_col = 0;
        }

        void activate(const std::string &key) {
            if (key == "↑") {
                m_shift = !m_shift;
            } else if (key == "DEL") {
                if (!m_text.empty()) m_text.pop_back();
            } else if (key == "SPACE") {
                m_text += ' ';
            } else if (key == "CANCEL") {
                tsl::goBack();
            } else if (key == "DONE") {
                if (m_on_done) m_on_done(m_text);
                tsl::goBack();
            } else {
                char c = key[0];
                if (m_shift && std::isalpha((unsigned char)c)) c = (char)std::toupper((unsigned char)c);
                m_text += c;
                m_shift = false;
            }
        }

        bool onClick(u64 keys) override {
            if (keys & HidNpadButton_AnyUp)    { if (m_row > 0) m_row--; clampCol(); return true; }
            if (keys & HidNpadButton_AnyDown)  { if (m_row < (int)g_rows.size() - 1) m_row++; clampCol(); return true; }
            if (keys & HidNpadButton_AnyLeft)  { if (m_col > 0) m_col--; return true; }
            if (keys & HidNpadButton_AnyRight) { if (m_col < (int)g_rows[m_row].size() - 1) m_col++; return true; }
            if (keys & HidNpadButton_A)        { activate(g_rows[m_row][m_col]); return true; }
            if (keys & HidNpadButton_X)        { if (!m_text.empty()) m_text.pop_back(); return true; }  // quick backspace
            return false;
        }

        void draw(tsl::gfx::Renderer *renderer) override {
            const s32 x = this->getX(), y = this->getY(), w = this->getWidth();

            // Input field
            const s32 field_y = y + 18;
            renderer->drawRect(x + 14, field_y, w - 28, 40, a(tsl::style::color::ColorFrame));
            const std::string shown = m_text.empty() ? "<empty>" : m_text;
            renderer->drawString(shown.c_str(), false, x + 24, field_y + 27, 22, a(tsl::style::color::ColorText));

            // Key grid
            const s32 grid_top = field_y + 64;
            const s32 row_h = 46;
            for (int r = 0; r < (int)g_rows.size(); r++) {
                const int n = (int)g_rows[r].size();
                const s32 cell_w = (w - 28) / 10;          // base cell width (10-col grid)
                const s32 row_w = (r >= 4) ? (w - 28) : (cell_w * n);  // special rows stretch
                const s32 sw = (r >= 4) ? (row_w / n) : cell_w;        // this row's cell width
                const s32 rx = x + 14 + ((r >= 4) ? 0 : (w - 28 - row_w) / 2);
                const s32 ry = grid_top + r * row_h;

                for (int c = 0; c < n; c++) {
                    const s32 kx = rx + c * sw;
                    const bool focused = (r == m_row && c == m_col);
                    const std::string &label = g_rows[r][c];

                    renderer->drawRect(kx + 2, ry + 2, sw - 4, row_h - 6,
                                       a(focused ? tsl::style::color::ColorHighlight
                                                 : tsl::style::color::ColorFrame));
                    std::string lbl = label;
                    if (m_shift && label.size() == 1 && std::isalpha((unsigned char)label[0]))
                        lbl[0] = (char)std::toupper((unsigned char)label[0]);
                    auto [tw, th] = renderer->drawString(lbl.c_str(), false, 0, 0, 18, tsl::style::color::ColorTransparent);
                    renderer->drawString(lbl.c_str(), false, kx + (sw - (s32)tw) / 2, ry + row_h / 2 + 4, 18,
                                         a(tsl::style::color::ColorText));
                }
            }
        }

        void layout(u16, u16, u16, u16) override {
            this->setBoundaries(this->getX(), this->getY(), this->getWidth(), this->getHeight());
        }
    };

}

KeyboardGui::KeyboardGui(const std::string &title, const std::string &initial, DoneCb on_done)
    : m_title(title), m_initial(initial), m_on_done(std::move(on_done)) {}

tsl::elm::Element *KeyboardGui::createUI() {
    auto frame = new tsl::elm::OverlayFrame(m_title, " type    backspace");
    frame->setContent(new KeyboardElement(m_initial, m_on_done));
    return frame;
}
