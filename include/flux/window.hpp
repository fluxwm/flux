#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <string>
#include <string_view>

namespace flux {

    class Compositor;

    class Window {
    public:
        Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel);
        ~Window() = default;

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        [[nodiscard]] wlr_xdg_toplevel* raw() const noexcept { return m_xdg_toplevel; }
        [[nodiscard]] wlr_scene_tree* scene_tree() const noexcept { return m_scene_tree; }
        [[nodiscard]] wlr_surface* surface() const noexcept { return m_xdg_toplevel->base->surface; }
        [[nodiscard]] const wlr_box& geometry() const noexcept { return m_xdg_toplevel->base->geometry; }
        [[nodiscard]] int x() const noexcept { return m_scene_tree->node.x; }
        [[nodiscard]] int y() const noexcept { return m_scene_tree->node.y; }
        [[nodiscard]] std::string_view title() const noexcept { return m_title; }
        [[nodiscard]] std::string_view app_id() const noexcept { return m_app_id; }

        void set_activated(bool activated) noexcept { wlr_xdg_toplevel_set_activated(m_xdg_toplevel, activated); }
        void set_size(int width, int height) noexcept { wlr_xdg_toplevel_set_size(m_xdg_toplevel, width, height); }
        void set_position(int x, int y) noexcept { wlr_scene_node_set_position(&m_scene_tree->node, x, y); }
        void set_pending_position(int x, int y) noexcept {
            m_pending_x = x;
            m_pending_y = y;
            m_has_pending_position = true;
        }
        void apply_pending_position() noexcept {
            if (m_has_pending_position) {
                set_position(m_pending_x, m_pending_y);
                m_has_pending_position = false;
            }
        }
        void raise_to_top() noexcept { wlr_scene_node_raise_to_top(&m_scene_tree->node); }
        void close() noexcept { wlr_xdg_toplevel_send_close(m_xdg_toplevel); }

    private:
        void on_map();
        void on_unmap();
        void on_commit();
        void on_destroy();
        void on_request_move();
        void on_request_resize(wlr_xdg_toplevel_resize_event* event);
        void on_request_maximize();
        void on_request_fullscreen();
        void on_set_title();
        void on_set_app_id();

        Compositor& m_compositor;
        wlr_xdg_toplevel* m_xdg_toplevel = nullptr;
        wlr_scene_tree* m_scene_tree = nullptr;
        int m_pending_x = 0;
        int m_pending_y = 0;
        bool m_has_pending_position = false;
        std::string m_title;
        std::string m_app_id;

        ListenerGroup m_listeners;
    };

}
