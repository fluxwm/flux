#include "flux/popup.hpp"
#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/output.hpp"

namespace flux {

    Popup::Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup, wlr_scene_tree* root_tree)
        : m_compositor(compositor), m_xdg_popup(xdg_popup), m_root_tree(root_tree) {
        m_listeners.listen(&xdg_popup->events.destroy, this, &Popup::on_destroy);

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

    Popup::Popup(Compositor& compositor, wlr_xwayland_surface* xsurface)
        : m_compositor(compositor), m_xwayland_surface(xsurface) {
        m_listeners.listen(&xsurface->events.associate, this, &Popup::on_associate);
        m_listeners.listen(&xsurface->events.dissociate, this, &Popup::on_dissociate);
        m_listeners.listen(&xsurface->events.destroy, this, &Popup::on_destroy);
        m_listeners.listen(&xsurface->events.request_configure, this, &Popup::on_request_configure);
        m_listeners.listen(&xsurface->events.set_override_redirect, this, &Popup::on_set_override_redirect);

        if (xsurface->surface) {
            on_associate();
        }
    }

    Popup::~Popup() {
        m_surface_listeners.clear();
        m_listeners.clear();
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
            m_wrapper_tree = nullptr;
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

        wlr_output* wlr_out = wlr_output_layout_output_at(m_compositor.output_layout(), root_lx, root_ly);
        Output* output = m_compositor.output_from_wlr(wlr_out);
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

    void Popup::on_associate() {
        m_surface_listeners.clear();
        if (m_xwayland_surface && m_xwayland_surface->surface) {
            m_surface_listeners.listen(&m_xwayland_surface->surface->events.map, this, &Popup::on_map);
            m_surface_listeners.listen(&m_xwayland_surface->surface->events.unmap, this, &Popup::on_unmap);
            if (m_xwayland_surface->surface->mapped) {
                on_map();
            }
        }
    }

    void Popup::on_dissociate() {
        m_surface_listeners.clear();
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
            m_wrapper_tree = nullptr;
        }
    }

    void Popup::on_map() {
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
        }
        m_wrapper_tree = wlr_scene_subsurface_tree_create(m_compositor.scene().popups(), m_xwayland_surface->surface);
        if (m_wrapper_tree) {
            wlr_scene_node_set_position(&m_wrapper_tree->node, m_xwayland_surface->x, m_xwayland_surface->y);
        }
        if (wlr_xwayland_surface_override_redirect_wants_focus(m_xwayland_surface) && !m_compositor.exclusive_layer()) {
            wlr_xwayland_surface_activate(m_xwayland_surface, true);
            m_compositor.focus_surface(m_xwayland_surface->surface);
        }
    }

    void Popup::on_unmap() {
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
            m_wrapper_tree = nullptr;
        }
        if (m_xwayland_surface && m_xwayland_surface->surface &&
            m_compositor.input().seat().focused_surface() == m_xwayland_surface->surface) {
            m_compositor.refocus();
        }
    }

    void Popup::on_request_configure(wlr_xwayland_surface_configure_event* event) {
        wlr_xwayland_surface_configure(m_xwayland_surface, event->x, event->y, event->width, event->height);
        if (m_wrapper_tree && m_xwayland_surface->surface && m_xwayland_surface->surface->mapped) {
            wlr_scene_node_set_position(&m_wrapper_tree->node, event->x, event->y);
        }
    }

    void Popup::on_set_override_redirect() {
        if (m_xwayland_surface && !m_xwayland_surface->override_redirect) {
            m_compositor.recreate_as_window(this);
        }
    }

}
