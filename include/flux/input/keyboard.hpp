#pragma once

#include "flux/config.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <vector>

namespace flux {
    class Input;
}

namespace flux::input {

    class Keyboard {
    public:
        Keyboard(Input& input, wlr_input_device* device, const KeyboardConfig& config);
        ~Keyboard();

        Keyboard(const Keyboard&) = delete;
        Keyboard& operator=(const Keyboard&) = delete;

        [[nodiscard]] wlr_keyboard* raw() const noexcept { return m_wlr_keyboard; }
        [[nodiscard]] bool is_consumed(uint32_t keycode) const noexcept;

        void apply_config(const KeyboardConfig& config);

    private:
        void on_modifiers();
        void on_key(wlr_keyboard_key_event* event);
        void on_keymap();
        void on_destroy();

        void start_repeat(uint32_t keycode, uint32_t modifiers, xkb_keysym_t sym);
        void stop_repeat();
        int on_repeat_timer();

        Input& m_input;
        wlr_keyboard* m_wlr_keyboard = nullptr;
        KeyboardConfig m_config{};
        bool m_is_virtual = false;
        bool m_numlock = false;
        std::vector<uint32_t> m_consumed_keycodes;

        wl_event_source* m_repeat_timer = nullptr;
        bool m_repeating = false;
        uint32_t m_repeat_keycode = 0;
        uint32_t m_repeat_modifiers = 0;
        xkb_keysym_t m_repeat_sym = XKB_KEY_NoSymbol;

        ListenerGroup m_listeners;
    };

}
