#pragma once

#include "frost/listener.hpp"
#include "frost/window.hpp"

namespace frost::window {

    class XWayland final : public Window {
    public:
        XWayland(Compositor& compositor, wlr_xwayland_surface* surface);
        ~XWayland() override;

        XWayland(const XWayland&) = delete;
        XWayland& operator=(const XWayland&) = delete;

        [[nodiscard]] wlr_surface* surface() const noexcept override;
        [[nodiscard]] wlr_xwayland_surface* xwayland_surface() const noexcept override { return m_surface; }
        void apply_resize(int w, int h) noexcept override;
        void close() noexcept override;

    protected:
        [[nodiscard]] std::pair<int, int> preferred_size() const noexcept override;
        [[nodiscard]] wlr_scene_node* clip_target() const noexcept override {
            return m_subsurface_tree ? &m_subsurface_tree->node : nullptr;
        }
        void apply_activated(bool activated) noexcept override;
        void apply_fullscreen(bool fullscreen) noexcept override;
        void apply_maximized(bool maximized) noexcept override;
        void configure_position(int x, int y) noexcept override;
        [[nodiscard]] bool configure_geometry(const wlr_box& box) noexcept override;
        [[nodiscard]] Window* query_parent() const noexcept override;
        [[nodiscard]] bool wants_floating() const noexcept override;

    private:
        void on_associate();
        void on_dissociate();
        void on_map();
        void on_commit();
        void on_request_configure(wlr_xwayland_surface_configure_event* event);
        void on_request_activate();
        void on_request_maximize();
        void on_request_fullscreen();
        void on_set_title();
        void on_set_app_id();
        void update_floating();
        void on_set_override_redirect();

        wlr_xwayland_surface* m_surface = nullptr;
        wlr_scene_tree* m_subsurface_tree = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_surface_listeners;
    };

}
