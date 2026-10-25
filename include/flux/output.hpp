#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Server;

    class Output {
    public:
        Output(Server& server, wlr_output* wlr_output);
        ~Output() = default;

        Output(const Output&) = delete;
        Output& operator=(const Output&) = delete;

        [[nodiscard]] wlr_output* raw() const noexcept { return m_wlr_output; }

    private:
        void on_frame();
        void on_request_state(const wlr_output_event_request_state* event);
        void on_destroy();

        Server& m_server;
        wlr_output* m_wlr_output = nullptr;

        ListenerGroup m_listeners;
    };

}
