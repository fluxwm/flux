#pragma once

#include "frost/wlroots.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

namespace frost {

    class Output;
    class Window;

    class Border {
    public:
        void init(wlr_scene_tree* parent) noexcept;
        void update(int w, int h, int bw, const std::array<float, 4>& color, float opacity = 1.0f,
                    const wlr_box* clip = nullptr) noexcept;
        void hide() noexcept;
        [[nodiscard]] bool is_enabled() const noexcept {
            return std::ranges::any_of(m_rects, [](const auto* r) { return r && r->node.enabled; });
        }

    private:
        std::array<wlr_scene_rect*, 4> m_rects{};
    };

    class Scene {
    public:
        Scene(wl_display* display, wlr_output_layout* output_layout);
        ~Scene();

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        void set_linux_dmabuf_v1(wlr_linux_dmabuf_v1* dmabuf) noexcept {
            if (m_scene && dmabuf) {
                wlr_scene_set_linux_dmabuf_v1(m_scene, dmabuf);
            }
        }

        void set_color_manager_v1(wlr_color_manager_v1* color_manager) noexcept {
            if (m_scene && color_manager) {
                wlr_scene_set_color_manager_v1(m_scene, color_manager);
            }
        }

        [[nodiscard]] wlr_scene_tree* windows() const noexcept { return m_scene_windows; }
        [[nodiscard]] wlr_scene_tree* fullscreen() const noexcept { return m_scene_fullscreen; }
        [[nodiscard]] wlr_scene_tree* popups() const noexcept { return m_scene_popups; }
        [[nodiscard]] wlr_scene_tree* window_popups() const noexcept { return m_scene_window_popups; }
        [[nodiscard]] wlr_scene_tree* drag_icons() const noexcept { return m_scene_drag_icons; }
        [[nodiscard]] wlr_scene_tree* lock_surfaces() const noexcept { return m_scene_lock_surfaces; }
        [[nodiscard]] wlr_scene_tree* layer_tree(zwlr_layer_shell_v1_layer layer) const noexcept;

        void set_session_lock_active(bool active);
        void update_lock_background(std::span<const std::unique_ptr<Output>> outputs);

        wlr_scene_output* create_output(wlr_output* output);
        void add_to_output_layout(wlr_output_layout_output* layout_output, wlr_scene_output* scene_output) noexcept;

        [[nodiscard]] wlr_surface* surface_at(double lx, double ly, double* sx, double* sy) const;
        [[nodiscard]] Window* window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) const;
        [[nodiscard]] bool surface_coords(wlr_surface* surface, int& lx, int& ly) const noexcept;

    private:
        [[nodiscard]] wlr_scene_node* interactive_node_at(double lx, double ly, double* sx, double* sy) const;

        wlr_scene* m_scene = nullptr;
        wlr_scene_tree* m_scene_background = nullptr;
        wlr_scene_tree* m_scene_bottom = nullptr;
        wlr_scene_tree* m_scene_windows = nullptr;
        wlr_scene_tree* m_scene_top = nullptr;
        wlr_scene_tree* m_scene_fullscreen = nullptr;
        wlr_scene_tree* m_scene_window_popups = nullptr;
        wlr_scene_tree* m_scene_overlay = nullptr;
        wlr_scene_tree* m_scene_popups = nullptr;
        wlr_scene_tree* m_scene_drag_icons = nullptr;
        wlr_scene_tree* m_scene_lock = nullptr;
        wlr_scene_tree* m_scene_lock_background = nullptr;
        wlr_scene_tree* m_scene_lock_surfaces = nullptr;
        wlr_scene_output_layout* m_scene_layout = nullptr;
    };

}
