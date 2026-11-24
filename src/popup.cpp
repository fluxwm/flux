#include "frost/popup.hpp"
#include "frost/compositor.hpp"
#include "frost/input.hpp"
#include "frost/output.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

namespace frost {

    Popup::Popup(Compositor& compositor, wlr_xdg_popup* xdg_popup, wlr_scene_tree* root_tree)
        : m_compositor(compositor), m_xdg_popup(xdg_popup), m_root_tree(root_tree) {
        m_listeners.listen(&xdg_popup->events.destroy, this, &Popup::on_destroy);

        wlr_scene_tree* parent_tree = nullptr;
        wlr_surface* parent_surface = wlr_surface_get_root_surface(xdg_popup->parent);
        wlr_xdg_surface* parent_xdg = wlr_xdg_surface_try_from_wlr_surface(parent_surface);

        if (parent_xdg && parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && parent_xdg->data) {
            parent_tree = static_cast<wlr_scene_tree*>(parent_xdg->data);
        } else {
            wlr_xdg_surface* root_xdg = wlr_xdg_surface_try_from_wlr_surface(root_popup_parent(xdg_popup->parent));
            wlr_scene_tree* target_layer = (root_xdg && root_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
                                               ? m_compositor.scene().window_popups()
                                               : m_compositor.scene().popups();
            m_wrapper_tree = wlr_scene_tree_create(target_layer);
            m_wrapper_tree->node.data = m_root_tree->node.data;
            parent_tree = m_wrapper_tree;
        }

        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

        m_listeners.listen(&xdg_popup->base->surface->events.commit, this, &Popup::on_commit);
        m_listeners.listen(&xdg_popup->events.reposition, this, &Popup::on_reposition);
        m_listeners.listen(&xdg_popup->base->surface->events.map, this, &Popup::on_map);
        m_listeners.listen(&xdg_popup->base->surface->events.unmap, this, &Popup::on_unmap);

        update_position();
    }

    Popup::Popup(Compositor& compositor, wlr_xwayland_surface* xsurface)
        : m_compositor(compositor), m_xwayland_surface(xsurface) {
        m_listeners.listen(&xsurface->events.associate, this, &Popup::on_associate);
        m_listeners.listen(&xsurface->events.dissociate, this, &Popup::on_dissociate);
        m_listeners.listen(&xsurface->events.destroy, this, &Popup::on_destroy);
        m_listeners.listen(&xsurface->events.request_configure, this, &Popup::on_request_configure);
        m_listeners.listen(&xsurface->events.set_geometry, this, &Popup::update_position);
        m_listeners.listen(&xsurface->events.set_override_redirect, this, &Popup::on_set_override_redirect);

        if (xsurface->surface) {
            on_associate();
        }
    }

    Popup::~Popup() {
        m_workspace_listeners.clear();
        m_surface_listeners.clear();
        m_listeners.clear();
        m_workspace = nullptr;
        if (m_xdg_popup && m_xdg_popup->base) {
            if (auto* self_tree = static_cast<wlr_scene_tree*>(m_xdg_popup->base->data)) {
                m_xdg_popup->base->data = nullptr;
                for (const auto& popup : m_compositor.popups()) {
                    if (!popup->raw() || !popup->raw()->base) {
                        continue;
                    }
                    for (auto* tree = static_cast<wlr_scene_tree*>(popup->raw()->base->data); tree;
                         tree = tree->node.parent) {
                        if (tree == self_tree) {
                            popup->raw()->base->data = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
            m_wrapper_tree = nullptr;
        }
    }

    void Popup::set_workspace(Workspace* workspace) {
        m_workspace_listeners.clear();
        m_workspace = workspace;
        if (m_workspace && m_workspace->tree()) {
            m_workspace_listeners.listen(&m_workspace->tree()->node.events.destroy, this, &Popup::on_workspace_destroy);
        }
    }

    void Popup::on_workspace_destroy() {
        m_workspace = nullptr;
        m_workspace_listeners.clear();
    }

    bool Popup::is_visible_on(const wlr_box& output_box) const noexcept {
        if (output_box.width <= 0 || output_box.height <= 0) {
            return false;
        }

        wlr_scene_tree* tree = nullptr;
        if (m_xdg_popup) {
            if (!m_xdg_popup->base || !m_xdg_popup->base->surface || !m_xdg_popup->base->surface->mapped) {
                return false;
            }
            tree = static_cast<wlr_scene_tree*>(m_xdg_popup->base->data);
        } else if (m_xwayland_surface) {
            if (!m_xwayland_surface->surface || !m_xwayland_surface->surface->mapped) {
                return false;
            }
            tree = m_wrapper_tree;
        }

        if (!tree) {
            return false;
        }

        int lx = 0;
        int ly = 0;
        if (!wlr_scene_node_coords(&tree->node, &lx, &ly)) {
            return false;
        }

        struct HitContext {
            wlr_box output_box;
            bool visible = false;
        } ctx{.output_box = output_box};

        wlr_scene_node_for_each_buffer(
            &tree->node,
            [](wlr_scene_buffer* buffer, int sx, int sy, void* data) {
                auto* hit = static_cast<HitContext*>(data);
                if (hit->visible || !buffer || !buffer->buffer) {
                    return;
                }
                const int w = buffer->dst_width > 0 ? buffer->dst_width : buffer->buffer->width;
                const int h = buffer->dst_height > 0 ? buffer->dst_height : buffer->buffer->height;
                if (w <= 0 || h <= 0) {
                    return;
                }
                const wlr_box buf_box{.x = sx, .y = sy, .width = w, .height = h};
                wlr_box intersection{};
                if (wlr_box_intersection(&intersection, &buf_box, &hit->output_box)) {
                    hit->visible = true;
                }
            },
            &ctx);

        return ctx.visible;
    }

    void Popup::update_position() noexcept {
        if (m_wrapper_tree && m_root_tree) {
            int root_lx = 0;
            int root_ly = 0;
            const bool visible = wlr_scene_node_coords(&m_root_tree->node, &root_lx, &root_ly);
            wlr_scene_node_set_position(&m_wrapper_tree->node, root_lx, root_ly);
            wlr_scene_node_set_enabled(&m_wrapper_tree->node, visible);
        } else if (m_wrapper_tree && m_xwayland_surface) {
            Workspace* ws = m_workspace;
            if (const auto* parent = Window::try_from(m_xwayland_surface->parent); parent && parent->workspace()) {
                ws = parent->workspace();
            }
            const bool visible = !ws || (ws->output().is_enabled() && ws == &ws->output().active_workspace() &&
                                         !ws->output().animation().is_busy());
            wlr_scene_node_set_position(&m_wrapper_tree->node, m_xwayland_surface->x, m_xwayland_surface->y);
            wlr_scene_node_set_enabled(&m_wrapper_tree->node, visible);
        }
    }

    void Popup::unconstrain() {
        update_position();

        int root_lx = 0;
        int root_ly = 0;
        wlr_scene_node_coords(&m_root_tree->node, &root_lx, &root_ly);

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
        set_workspace(nullptr);
        m_surface_listeners.clear();
        if (m_wrapper_tree) {
            wlr_scene_node_destroy(&m_wrapper_tree->node);
            m_wrapper_tree = nullptr;
        }
    }

    void Popup::on_map() {
        if (m_xwayland_surface) {
            Workspace* ws = nullptr;
            if (const auto* parent = Window::try_from(m_xwayland_surface->parent); parent && parent->workspace()) {
                ws = parent->workspace();
            } else if (m_xwayland_surface->pid > 0) {
                if (const Window* focused = m_compositor.focused_window();
                    focused && focused->xwayland_surface() &&
                    focused->xwayland_surface()->pid == m_xwayland_surface->pid) {
                    ws = focused->workspace();
                }
            }
            if (!ws) {
                wlr_output* wlr_out = wlr_output_layout_output_at(m_compositor.output_layout(), m_xwayland_surface->x,
                                                                  m_xwayland_surface->y);
                Output* out = m_compositor.output_from_wlr(wlr_out);
                if (!out || !out->is_enabled()) {
                    out = m_compositor.focused_output();
                }
                if (out) {
                    ws = &out->active_workspace();
                }
            }
            set_workspace(ws);

            if (m_wrapper_tree) {
                wlr_scene_node_destroy(&m_wrapper_tree->node);
            }
            m_wrapper_tree = wlr_scene_tree_create(m_compositor.scene().window_popups());
            if (m_wrapper_tree) {
                wlr_scene_subsurface_tree_create(m_wrapper_tree, m_xwayland_surface->surface);
                update_position();
            }
            if (m_wrapper_tree && m_wrapper_tree->node.enabled &&
                wlr_xwayland_surface_override_redirect_wants_focus(m_xwayland_surface) && !m_compositor.is_locked()) {
                wlr_xwayland_surface_activate(m_xwayland_surface, true);
                m_compositor.focus_surface(m_xwayland_surface->surface);
            }
        }
        m_compositor.update_cursor_focus();
    }

    void Popup::on_unmap() {
        if (m_xwayland_surface) {
            set_workspace(nullptr);
            if (m_wrapper_tree) {
                wlr_scene_node_destroy(&m_wrapper_tree->node);
                m_wrapper_tree = nullptr;
            }
            if (m_xwayland_surface->surface &&
                m_compositor.input().seat().focused_surface() == m_xwayland_surface->surface) {
                m_compositor.refocus();
                return;
            }
        }
        m_compositor.update_cursor_focus();
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
