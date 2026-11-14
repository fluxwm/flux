#include "flux/input/keyboard.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/input.hpp"

#include <algorithm>

namespace flux::input {

    Keyboard::Keyboard(Input& input, wlr_input_device* device, const KeyboardConfig& config)
        : m_input(input), m_wlr_keyboard(wlr_keyboard_from_input_device(device)),
          m_is_virtual(wlr_input_device_get_virtual_keyboard(device) != nullptr) {
        m_wlr_keyboard->data = this;
        apply_config(config);

        wl_event_loop* loop = wl_display_get_event_loop(m_input.compositor().display());
        if (loop) {
            m_repeat_timer = wl_event_loop_add_timer(
                loop, [](void* data) -> int { return static_cast<Keyboard*>(data)->on_repeat_timer(); }, this);
        }

        m_listeners.listen(&m_wlr_keyboard->events.modifiers, this, &Keyboard::on_modifiers);
        m_listeners.listen(&m_wlr_keyboard->events.key, this, &Keyboard::on_key);
        m_listeners.listen(&m_wlr_keyboard->events.keymap, this, &Keyboard::on_keymap);
        m_listeners.listen(&device->events.destroy, this, &Keyboard::on_destroy);

        if (m_wlr_keyboard->keymap || !m_input.seat().keyboard()) {
            m_input.seat().set_keyboard(m_wlr_keyboard);
        }
    }

    Keyboard::~Keyboard() {
        if (m_wlr_keyboard) {
            m_wlr_keyboard->data = nullptr;
        }
        if (m_repeat_timer) {
            wl_event_source_remove(m_repeat_timer);
        }
    }

    bool Keyboard::is_consumed(uint32_t keycode) const noexcept {
        return std::ranges::contains(m_consumed_keycodes, keycode);
    }

    void Keyboard::apply_config(const KeyboardConfig& config) {
        stop_repeat();
        if (m_is_virtual) {
            if (m_wlr_keyboard->repeat_info.rate != config.repeat_rate ||
                m_wlr_keyboard->repeat_info.delay != config.repeat_delay) {
                wlr_keyboard_set_repeat_info(m_wlr_keyboard, config.repeat_rate, config.repeat_delay);
            }
            return;
        }

        if (m_wlr_keyboard->keymap && m_config == config) {
            return;
        }

        wlr_keyboard_set_repeat_info(m_wlr_keyboard, config.repeat_rate, config.repeat_delay);

        bool keymap_updated = false;
        if (!m_wlr_keyboard->keymap || m_config.rules != config.rules || m_config.model != config.model ||
            m_config.layout != config.layout || m_config.variant != config.variant ||
            m_config.options != config.options) {
            const xkb_rule_names rules{
                .rules = config.rules.empty() ? nullptr : config.rules.c_str(),
                .model = config.model.empty() ? nullptr : config.model.c_str(),
                .layout = config.layout.empty() ? nullptr : config.layout.c_str(),
                .variant = config.variant.empty() ? nullptr : config.variant.c_str(),
                .options = config.options.empty() ? nullptr : config.options.c_str(),
            };

            if (xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
                xkb_keymap* keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
                if (!keymap) {
                    wlr_log(WLR_ERROR, "failed to compile xkb keymap, falling back to default");
                    if (xkb_context* fallback_ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES)) {
                        keymap = xkb_keymap_new_from_names(fallback_ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
                        xkb_context_unref(fallback_ctx);
                    }
                }
                if (keymap) {
                    wlr_keyboard_set_keymap(m_wlr_keyboard, keymap);
                    xkb_keymap_unref(keymap);
                    keymap_updated = true;
                } else {
                    wlr_log(WLR_ERROR, "failed to compile any valid xkb keymap");
                }
                xkb_context_unref(context);
            } else {
                wlr_log(WLR_ERROR, "failed to create xkb context");
            }
        }

        m_config = config;

        if (m_wlr_keyboard->keymap && (m_numlock != config.numlock || (keymap_updated && config.numlock))) {
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

        if (m_repeating) {
            const uint32_t cur_mods = wlr_keyboard_get_modifiers(m_wlr_keyboard) & key_modifier_mask;
            if (cur_mods != m_repeat_modifiers) {
                stop_repeat();
            }
        }
    }

    void Keyboard::start_repeat(uint32_t keycode, uint32_t modifiers, xkb_keysym_t sym) {
        const int32_t rate = m_wlr_keyboard->repeat_info.rate;
        const int32_t delay = m_wlr_keyboard->repeat_info.delay;
        if (rate <= 0 || delay <= 0 || !m_repeat_timer) {
            return;
        }

        m_repeat_keycode = keycode;
        m_repeat_modifiers = modifiers & key_modifier_mask;
        m_repeat_sym = sym;
        m_repeating = true;
        wl_event_source_timer_update(m_repeat_timer, delay);
    }

    void Keyboard::stop_repeat() {
        if (m_repeating) {
            m_repeating = false;
            m_repeat_keycode = 0;
            m_repeat_modifiers = 0;
            m_repeat_sym = XKB_KEY_NoSymbol;
            wl_event_source_timer_update(m_repeat_timer, 0);
        }
    }

    int Keyboard::on_repeat_timer() {
        if (!m_repeating) {
            return 0;
        }

        m_input.seat().notify_activity();
        const auto res = m_input.compositor().handle_keybinding(m_repeat_modifiers, m_repeat_sym, true);
        const int32_t rate = m_wlr_keyboard->repeat_info.rate;
        if (res.handled && rate > 0 && m_repeating) {
            wl_event_source_timer_update(m_repeat_timer, std::max(1, 1000 / rate));
        } else {
            stop_repeat();
        }
        return 0;
    }

    void Keyboard::on_key(wlr_keyboard_key_event* event) {
        m_input.seat().notify_activity();
        bool handled = false;

        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            stop_repeat();

            const uint32_t keycode = event->keycode + 8;
            const xkb_keysym_t* syms = nullptr;
            const int nsyms =
                m_wlr_keyboard->xkb_state ? xkb_state_key_get_syms(m_wlr_keyboard->xkb_state, keycode, &syms) : 0;
            const uint32_t modifiers = wlr_keyboard_get_modifiers(m_wlr_keyboard);

            auto try_syms = [&](const xkb_keysym_t* list, int count) {
                for (int i = 0; i < count; ++i) {
                    if (const auto res = m_input.compositor().handle_keybinding(modifiers, list[i]); res.handled) {
                        if (res.repeat) {
                            start_repeat(event->keycode, modifiers, list[i]);
                        }
                        return true;
                    }
                }
                return false;
            };

            handled = try_syms(syms, nsyms);
            if (!handled && m_wlr_keyboard->keymap && m_wlr_keyboard->xkb_state) {
                auto try_layout = [&](xkb_layout_index_t layout) {
                    const xkb_keysym_t* raw_syms = nullptr;
                    const int n_raw =
                        xkb_keymap_key_get_syms_by_level(m_wlr_keyboard->keymap, keycode, layout, 0, &raw_syms);
                    return try_syms(raw_syms, n_raw);
                };

                const xkb_layout_index_t active_layout = xkb_state_key_get_layout(m_wlr_keyboard->xkb_state, keycode);
                if (xkb_state_key_get_level(m_wlr_keyboard->xkb_state, keycode, active_layout) != 0) {
                    handled = try_layout(active_layout);
                }

                const xkb_layout_index_t num_layouts = xkb_keymap_num_layouts_for_key(m_wlr_keyboard->keymap, keycode);
                for (xkb_layout_index_t l = 0; !handled && l < num_layouts; ++l) {
                    if (l != active_layout) {
                        handled = try_layout(l);
                    }
                }
            }

            if (handled && !std::ranges::contains(m_consumed_keycodes, event->keycode)) {
                m_consumed_keycodes.push_back(event->keycode);
            }
        } else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (m_repeating && event->keycode == m_repeat_keycode) {
                stop_repeat();
            }

            handled = std::erase(m_consumed_keycodes, event->keycode) > 0;
        }

        if (!handled) {
            m_input.seat().set_keyboard(m_wlr_keyboard);
            m_input.seat().notify_key(event->time_msec, event->keycode, event->state);
        }
    }

    void Keyboard::on_keymap() {
        stop_repeat();
        m_input.seat().set_keyboard(m_wlr_keyboard);
    }

    void Keyboard::on_destroy() {
        m_input.remove_keyboard(this);
    }

}
