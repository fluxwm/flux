#include "flux/input/keyboard.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/input.hpp"

#include <algorithm>

namespace flux::input {

    Keyboard::Keyboard(Input& input, wlr_input_device* device, const KeyboardConfig& config)
        : m_input(input), m_wlr_keyboard(wlr_keyboard_from_input_device(device)) {
        xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

        wlr_keyboard_set_keymap(m_wlr_keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);

        apply_config(config);

        m_listeners.listen(&m_wlr_keyboard->events.modifiers, this, &Keyboard::on_modifiers);
        m_listeners.listen(&m_wlr_keyboard->events.key, this, &Keyboard::on_key);
        m_listeners.listen(&device->events.destroy, this, &Keyboard::on_destroy);

        m_input.seat().set_keyboard(m_wlr_keyboard);
    }

    void Keyboard::apply_config(const KeyboardConfig& config) {
        wlr_keyboard_set_repeat_info(m_wlr_keyboard, config.repeat_rate, config.repeat_delay);

        if (m_wlr_keyboard->keymap && m_numlock != config.numlock) {
            m_numlock = config.numlock;
            const xkb_mod_index_t idx = xkb_keymap_mod_get_index(m_wlr_keyboard->keymap, XKB_MOD_NAME_NUM);
            if (idx != XKB_MOD_INVALID) {
                const uint32_t mask = 1u << idx;
                const uint32_t locked = config.numlock ? (m_wlr_keyboard->modifiers.locked | mask)
                                                       : (m_wlr_keyboard->modifiers.locked & ~mask);
                wlr_keyboard_notify_modifiers(m_wlr_keyboard, m_wlr_keyboard->modifiers.depressed,
                                              m_wlr_keyboard->modifiers.latched, locked,
                                              m_wlr_keyboard->modifiers.group);
            }
        }
    }

    void Keyboard::on_modifiers() {
        m_input.seat().set_keyboard(m_wlr_keyboard);
        m_input.seat().notify_modifiers(&m_wlr_keyboard->modifiers);
    }

    void Keyboard::on_key(wlr_keyboard_key_event* event) {
        uint32_t keycode = event->keycode + 8;
        const xkb_keysym_t* syms = nullptr;
        int nsyms = m_wlr_keyboard->xkb_state ? xkb_state_key_get_syms(m_wlr_keyboard->xkb_state, keycode, &syms) : 0;

        bool handled = false;
        uint32_t modifiers = wlr_keyboard_get_modifiers(m_wlr_keyboard);

        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; ++i) {
                if (m_input.compositor().handle_keybinding(modifiers, syms[i])) {
                    handled = true;
                    break;
                }
            }

            if (!handled && m_wlr_keyboard->keymap && m_wlr_keyboard->xkb_state) {
                xkb_layout_index_t layout = xkb_state_key_get_layout(m_wlr_keyboard->xkb_state, keycode);
                const xkb_keysym_t* raw_syms = nullptr;
                int n_raw = xkb_keymap_key_get_syms_by_level(m_wlr_keyboard->keymap, keycode, layout, 0, &raw_syms);
                for (int i = 0; i < n_raw; ++i) {
                    if (m_input.compositor().handle_keybinding(modifiers, raw_syms[i])) {
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
            m_input.seat().set_keyboard(m_wlr_keyboard);
            m_input.seat().notify_key(event->time_msec, event->keycode, event->state);
        }
    }

    void Keyboard::on_destroy() {
        m_input.remove_keyboard(this);
    }

}
