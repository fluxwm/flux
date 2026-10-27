#include "flux/input.hpp"
#include "flux/compositor.hpp"
#include "flux/util.hpp"

#include <algorithm>

namespace flux {

    Input::Input(Compositor& compositor)
        : m_compositor(compositor), m_seat(std::make_unique<input::Seat>(*this, compositor.display())),
          m_cursor(std::make_unique<input::Cursor>(*this)) {
        m_listeners.listen(&m_compositor.backend()->events.new_input, this, &Input::on_new_input);
    }

    void Input::on_new_input(wlr_input_device* device) {
        switch (device->type) {
            case WLR_INPUT_DEVICE_KEYBOARD:
                m_keyboards.push_back(std::make_unique<input::Keyboard>(*this, device));
                break;
            case WLR_INPUT_DEVICE_POINTER:
                m_pointers.push_back(std::make_unique<input::Pointer>(*this, device));
                break;
            default:
                break;
        }
        update_capabilities();
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

    void Input::remove_keyboard(input::Keyboard* keyboard) {
        auto it = std::ranges::find_if(m_keyboards, [keyboard](const auto& kb) { return kb.get() == keyboard; });
        if (it != m_keyboards.end()) {
            it = m_keyboards.erase(it);
            m_seat->set_keyboard(it != m_keyboards.end() ? (*it)->raw() : nullptr);
        }
        update_capabilities();
    }

    void Input::remove_pointer(input::Pointer* pointer) {
        erase_unique_ptr(m_pointers, pointer);
        update_capabilities();
    }

}
