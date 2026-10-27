#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {
    class Input;
}

namespace flux::input {

    class Pointer {
    public:
        Pointer(Input& input, wlr_input_device* device);
        ~Pointer() = default;

        Pointer(const Pointer&) = delete;
        Pointer& operator=(const Pointer&) = delete;

    private:
        void on_destroy();

        Input& m_input;
        ListenerGroup m_listeners;
    };

}
