#include "flux/layer.hpp"
#include "flux/compositor.hpp"
#include "flux/output.hpp"

namespace flux {

    Layer::Layer(Compositor& compositor, wlr_layer_surface_v1* layer_surface)
        : m_compositor(compositor), m_layer_surface(layer_surface) {
        wlr_scene_tree* parent = m_compositor.layer_tree(layer_surface->pending.layer);
        m_scene_layer = wlr_scene_layer_surface_v1_create(parent, layer_surface);
        if (!m_scene_layer) {
            wlr_layer_surface_v1_destroy(layer_surface);
            m_layer_surface = nullptr;
            return;
        }

        layer_surface->data = this;

        m_listeners.listen(&layer_surface->surface->events.commit, this, &Layer::on_commit);
        m_listeners.listen(&layer_surface->surface->events.map, this, &Layer::on_map);
        m_listeners.listen(&layer_surface->surface->events.unmap, this, &Layer::on_unmap);
        m_listeners.listen(&layer_surface->events.destroy, this, &Layer::on_destroy);
        m_listeners.listen(&layer_surface->events.new_popup, this, &Layer::on_new_popup);
    }

    Layer::~Layer() {
        if (m_layer_surface) {
            m_layer_surface->data = nullptr;
        }
    }

    Output* Layer::output() const noexcept {
        return m_layer_surface ? m_compositor.output_from_wlr(m_layer_surface->output) : nullptr;
    }

    void Layer::on_commit() {
        if (!m_layer_surface) {
            return;
        }

        if (m_layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
            wlr_scene_tree* new_parent = m_compositor.layer_tree(m_layer_surface->current.layer);
            wlr_scene_node_reparent(&m_scene_layer->tree->node, new_parent);
        }

        constexpr uint32_t arrange_mask = WLR_LAYER_SURFACE_V1_STATE_DESIRED_SIZE | WLR_LAYER_SURFACE_V1_STATE_ANCHOR |
                                          WLR_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE |
                                          WLR_LAYER_SURFACE_V1_STATE_MARGIN |
                                          WLR_LAYER_SURFACE_V1_STATE_EXCLUSIVE_EDGE | WLR_LAYER_SURFACE_V1_STATE_LAYER;

        if (m_layer_surface->initial_commit || (m_layer_surface->current.committed & arrange_mask)) {
            if (Output* out = output()) {
                m_compositor.arrange_layers(out);
            }
        }

        if (m_layer_surface->current.committed &
            (WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY | WLR_LAYER_SURFACE_V1_STATE_LAYER)) {
            m_compositor.refocus();
        }
    }

    void Layer::on_map() {
        if (!m_layer_surface) {
            return;
        }

        wlr_log(WLR_DEBUG, "layer surface mapped: namespace '%s'",
                m_layer_surface->_namespace ? m_layer_surface->_namespace : "");

        if (Output* out = output()) {
            m_compositor.arrange_layers(out);
        }

        if (m_layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
            auto* excl = m_compositor.exclusive_layer();
            if (!excl || excl == this) {
                m_compositor.focus_layer(this);
            }
        }
    }

    void Layer::on_unmap() {
        if (m_layer_surface) {
            wlr_log(WLR_DEBUG, "layer surface unmapped: namespace '%s'",
                    m_layer_surface->_namespace ? m_layer_surface->_namespace : "");
        }
        if (Output* out = output()) {
            m_compositor.arrange_layers(out);
        }
        m_compositor.refocus();
    }

    void Layer::on_destroy() {
        m_compositor.remove_layer(this);
    }

    void Layer::on_new_popup(wlr_xdg_popup* popup) {
        m_compositor.create_popup(popup);
    }

}
