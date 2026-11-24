#pragma once

#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

namespace frost {

    class Compositor;
    class Workspace;

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
        [[nodiscard]] bool is_visible_on(const wlr_box& output_box) const noexcept;
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
        void set_workspace(Workspace* workspace);
        void on_workspace_destroy();

        Compositor& m_compositor;
        wlr_xdg_popup* m_xdg_popup = nullptr;
        wlr_xwayland_surface* m_xwayland_surface = nullptr;
        wlr_scene_tree* m_root_tree = nullptr;
        wlr_scene_tree* m_wrapper_tree = nullptr;
        Workspace* m_workspace = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_surface_listeners;
        ListenerGroup m_workspace_listeners;
    };

}
