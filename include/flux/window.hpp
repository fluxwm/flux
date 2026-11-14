#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <array>
#include <string>
#include <string_view>

namespace flux {

    class Compositor;
    class Workspace;
    class Output;

    class Window {
    public:
        Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel);
        Window(Compositor& compositor, wlr_xwayland_surface* xwayland_surface);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        [[nodiscard]] static Window* try_from(wlr_xdg_toplevel* toplevel) noexcept {
            if (!toplevel || !toplevel->base || !toplevel->base->data) {
                return nullptr;
            }
            auto* tree = static_cast<wlr_scene_tree*>(toplevel->base->data);
            return static_cast<Window*>(tree->node.data);
        }

        [[nodiscard]] static Window* try_from(wlr_xwayland_surface* xsurface) noexcept {
            if (!xsurface || xsurface->override_redirect || !xsurface->data) {
                return nullptr;
            }
            auto* tree = static_cast<wlr_scene_tree*>(xsurface->data);
            return static_cast<Window*>(tree->node.data);
        }

        [[nodiscard]] static Window* try_from(wlr_surface* surface) noexcept {
            if (!surface) {
                return nullptr;
            }
            wlr_surface* root = wlr_surface_get_root_surface(surface);
            if (!root) {
                return nullptr;
            }
            if (auto* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root)) {
                return try_from(toplevel);
            }
            if (auto* xsurface = wlr_xwayland_surface_try_from_wlr_surface(root)) {
                return try_from(xsurface);
            }
            return nullptr;
        }

        [[nodiscard]] wlr_scene_tree* scene_tree() const noexcept { return m_scene_tree; }
        [[nodiscard]] wlr_surface* surface() const noexcept {
            return m_xdg_toplevel ? m_xdg_toplevel->base->surface
                                  : (m_xwayland_surface ? m_xwayland_surface->surface : nullptr);
        }
        [[nodiscard]] wlr_xwayland_surface* xwayland_surface() const noexcept { return m_xwayland_surface; }
        [[nodiscard]] std::string_view title() const noexcept { return m_title; }
        [[nodiscard]] std::string_view app_id() const noexcept { return m_app_id; }
        [[nodiscard]] bool is_maximized() const noexcept { return m_maximized; }
        [[nodiscard]] bool is_fullscreen() const noexcept { return m_fullscreen; }
        [[nodiscard]] bool is_floating() const noexcept { return !m_fullscreen && !m_maximized && m_floating; }
        [[nodiscard]] bool is_tiled() const noexcept { return !m_fullscreen && !m_maximized && !m_floating; }
        [[nodiscard]] bool is_placed() const noexcept { return m_placed; }
        [[nodiscard]] bool is_mapped() const noexcept {
            const auto* s = surface();
            return s && s->mapped;
        }
        [[nodiscard]] wlr_box geometry_box() const noexcept;
        [[nodiscard]] Workspace* workspace() const noexcept { return m_workspace; }
        [[nodiscard]] uint32_t workspace_id() const noexcept { return m_workspace_id; }
        [[nodiscard]] Output* output() const noexcept;
        void set_output(Output* output) noexcept;
        void set_placed(bool placed) noexcept { m_placed = placed; }

        void set_activated(bool activated) noexcept;
        void update_borders() noexcept;
        void set_workspace(Workspace* ws) noexcept;
        void reparent_to_workspace() noexcept;
        void set_position(int x, int y) noexcept;
        void set_geometry(const wlr_box& box) noexcept;
        void center(const wlr_box& area) noexcept;
        void toggle_maximize();
        void set_fullscreen(bool fullscreen);
        void toggle_fullscreen() { set_fullscreen(!m_fullscreen); }
        void set_floating(bool floating);
        void toggle_floating();
        void start_grab(uint32_t edges);
        void update_grab(int dx, int dy);
        void end_grab() noexcept;
        void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration);
        void raise_to_top() noexcept;
        void lower_to_bottom() noexcept;
        void close() noexcept {
            if (m_xdg_toplevel) {
                wlr_xdg_toplevel_send_close(m_xdg_toplevel);
            } else if (m_xwayland_surface) {
                wlr_xwayland_surface_close(m_xwayland_surface);
            }
        }

    private:
        [[nodiscard]] bool wants_floating() const noexcept;
        void on_associate();
        void on_dissociate();
        void on_map();
        void on_unmap();
        void on_commit();
        void on_destroy();
        void on_request_configure(wlr_xwayland_surface_configure_event* event);
        void on_request_activate();
        void on_request_maximize();
        void on_request_fullscreen();
        void on_request_decoration_mode();
        void on_destroy_decoration();
        void on_set_title();
        void on_set_app_id();
        void on_set_parent();
        void on_set_hints();
        void on_set_override_redirect();
        void restore_floating_box() noexcept;
        bool move_scene_tree(int x, int y) noexcept;
        void update_foreign_toplevel() noexcept;

        Compositor& m_compositor;
        wlr_xdg_toplevel* m_xdg_toplevel = nullptr;
        wlr_xwayland_surface* m_xwayland_surface = nullptr;
        wlr_xdg_toplevel_decoration_v1* m_decoration = nullptr;
        wlr_scene_tree* m_scene_tree = nullptr;
        wlr_scene_tree* m_subsurface_tree = nullptr;
        bool m_maximized = false;
        bool m_fullscreen = false;
        bool m_floating = false;
        bool m_floating_override = false;
        bool m_placed = false;
        bool m_resizing = false;
        bool m_activated = false;
        uint32_t m_resize_edges = WLR_EDGE_NONE;
        wlr_box m_grab_box{};
        wlr_box m_floating_box{};
        Workspace* m_workspace = nullptr;
        uint32_t m_workspace_id = 1;
        std::string m_title;
        std::string m_app_id;
        wlr_ext_foreign_toplevel_handle_v1* m_ext_toplevel_handle = nullptr;
        std::array<wlr_scene_rect*, 4> m_borders{};

        ListenerGroup m_listeners;
        ListenerGroup m_surface_listeners;
        ListenerGroup m_decoration_listeners;
    };

}
