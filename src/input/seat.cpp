#include "frost/input/seat.hpp"
#include "frost/compositor.hpp"
#include "frost/input.hpp"
#include "frost/layer.hpp"
#include "frost/log.hpp"
#include "frost/output.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace frost::input {

    namespace {
        void warp_to_hint(Input& input, const wlr_pointer_constraint_v1* constraint) {
            if (!constraint || constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
                !constraint->current.cursor_hint.enabled ||
                constraint->surface != constraint->seat->pointer_state.focused_surface) {
                return;
            }
            const double sx = constraint->current.cursor_hint.x;
            const double sy = constraint->current.cursor_hint.y;

            int lx = 0;
            int ly = 0;
            if (input.compositor().scene().surface_coords(constraint->surface, lx, ly)) {
                input.cursor().warp(lx + sx, ly + sy);
                wlr_seat_pointer_warp(constraint->seat, sx, sy);
            }
        }

        [[nodiscard]] bool is_inhibitor_visible(const Compositor& comp, wlr_surface* surface) noexcept {
            if (!surface || !surface->mapped) {
                return false;
            }
            wlr_surface* root = root_popup_parent(surface);
            if (comp.is_session_locked()) {
                const Output* lock_out = comp.output_from_lock_surface(root);
                return lock_out && lock_out->is_enabled();
            }
            if (const Window* win = Window::try_from(root)) {
                if (!win->workspace() || !win->scene_tree() || !win->scene_tree()->node.enabled) {
                    return false;
                }
                const Output& out = win->workspace()->output();
                return out.is_enabled() && win->workspace() == &out.active_workspace();
            }
            if (const Layer* layer = Layer::try_from(root)) {
                return layer->output() && layer->output()->is_enabled();
            }
            return true;
        }
    }

    struct IdleInhibitor {
        wlr_surface* surface = nullptr;
        ListenerGroup listeners;
    };

    Seat::Seat(Input& input, wl_display* display, const char* name)
        : m_input(input), m_wlr_seat(wlr_seat_create(display, name)),
          m_idle_notifier(wlr_idle_notifier_v1_create(display)),
          m_pointer_constraints(wlr_pointer_constraints_v1_create(display)) {
        m_listeners.listen(&m_wlr_seat->events.request_set_cursor, this, &Seat::on_request_set_cursor);
        m_listeners.listen(&m_wlr_seat->pointer_state.events.focus_change, this, &Seat::on_pointer_focus_change);
        m_listeners.listen(&m_wlr_seat->events.request_set_selection, this, &Seat::on_request_set_selection);
        m_listeners.listen(&m_wlr_seat->events.request_set_primary_selection, this,
                           &Seat::on_request_set_primary_selection);
        m_listeners.listen(&m_wlr_seat->events.request_start_drag, this, &Seat::on_request_start_drag);
        m_listeners.listen(&m_wlr_seat->events.start_drag, this, &Seat::on_start_drag);

        auto* cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(display, 2);
        m_listeners.listen(&cursor_shape_mgr->events.request_set_shape, this, &Seat::on_request_set_shape);

        auto* idle_inhibit_mgr = wlr_idle_inhibit_v1_create(display);
        m_listeners.listen(&idle_inhibit_mgr->events.new_inhibitor, this, &Seat::on_new_idle_inhibitor);

        m_listeners.listen(&m_pointer_constraints->events.new_constraint, this, &Seat::update_pointer_constraint);
    }

    Seat::~Seat() {
        m_drag_listeners.clear();
        m_constraint_listeners.clear();
        m_idle_inhibitors.clear();
        m_listeners.clear();
        if (m_wlr_seat) {
            wlr_seat_destroy(m_wlr_seat);
        }
    }

    void Seat::on_request_set_cursor(wlr_seat_pointer_request_set_cursor_event* event) {
        if (m_wlr_seat->pointer_state.focused_client == event->seat_client) {
            m_input.cursor().set_surface(event->surface, event->hotspot_x, event->hotspot_y);
        }
    }

    void Seat::on_request_set_shape(wlr_cursor_shape_manager_v1_request_set_shape_event* event) {
        if (m_wlr_seat->pointer_state.focused_client == event->seat_client) {
            m_input.cursor().set_xcursor(wlr_cursor_shape_v1_name(event->shape));
        }
    }

    void Seat::on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event) {
        m_input.cursor().clear_button_grab();
        if (event->new_surface == nullptr) {
            m_input.cursor().set_xcursor("default");
        }
        update_pointer_constraint();
    }

    void Seat::on_request_set_selection(wlr_seat_request_set_selection_event* event) {
        wlr_seat_set_selection(m_wlr_seat, event->source, event->serial);
    }

    void Seat::on_request_set_primary_selection(wlr_seat_request_set_primary_selection_event* event) {
        if (m_input.compositor().config().middle_click_paste()) {
            wlr_seat_set_primary_selection(m_wlr_seat, event->source, event->serial);
        } else if (event->source) {
            wlr_primary_selection_source_destroy(event->source);
        }
    }

    void Seat::on_request_start_drag(wlr_seat_request_start_drag_event* event) {
        if (!m_input.compositor().is_locked() &&
            wlr_seat_validate_pointer_grab_serial(m_wlr_seat, event->origin, event->serial)) {
            wlr_seat_start_pointer_drag(m_wlr_seat, event->drag, event->serial);
        } else {
            log_debug("rejecting drag start request for serial {}", event->serial);
            if (event->drag->source) {
                wlr_data_source_destroy(event->drag->source);
            }
        }
    }

    void Seat::on_start_drag(wlr_drag* drag) {
        m_input.cursor().clear_button_grab();
        if (drag->icon) {
            wlr_scene_drag_icon_create(m_input.compositor().scene().drag_icons(), drag->icon);
            update_drag_icons_position();
        }
        m_drag_listeners.clear();
        m_drag_listeners.listen(&drag->events.destroy, this, &Seat::on_destroy_drag);
    }

    void Seat::on_destroy_drag() {
        m_drag_listeners.clear();
        m_input.cursor().set_xcursor("default");
        m_input.compositor().refocus();
    }

    void Seat::cancel_drag() noexcept {
        if (!m_wlr_seat || !m_wlr_seat->drag) {
            return;
        }
        m_drag_listeners.clear();
        if (auto* source = m_wlr_seat->drag->source) {
            wlr_data_source_destroy(source);
        }
        if (m_wlr_seat->drag) {
            wlr_seat_keyboard_end_grab(m_wlr_seat);
        }
        m_input.cursor().set_xcursor("default");
    }

    void Seat::update_drag_icons_position() noexcept {
        wlr_scene_tree* drag_icons = m_input.compositor().scene().drag_icons();
        if (wl_list_empty(&drag_icons->children)) {
            return;
        }

        const int cx = static_cast<int>(std::round(m_input.cursor().x()));
        const int cy = static_cast<int>(std::round(m_input.cursor().y()));

        wlr_scene_node* node = nullptr;
        wl_list_for_each(node, &drag_icons->children, link) {
            wlr_scene_node_set_position(node, cx, cy);
        }
    }

    void Seat::notify_keyboard_enter(wlr_surface* surface) noexcept {
        if (!surface) {
            set_active_constraint(nullptr);
            wlr_seat_keyboard_notify_clear_focus(m_wlr_seat);
            return;
        }
        wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_wlr_seat);
        if (keyboard) {
            const auto* kb = static_cast<const Keyboard*>(keyboard->data);
            std::array<uint32_t, WLR_KEYBOARD_KEYS_CAP> keycodes{};
            size_t count = 0;
            for (size_t i = 0; i < keyboard->num_keycodes; ++i) {
                const uint32_t kc = keyboard->keycodes[i];
                if (!kb || !kb->is_consumed(kc)) {
                    keycodes[count++] = kc;
                }
            }
            wlr_seat_keyboard_notify_enter(m_wlr_seat, surface, keycodes.data(), count, &keyboard->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(m_wlr_seat, surface, nullptr, 0, nullptr);
        }
        update_pointer_constraint();
    }

    void Seat::on_new_idle_inhibitor(wlr_idle_inhibitor_v1* inhibitor) {
        auto* surface = inhibitor->surface;
        auto& inh = m_idle_inhibitors.emplace_back(std::make_unique<IdleInhibitor>(surface));
        auto* ptr = inh.get();
        ptr->listeners.listen(&inhibitor->events.destroy, [this, ptr](void*) {
            erase_unique_ptr(m_idle_inhibitors, ptr);
            update_idle_inhibit();
        });
        ptr->listeners.listen(&surface->events.map, this, &Seat::update_idle_inhibit);
        ptr->listeners.listen(&surface->events.unmap, this, &Seat::update_idle_inhibit);
        update_idle_inhibit();
    }

    void Seat::update_idle_inhibit() {
        const auto& comp = m_input.compositor();
        const bool inhibited = std::ranges::any_of(
            m_idle_inhibitors, [&](const auto& inh) { return is_inhibitor_visible(comp, inh->surface); });
        wlr_idle_notifier_v1_set_inhibited(m_idle_notifier, inhibited);
    }

    void Seat::update_pointer_constraint() {
        wlr_surface* surface = focused_pointer_surface();
        wlr_surface* kb_surface = focused_surface();
        const bool same_root = surface && kb_surface &&
                               (wlr_surface_get_root_surface(surface) == wlr_surface_get_root_surface(kb_surface));
        auto* constraint =
            same_root ? wlr_pointer_constraints_v1_constraint_for_surface(m_pointer_constraints, surface, m_wlr_seat)
                      : nullptr;
        set_active_constraint(constraint);
    }

    void Seat::set_active_constraint(wlr_pointer_constraint_v1* constraint) {
        if (m_active_constraint == constraint) {
            return;
        }
        m_constraint_listeners.clear();
        if (m_active_constraint) {
            warp_to_hint(m_input, m_active_constraint);
            wlr_pointer_constraint_v1_send_deactivated(m_active_constraint);
        }
        m_active_constraint = constraint;
        if (m_active_constraint) {
            m_constraint_listeners.listen(&m_active_constraint->events.destroy, this, &Seat::on_destroy_constraint);
            wlr_pointer_constraint_v1_send_activated(m_active_constraint);
        }
    }

    void Seat::on_destroy_constraint() {
        auto* constraint = m_active_constraint;
        m_active_constraint = nullptr;
        m_constraint_listeners.clear();
        warp_to_hint(m_input, constraint);
        m_input.cursor().update_focus();
    }

}
