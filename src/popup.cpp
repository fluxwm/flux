#include "flux/popup.hpp"
#include "flux/compositor.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/util.hpp"

namespace flux {

    Popup::Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup) : m_compositor(compositor), m_xdg_popup(xdg_popup) {
        m_listeners.listen(&xdg_popup->events.destroy, this, &Popup::on_destroy);

        wlr_surface* root_surface = root_popup_parent(xdg_popup->parent);
        if (wlr_xdg_surface* toplevel = wlr_xdg_surface_try_from_wlr_surface(root_surface);
            toplevel && toplevel->data) {
            m_root_tree = static_cast<wlr_scene_tree*>(toplevel->data);
        } else if (auto* ls = Layer::try_from(root_surface); ls && ls->scene_tree()) {
            m_root_tree = ls->scene_tree();
            m_output = ls->output();
        }

        if (!m_root_tree) {
            wlr_log(WLR_ERROR, "popup parent surface has no associated scene tree");
            return;
        }

        wlr_scene_tree* parent_tree = nullptr;
        wlr_surface* parent_surface = wlr_surface_get_root_surface(xdg_popup->parent);
        wlr_xdg_surface* parent_xdg = wlr_xdg_surface_try_from_wlr_surface(parent_surface);

        if (parent_xdg && parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && parent_xdg->data) {
            parent_tree = static_cast<wlr_scene_tree*>(parent_xdg->data);
        } else {
            wlr_scene_tree* target_layer = (parent_xdg && parent_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
                                               ? m_compositor.scene().window_popups()
                                               : m_compositor.scene().popups();
            m_wrapper_tree = wlr_scene_tree_create(target_layer);
            m_wrapper_tree->node.data = m_root_tree->node.data;
            parent_tree = m_wrapper_tree;
        }

        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

        m_listeners.listen(&xdg_popup->base->surface->events.commit, this, &Popup::on_commit);
        m_listeners.listen(&xdg_popup->events.reposition, this, &Popup::on_reposition);
    }

    Popup::~Popup() {
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
        }
    }

    void Popup::update_position() noexcept {
        if (m_wrapper_tree && m_root_tree) {
            int root_lx = 0;
            int root_ly = 0;
            wlr_scene_node_coords(&m_root_tree->node, &root_lx, &root_ly);
            wlr_scene_node_set_position(&m_wrapper_tree->node, root_lx, root_ly);
        }
    }

    void Popup::unconstrain() {
        int root_lx = 0;
        int root_ly = 0;
        wlr_scene_node_coords(&m_root_tree->node, &root_lx, &root_ly);

        if (m_wrapper_tree) {
            wlr_scene_node_set_position(&m_wrapper_tree->node, root_lx, root_ly);
        }

        Output* output = m_output;
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
