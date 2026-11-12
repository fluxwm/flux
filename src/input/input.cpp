#include "flux/input.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/util.hpp"

#include <ranges>

namespace flux {

    Input::Input(Compositor& compositor)
        : m_compositor(compositor), m_seat(std::make_unique<input::Seat>(*this, compositor.display())),
          m_cursor(std::make_unique<input::Cursor>(*this)) {
        m_listeners.listen(&m_compositor.backend()->events.new_input, this, &Input::on_new_input);

        auto* virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(compositor.display());
        m_listeners.listen(&virtual_keyboard_mgr->events.new_virtual_keyboard, this, &Input::on_new_virtual_keyboard);

        auto* virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(compositor.display());
        m_listeners.listen(&virtual_pointer_mgr->events.new_virtual_pointer, this, &Input::on_new_virtual_pointer);
    }

    void Input::on_new_input(wlr_input_device* device) {
        switch (device->type) {
            case WLR_INPUT_DEVICE_KEYBOARD: {
                const auto& kb = m_compositor.config().keyboard();
                m_keyboards.push_back(std::make_unique<input::Keyboard>(*this, device, kb));
                break;
            }
            case WLR_INPUT_DEVICE_POINTER: {
                const auto& tp = m_compositor.config().touchpad();
                m_pointers.push_back(std::make_unique<input::Pointer>(*this, device, tp));
                break;
            }
            default:
                break;
        }
        update_capabilities();
        if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
            if (wlr_surface* focused = m_seat->focused_surface()) {
                m_seat->notify_keyboard_enter(focused);
            } else {
                m_compositor.refocus();
            }
        }
    }

    void Input::on_new_virtual_keyboard(wlr_virtual_keyboard_v1* keyboard) {
        if (!keyboard->seat) {
            return;
        }
        on_new_input(&keyboard->keyboard.base);
    }

    void Input::on_new_virtual_pointer(wlr_virtual_pointer_v1_new_pointer_event* event) {
        auto* device = &event->new_pointer->pointer.base;
        on_new_input(device);
        if (event->suggested_output) {
            m_cursor->map_to_output(device, event->suggested_output);
        }
    }

    void Input::update_capabilities() {
        uint32_t caps = 0;
        if (!m_pointers.empty()) {
            caps |= WL_SEAT_CAPABILITY_POINTER;
        }
        if (!m_keyboards.empty()) {
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        }
        m_seat->set_capabilities(caps);
    }

    void Input::apply_config() {
        const auto& cfg = m_compositor.config();
        if (!cfg.middle_click_paste()) {
            m_seat->clear_primary_selection();
        }
        for (const auto& keyboard : m_keyboards) {
            keyboard->apply_config(cfg.keyboard());
        }
        for (const auto& pointer : m_pointers) {
            pointer->apply_config(cfg.touchpad());
        }
        m_cursor->apply_config(cfg.cursor());
    }

    void Input::remove_keyboard(input::Keyboard* keyboard) {
        if (erase_unique_ptr(m_keyboards, keyboard)) {
            auto it = std::ranges::find_if(std::views::reverse(m_keyboards),
                                           [](const auto& kb) { return kb->raw()->keymap != nullptr; });
            m_seat->set_keyboard(
                it != m_keyboards.rend() ? (*it)->raw() : (!m_keyboards.empty() ? m_keyboards.back()->raw() : nullptr));
        }
        update_capabilities();
    }

    void Input::remove_pointer(input::Pointer* pointer) {
        erase_unique_ptr(m_pointers, pointer);
        update_capabilities();
    }

}
