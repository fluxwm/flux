#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <vector>

namespace flux {
    class Input;
    struct KeyboardConfig;
}

namespace flux::input {

    class Keyboard {
    public:
        Keyboard(Input& input, wlr_input_device* device, const KeyboardConfig& config);
        ~Keyboard() = default;

        Keyboard(const Keyboard&) = delete;
        Keyboard& operator=(const Keyboard&) = delete;

        [[nodiscard]] wlr_keyboard* raw() const noexcept { return m_wlr_keyboard; }

        void apply_config(const KeyboardConfig& config);

    private:
        void on_modifiers();
        void on_key(wlr_keyboard_key_event* event);
        void on_destroy();

        Input& m_input;
        wlr_keyboard* m_wlr_keyboard = nullptr;
        std::vector<uint32_t> m_consumed_keycodes;

        ListenerGroup m_listeners;
    };

}
