#include "flux/input/pointer.hpp"
#include "flux/config.hpp"
#include "flux/input.hpp"

namespace flux::input {

    Pointer::Pointer(Input& input, wlr_input_device* device, const MouseConfig& mouse_config,
                     const TouchpadConfig& touchpad_config)
        : m_input(input), m_device(device) {
        m_input.cursor().attach_device(device);
        apply_config(mouse_config, touchpad_config);
        m_listeners.listen(&device->events.destroy, this, &Pointer::on_destroy);
    }

    void Pointer::apply_config(const MouseConfig& mouse_config, const TouchpadConfig& touchpad_config) {
        if (!wlr_input_device_is_libinput(m_device)) {
            return;
        }

        libinput_device* handle = wlr_libinput_get_device_handle(m_device);
        if (!handle) {
            return;
        }

        const bool is_touchpad = libinput_device_config_tap_get_finger_count(handle) > 0;
        if (is_touchpad) {
            const auto tap = touchpad_config.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED;
            libinput_device_config_tap_set_enabled(handle, tap);
        }

        if (libinput_device_config_scroll_has_natural_scroll(handle)) {
            const bool natural_scroll = is_touchpad ? touchpad_config.natural_scroll : mouse_config.natural_scroll;
            libinput_device_config_scroll_set_natural_scroll_enabled(handle, natural_scroll);
        }
    }

    void Pointer::on_destroy() {
        m_input.remove_pointer(this);
    }

}
