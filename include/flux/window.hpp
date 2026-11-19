#pragma once

#include "flux/animation.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <array>
#include <string>
#include <string_view>

namespace flux {

    class Compositor;
    class Workspace;
    class Output;

    inline void layout_border_rects(const std::array<wlr_scene_rect*, 4>& borders, int w, int h, int bw,
                                    const std::array<float, 4>& color, float opacity = 1.0f) noexcept {
        const std::array<float, 4> c{
            color[0] * opacity,
            color[1] * opacity,
            color[2] * opacity,
            color[3] * opacity,
        };

        wlr_scene_node_set_position(&borders[0]->node, -bw, -bw);
        wlr_scene_rect_set_size(borders[0], w + (2 * bw), bw);

        wlr_scene_node_set_position(&borders[1]->node, -bw, h);
        wlr_scene_rect_set_size(borders[1], w + (2 * bw), bw);

        wlr_scene_node_set_position(&borders[2]->node, -bw, 0);
        wlr_scene_rect_set_size(borders[2], bw, h);

        wlr_scene_node_set_position(&borders[3]->node, w, 0);
        wlr_scene_rect_set_size(borders[3], bw, h);

        for (auto* border : borders) {
            wlr_scene_rect_set_color(border, c.data());
            wlr_scene_node_set_enabled(&border->node, true);
        }
    }

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
        [[nodiscard]] bool is_pending_center() const noexcept { return m_pending_center_serial != 0; }
        [[nodiscard]] bool is_mapped() const noexcept {
            const auto* s = surface();
            return s && s->mapped;
        }
        [[nodiscard]] wlr_box geometry_box() const noexcept;
        [[nodiscard]] bool is_animating() const noexcept { return m_animating || m_opening; }
        [[nodiscard]] bool is_activated() const noexcept { return m_activated; }
        [[nodiscard]] bool has_visible_borders() const noexcept;
        [[nodiscard]] float tile_weight() const noexcept { return m_tile_weight; }
        void set_tile_weight(float weight) noexcept { m_tile_weight = weight; }
        void set_tiled_resizing(bool resizing) noexcept;
        [[nodiscard]] Workspace* workspace() const noexcept { return m_workspace; }
        [[nodiscard]] uint32_t workspace_id() const noexcept { return m_workspace_id; }
        [[nodiscard]] Output* output() const noexcept;
        void set_output(Output* output) noexcept;
        void set_placed(bool placed) noexcept {
            m_placed = placed;
            if (!placed) {
                m_has_initial_position = false;
            }
        }

        void snap_animation() noexcept;
        bool step_animation(const timespec& now) noexcept;

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
        void end_grab(bool commit = true) noexcept;
        void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration);
        void close() noexcept {
            if (m_xdg_toplevel) {
                wlr_xdg_toplevel_send_close(m_xdg_toplevel);
            } else if (m_xwayland_surface) {
                wlr_xwayland_surface_close(m_xwayland_surface);
            }
        }

    private:
        [[nodiscard]] bool wants_floating() const noexcept;
        [[nodiscard]] bool can_animate() const noexcept;
        void apply_position() noexcept;
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
        void save_floating_box() noexcept;
        void reset_placement_state() noexcept;
        void apply_open_transform() noexcept;
        void reset_open_transform() noexcept;
        void move_scene_tree(int x, int y) noexcept;
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
        bool m_has_initial_position = false;
        bool m_resizing = false;
        bool m_grabbed = false;
        bool m_animating = false;
        bool m_opening = false;
        bool m_activated = false;
        float m_tile_weight = 1.0f;
        uint32_t m_resize_edges = WLR_EDGE_NONE;
        uint32_t m_resize_serial = 0;
        uint32_t m_pending_center_serial = 0;
        uint32_t m_pending_move_serial = 0;
        wlr_box m_grab_box{};
        wlr_box m_floating_box{};
        int m_base_x = 0;
        int m_base_y = 0;
        int m_target_x = 0;
        int m_target_y = 0;
        Spring m_spring_x{};
        Spring m_spring_y{};
        Spring m_open_spring{1.0, {.stiffness = 900.0, .damping_ratio = 1.0}};
        timespec m_last_animation_time{};
        int m_committed_width = 0;
        int m_committed_height = 0;
        int m_border_width = 0;
        int m_border_height = 0;
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
