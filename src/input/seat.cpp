#include "flux/input/seat.hpp"
#include "flux/input.hpp"

namespace flux::input {

    Seat::Seat(Input& input, wl_display* display, const char* name)
        : m_input(input), m_wlr_seat(wlr_seat_create(display, name)) {
        m_listeners.listen(&m_wlr_seat->events.request_set_cursor, this, &Seat::on_request_set_cursor);
        m_listeners.listen(&m_wlr_seat->pointer_state.events.focus_change, this, &Seat::on_pointer_focus_change);
        m_listeners.listen(&m_wlr_seat->events.request_set_selection, this, &Seat::on_request_set_selection);

        auto* cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(display, 2);
        m_listeners.listen(&cursor_shape_mgr->events.request_set_shape, this, &Seat::on_request_set_shape);
    }

    Seat::~Seat() {
        m_listeners.clear();
        if (m_wlr_seat) {
            wlr_seat_destroy(m_wlr_seat);
        }
    }

    void Seat::on_request_set_cursor(wlr_seat_pointer_request_set_cursor_event* event) {
        if (m_wlr_seat->pointer_state.focused_client == event->seat_client && m_input.cursor().is_passthrough()) {
            m_input.cursor().set_surface(event->surface, event->hotspot_x, event->hotspot_y);
        }
    }

    void Seat::on_request_set_shape(wlr_cursor_shape_manager_v1_request_set_shape_event* event) {
        if (m_wlr_seat->pointer_state.focused_client == event->seat_client && m_input.cursor().is_passthrough()) {
            m_input.cursor().set_xcursor(wlr_cursor_shape_v1_name(event->shape));
        }
    }

    void Seat::on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event) {
        if (event->new_surface == nullptr && m_input.cursor().is_passthrough()) {
            m_input.cursor().set_xcursor("default");
        }
    }

    void Seat::on_request_set_selection(wlr_seat_request_set_selection_event* event) {
        wlr_seat_set_selection(m_wlr_seat, event->source, event->serial);
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

}
