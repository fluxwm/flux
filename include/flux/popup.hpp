#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;

    class Popup {
    public:
        Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup);
        ~Popup() = default;

        Popup(const Popup&) = delete;
        Popup& operator=(const Popup&) = delete;

        [[nodiscard]] wlr_xdg_popup* raw() const noexcept { return m_xdg_popup; }

    private:
        void on_commit();
        void on_destroy();

        Compositor& m_compositor;
        wlr_xdg_popup* m_xdg_popup = nullptr;

        ListenerGroup m_listeners;
    };

}
