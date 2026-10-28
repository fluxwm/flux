#include "flux/popup.hpp"
#include "flux/compositor.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/util.hpp"

namespace flux {

    Popup::Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup) : m_compositor(compositor), m_xdg_popup(xdg_popup) {
        m_listeners.listen(&xdg_popup->events.destroy, this, &Popup::on_destroy);

        if (!xdg_popup->parent) {
            wlr_log(WLR_ERROR, "popup parent is null");
            return;
        }

        wlr_scene_tree* parent_tree = nullptr;
        wlr_surface* parent_root = wlr_surface_get_root_surface(xdg_popup->parent);
        if (wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(parent_root); parent && parent->data) {
            parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        } else if (wlr_layer_surface_v1* layer_parent = wlr_layer_surface_v1_try_from_wlr_surface(parent_root);
                   layer_parent && layer_parent->data) {
            auto* ls = static_cast<Layer*>(layer_parent->data);
            if (ls->scene_layer()) {
                parent_tree = ls->scene_layer()->tree;
            }
        }

        if (!parent_tree) {
            wlr_log(WLR_ERROR, "popup parent surface has no associated scene tree");
            return;
        }

        auto* scene_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
        xdg_popup->base->data = scene_tree;

        m_listeners.listen(&xdg_popup->base->surface->events.commit, this, &Popup::on_commit);
        m_listeners.listen(&xdg_popup->events.reposition, this, &Popup::on_reposition);
    }

    void Popup::unconstrain() {
        wlr_surface* root_surface = root_popup_parent(m_xdg_popup->base->surface);
        wlr_scene_tree* root_tree = nullptr;
        Output* output = nullptr;

        if (wlr_xdg_surface* toplevel = wlr_xdg_surface_try_from_wlr_surface(root_surface);
            toplevel && toplevel->data) {
            root_tree = static_cast<wlr_scene_tree*>(toplevel->data);
        } else if (wlr_layer_surface_v1* layer_raw = wlr_layer_surface_v1_try_from_wlr_surface(root_surface);
                   layer_raw && layer_raw->data) {
            auto* ls = static_cast<Layer*>(layer_raw->data);
            if (ls->scene_layer()) {
                root_tree = ls->scene_layer()->tree;
                output = ls->output();
            }
        }

        if (root_tree) {
            int root_lx = 0;
            int root_ly = 0;
            wlr_scene_node_coords(&root_tree->node, &root_lx, &root_ly);

            if (!output) {
                wlr_output* wlr_out = wlr_output_layout_output_at(m_compositor.output_layout(), root_lx, root_ly);
                output = m_compositor.output_from_wlr(wlr_out);
            }
            if (!output) {
                output = m_compositor.focused_output();
            }

            if (output) {
                const wlr_box output_box = output->layout_box();
                const wlr_box toplevel_space_box{
                    .x = output_box.x - root_lx,
                    .y = output_box.y - root_ly,
                    .width = output_box.width,
                    .height = output_box.height,
                };
                wlr_xdg_popup_unconstrain_from_box(m_xdg_popup, &toplevel_space_box);
                return;
            }
        }

        wlr_xdg_surface_schedule_configure(m_xdg_popup->base);
    }

    void Popup::on_commit() {
        if (m_xdg_popup->base->initial_commit) {
            unconstrain();
        }
    }

    void Popup::on_reposition() {
        unconstrain();
    }

    void Popup::on_destroy() {
        m_compositor.remove_popup(this);
    }

}
