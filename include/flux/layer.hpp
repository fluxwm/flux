#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;
    class Output;

    class Layer {
    public:
        Layer(Compositor& compositor, wlr_layer_surface_v1* layer_surface);
        ~Layer();

        Layer(const Layer&) = delete;
        Layer& operator=(const Layer&) = delete;

        [[nodiscard]] wlr_layer_surface_v1* raw() const noexcept { return m_layer_surface; }
        [[nodiscard]] wlr_surface* surface() const noexcept { return m_layer_surface->surface; }
        [[nodiscard]] wlr_scene_layer_surface_v1* scene_layer() const noexcept { return m_scene_layer; }
        [[nodiscard]] wlr_scene_tree* scene_tree() const noexcept {
            return m_scene_layer ? m_scene_layer->tree : nullptr;
        }
        [[nodiscard]] Output* output() const noexcept;
        [[nodiscard]] bool is_mapped() const noexcept { return surface()->mapped; }
        [[nodiscard]] bool is_initialized() const noexcept { return m_layer_surface->initialized; }
        [[nodiscard]] bool is_exclusive() const noexcept {
            return m_layer_surface->current.keyboard_interactive ==
                   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
        }
        [[nodiscard]] bool is_focusable() const noexcept {
            return m_layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
        }
        [[nodiscard]] static Layer* try_from(wlr_surface* surface) noexcept {
            if (!surface) {
                return nullptr;
            }
            auto* raw = wlr_layer_surface_v1_try_from_wlr_surface(surface);
            return raw ? static_cast<Layer*>(raw->data) : nullptr;
        }
        [[nodiscard]] zwlr_layer_shell_v1_layer layer() const noexcept { return m_layer_surface->current.layer; }
        [[nodiscard]] int32_t exclusive_zone() const noexcept { return m_layer_surface->current.exclusive_zone; }

    private:
        void on_commit();
        void on_map();
        void on_unmap();
        void on_destroy();
        void on_new_popup(wlr_xdg_popup* popup);

        Compositor& m_compositor;
        wlr_layer_surface_v1* m_layer_surface = nullptr;
        wlr_scene_layer_surface_v1* m_scene_layer = nullptr;

        ListenerGroup m_listeners;
    };

}
