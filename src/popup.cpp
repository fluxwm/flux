#include "flux/popup.hpp"
#include "flux/compositor.hpp"

namespace flux {

    Popup::Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup) : m_compositor(compositor), m_xdg_popup(xdg_popup) {
        m_listeners.listen(&xdg_popup->events.destroy, this, &Popup::on_destroy);

        wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
        if (!parent || !parent->data) {
            wlr_log(WLR_ERROR, "Popup parent surface has no associated scene tree");
            return;
        }

        auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

        m_listeners.listen(&xdg_popup->base->surface->events.commit, this, &Popup::on_commit);
    }

    void Popup::on_commit() {
        if (m_xdg_popup->base->initial_commit) {
            wlr_xdg_surface* root = m_xdg_popup->base;
            while (root->role == WLR_XDG_SURFACE_ROLE_POPUP && root->popup && root->popup->parent) {
                wlr_xdg_surface* next = wlr_xdg_surface_try_from_wlr_surface(root->popup->parent);
                if (!next) {
                    break;
                }
                root = next;
            }

            if (root && root->data) {
                auto* root_tree = static_cast<wlr_scene_tree*>(root->data);
                int root_lx = 0;
                int root_ly = 0;
                wlr_scene_node_coords(&root_tree->node, &root_lx, &root_ly);

                wlr_output* output = wlr_output_layout_output_at(m_compositor.output_layout(), root_lx, root_ly);
                if (output != nullptr) {
                    wlr_box output_box{};
                    wlr_output_layout_get_box(m_compositor.output_layout(), output, &output_box);

                    wlr_box toplevel_space_box{
                        .x = output_box.x - root_lx,
                        .y = output_box.y - root_ly,
                        .width = output_box.width,
                        .height = output_box.height,
                    };
                    wlr_xdg_popup_unconstrain_from_box(m_xdg_popup, &toplevel_space_box);
                }
            }

            wlr_xdg_surface_schedule_configure(m_xdg_popup->base);
        }
    }

    void Popup::on_destroy() {
        m_compositor.remove_popup(this);
    }

}
