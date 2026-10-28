#pragma once

#include "flux/wlroots.hpp"

namespace flux {

    class Window;

    class Scene {
    public:
        Scene(wl_display* display, wlr_output_layout* output_layout);
        ~Scene();

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        [[nodiscard]] wlr_scene_tree* windows() const noexcept { return m_scene_windows; }
        [[nodiscard]] wlr_scene_tree* fullscreen() const noexcept { return m_scene_fullscreen; }
        [[nodiscard]] wlr_scene_tree* layer_tree(zwlr_layer_shell_v1_layer layer) const noexcept;

        wlr_scene_output* attach_output(wlr_output* output, wlr_output_layout_output* layout_output);

        [[nodiscard]] wlr_surface* surface_at(double lx, double ly, double* sx, double* sy) const;
        [[nodiscard]] Window* window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) const;

    private:
        [[nodiscard]] wlr_scene_node* surface_node_at(double lx, double ly, double* sx, double* sy,
                                                      wlr_surface** surface) const;

        wlr_scene* m_scene = nullptr;
        wlr_scene_tree* m_scene_background = nullptr;
        wlr_scene_tree* m_scene_bottom = nullptr;
        wlr_scene_tree* m_scene_windows = nullptr;
        wlr_scene_tree* m_scene_top = nullptr;
        wlr_scene_tree* m_scene_fullscreen = nullptr;
        wlr_scene_tree* m_scene_overlay = nullptr;
        wlr_scene_output_layout* m_scene_layout = nullptr;
    };

}
