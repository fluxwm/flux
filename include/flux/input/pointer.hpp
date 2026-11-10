#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {
    class Input;
    struct TouchpadConfig;
}

namespace flux::input {

    constexpr uint32_t btn_left = 0x110;
    constexpr uint32_t btn_right = 0x111;

    class Pointer {
    public:
        Pointer(Input& input, wlr_input_device* device, const TouchpadConfig& config);
        ~Pointer() = default;

        Pointer(const Pointer&) = delete;
        Pointer& operator=(const Pointer&) = delete;

        void apply_config(const TouchpadConfig& config);

    private:
        void on_destroy();

        Input& m_input;
        wlr_input_device* m_device = nullptr;
        ListenerGroup m_listeners;
    };

}
