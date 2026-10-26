#include "flux/keyboard.hpp"
#include "flux/server.hpp"

#include <algorithm>

namespace flux {

    Keyboard::Keyboard(Server& server, wlr_input_device* device)
        : m_server(server), m_wlr_keyboard(wlr_keyboard_from_input_device(device)) {
        xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

        wlr_keyboard_set_keymap(m_wlr_keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(m_wlr_keyboard, 25, 600);

        m_listeners.listen(&m_wlr_keyboard->events.modifiers, this, &Keyboard::on_modifiers);
        m_listeners.listen(&m_wlr_keyboard->events.key, this, &Keyboard::on_key);
        m_listeners.listen(&device->events.destroy, this, &Keyboard::on_destroy);

        wlr_seat_set_keyboard(m_server.seat(), m_wlr_keyboard);
    }

    void Keyboard::on_modifiers() {
        wlr_seat_set_keyboard(m_server.seat(), m_wlr_keyboard);
        wlr_seat_keyboard_notify_modifiers(m_server.seat(), &m_wlr_keyboard->modifiers);
    }

    void Keyboard::on_key(wlr_keyboard_key_event* event) {
        uint32_t keycode = event->keycode + 8;
        const xkb_keysym_t* syms = nullptr;
        int nsyms = xkb_state_key_get_syms(m_wlr_keyboard->xkb_state, keycode, &syms);

        bool handled = false;
        uint32_t modifiers = wlr_keyboard_get_modifiers(m_wlr_keyboard);

        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; ++i) {
                if (m_server.handle_keybinding(modifiers, syms[i])) {
                    handled = true;
                    break;
                }
            }

            if (!handled && m_wlr_keyboard->keymap) {
                xkb_layout_index_t layout = xkb_state_key_get_layout(m_wlr_keyboard->xkb_state, keycode);
                const xkb_keysym_t* raw_syms = nullptr;
                int n_raw = xkb_keymap_key_get_syms_by_level(m_wlr_keyboard->keymap, keycode, layout, 0, &raw_syms);
                for (int i = 0; i < n_raw; ++i) {
                    if (m_server.handle_keybinding(modifiers, raw_syms[i])) {
                        handled = true;
                        break;
                    }
                }
            }

            if (handled && !std::ranges::contains(m_consumed_keycodes, event->keycode)) {
                m_consumed_keycodes.push_back(event->keycode);
            }
        } else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            handled = std::erase(m_consumed_keycodes, event->keycode) > 0;
        }

        if (!handled) {
            wlr_seat_set_keyboard(m_server.seat(), m_wlr_keyboard);
            wlr_seat_keyboard_notify_key(m_server.seat(), event->time_msec, event->keycode, event->state);
        }
    }

    void Keyboard::on_destroy() {
        m_server.remove_keyboard(this);
    }

}
