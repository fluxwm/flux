#include "flux/input/seat.hpp"
#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/util.hpp"

#include <algorithm>

namespace flux::input {

    struct IdleInhibitor {
        wlr_surface* surface = nullptr;
        ListenerGroup listeners;
    };

    Seat::Seat(Input& input, wl_display* display, const char* name)
        : m_input(input), m_wlr_seat(wlr_seat_create(display, name)),
          m_idle_notifier(wlr_idle_notifier_v1_create(display)) {
        m_listeners.listen(&m_wlr_seat->events.request_set_cursor, this, &Seat::on_request_set_cursor);
        m_listeners.listen(&m_wlr_seat->pointer_state.events.focus_change, this, &Seat::on_pointer_focus_change);
        m_listeners.listen(&m_wlr_seat->events.request_set_selection, this, &Seat::on_request_set_selection);
        m_listeners.listen(&m_wlr_seat->events.request_set_primary_selection, this,
                           &Seat::on_request_set_primary_selection);

        auto* cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(display, 2);
        m_listeners.listen(&cursor_shape_mgr->events.request_set_shape, this, &Seat::on_request_set_shape);

        auto* idle_inhibit_mgr = wlr_idle_inhibit_v1_create(display);
        m_listeners.listen(&idle_inhibit_mgr->events.new_inhibitor, this, &Seat::on_new_idle_inhibitor);
    }

    Seat::~Seat() {
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
        if (event->new_surface == nullptr) {
            m_input.cursor().set_xcursor("default");
        }
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

    void Seat::notify_keyboard_enter(wlr_surface* surface) noexcept {
        if (!surface) {
            clear_keyboard_focus();
            return;
        }
        wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_wlr_seat);
        if (keyboard) {
            wlr_seat_keyboard_notify_enter(m_wlr_seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                           &keyboard->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(m_wlr_seat, surface, nullptr, 0, nullptr);
        }
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
        const bool inhibited =
            std::ranges::any_of(m_idle_inhibitors, [](const auto& inh) { return inh->surface->mapped; });
        wlr_idle_notifier_v1_set_inhibited(m_idle_notifier, inhibited);
    }

}
