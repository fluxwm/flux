#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <vector>

namespace flux {

    class Server;

    class Keyboard {
    public:
        Keyboard(Server& server, wlr_input_device* device);
        ~Keyboard() = default;

        Keyboard(const Keyboard&) = delete;
        Keyboard& operator=(const Keyboard&) = delete;

        [[nodiscard]] wlr_keyboard* raw() const noexcept { return m_wlr_keyboard; }

    private:
        void on_modifiers();
        void on_key(wlr_keyboard_key_event* event);
        void on_destroy();

        Server& m_server;
        wlr_keyboard* m_wlr_keyboard = nullptr;
        std::vector<uint32_t> m_consumed_keycodes;

        ListenerGroup m_listeners;
    };

}
