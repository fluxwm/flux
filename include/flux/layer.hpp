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
        [[nodiscard]] wlr_scene_layer_surface_v1* scene_layer() const noexcept { return m_scene_layer; }
        [[nodiscard]] Output* output() const noexcept;

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
