#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;
    class Output;

    class Popup {
    public:
        Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup);
        ~Popup();

        Popup(const Popup&) = delete;
        Popup& operator=(const Popup&) = delete;

        [[nodiscard]] wlr_xdg_popup* raw() const noexcept { return m_xdg_popup; }
        [[nodiscard]] wlr_scene_tree* root_tree() const noexcept { return m_root_tree; }
        void update_position() noexcept;

    private:
        void unconstrain();
        void on_commit();
        void on_reposition();
        void on_destroy();

        Compositor& m_compositor;
        wlr_xdg_popup* m_xdg_popup = nullptr;
        wlr_scene_tree* m_root_tree = nullptr;
        wlr_scene_tree* m_wrapper_tree = nullptr;
        Output* m_output = nullptr;

        ListenerGroup m_listeners;
    };

}
