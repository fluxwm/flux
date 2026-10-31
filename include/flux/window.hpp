#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <string>
#include <string_view>

namespace flux {

    class Compositor;
    class Workspace;
    class Output;

    class Window {
    public:
        Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel);
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

        [[nodiscard]] wlr_scene_tree* scene_tree() const noexcept { return m_scene_tree; }
        [[nodiscard]] wlr_surface* surface() const noexcept { return m_xdg_toplevel->base->surface; }
        [[nodiscard]] std::string_view title() const noexcept { return m_title; }
        [[nodiscard]] std::string_view app_id() const noexcept { return m_app_id; }
        [[nodiscard]] bool is_maximized() const noexcept { return m_maximized; }
        [[nodiscard]] bool is_fullscreen() const noexcept { return m_fullscreen; }
        [[nodiscard]] bool is_mapped() const noexcept { return surface()->mapped; }
        [[nodiscard]] Workspace* workspace() const noexcept { return m_workspace; }
        [[nodiscard]] Output* output() const noexcept { return m_output; }
        void set_output(Output* output) noexcept { m_output = output; }

        void set_activated(bool activated) noexcept { wlr_xdg_toplevel_set_activated(m_xdg_toplevel, activated); }
        void set_workspace(Workspace* ws) noexcept {
            if (m_workspace != ws) {
                m_workspace = ws;
                reparent_to_workspace();
            }
        }
        void reparent_to_workspace() noexcept;
        void set_geometry(const wlr_box& box) noexcept;
        void toggle_maximize();
        void set_fullscreen(bool fullscreen);
        void toggle_fullscreen() { set_fullscreen(!m_fullscreen); }
        void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration);
        void raise_to_top() noexcept { wlr_scene_node_raise_to_top(&m_scene_tree->node); }
        void lower_to_bottom() noexcept { wlr_scene_node_lower_to_bottom(&m_scene_tree->node); }
        void close() noexcept { wlr_xdg_toplevel_send_close(m_xdg_toplevel); }

    private:
        void on_map();
        void on_unmap();
        void on_commit();
        void on_destroy();
        void on_request_maximize();
        void on_request_fullscreen();
        void on_request_decoration_mode();
        void on_destroy_decoration();
        void on_set_title();
        void on_set_app_id();
        void update_foreign_toplevel() noexcept;

        Compositor& m_compositor;
        wlr_xdg_toplevel* m_xdg_toplevel = nullptr;
        wlr_xdg_toplevel_decoration_v1* m_decoration = nullptr;
        wlr_scene_tree* m_scene_tree = nullptr;
        bool m_maximized = false;
        bool m_fullscreen = false;
        Workspace* m_workspace = nullptr;
        Output* m_output = nullptr;
        std::string m_title;
        std::string m_app_id;
        wlr_ext_foreign_toplevel_handle_v1* m_ext_toplevel_handle = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_decoration_listeners;
    };

}
