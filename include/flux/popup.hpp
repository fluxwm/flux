#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;

    class Popup {
    public:
        Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup, wlr_scene_tree* root_tree);
        Popup(Compositor& compositor, wlr_xwayland_surface* xsurface);
        ~Popup();

        Popup(const Popup&) = delete;
        Popup& operator=(const Popup&) = delete;

        [[nodiscard]] wlr_xdg_popup* raw() const noexcept { return m_xdg_popup; }
        [[nodiscard]] wlr_xwayland_surface* xwayland_surface() const noexcept { return m_xwayland_surface; }
        [[nodiscard]] wlr_scene_tree* root_tree() const noexcept { return m_root_tree; }
        void update_position() noexcept;

    private:
        void unconstrain();
        void on_commit();
        void on_reposition();
        void on_destroy();

        void on_associate();
        void on_dissociate();
        void on_map();
        void on_unmap();
        void on_request_configure(wlr_xwayland_surface_configure_event* event);
        void on_set_override_redirect();

        Compositor& m_compositor;
        wlr_xdg_popup* m_xdg_popup = nullptr;
        wlr_xwayland_surface* m_xwayland_surface = nullptr;
        wlr_scene_tree* m_root_tree = nullptr;
        wlr_scene_tree* m_wrapper_tree = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_surface_listeners;
    };

}
