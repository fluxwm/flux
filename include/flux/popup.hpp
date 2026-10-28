#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;

    class Popup {
    public:
        Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup);
        ~Popup();

        Popup(const Popup&) = delete;
        Popup& operator=(const Popup&) = delete;

    private:
        void unconstrain();
        void on_commit();
        void on_reposition();
        void on_destroy();

        Compositor& m_compositor;
        wlr_xdg_popup* m_xdg_popup = nullptr;
        wlr_scene_tree* m_wrapper_tree = nullptr;

        ListenerGroup m_listeners;
    };

}
