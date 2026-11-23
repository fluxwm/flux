#pragma once

#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

namespace frost {
    class Input;
    struct MouseConfig;
    struct TouchpadConfig;
}

namespace frost::input {

    constexpr uint32_t btn_left = 0x110;
    constexpr uint32_t btn_right = 0x111;

    class Pointer {
    public:
        Pointer(Input& input, wlr_input_device* device, const MouseConfig& mouse_config,
                const TouchpadConfig& touchpad_config);
        ~Pointer() = default;

        Pointer(const Pointer&) = delete;
        Pointer& operator=(const Pointer&) = delete;

        void apply_config(const MouseConfig& mouse_config, const TouchpadConfig& touchpad_config);

    private:
        void on_destroy();

        Input& m_input;
        wlr_input_device* m_device = nullptr;
        ListenerGroup m_listeners;
    };

}
