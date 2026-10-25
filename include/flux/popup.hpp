#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Server;

    class Popup {
    public:
        Popup(Server& server, wlr_xdg_popup* xdg_popup);
        ~Popup() = default;

        Popup(const Popup&) = delete;
        Popup& operator=(const Popup&) = delete;

        [[nodiscard]] wlr_xdg_popup* raw() const noexcept { return m_xdg_popup; }

    private:
        void on_commit();
        void on_destroy();

        Server& m_server;
        wlr_xdg_popup* m_xdg_popup = nullptr;

        ListenerGroup m_listeners;
    };

}
