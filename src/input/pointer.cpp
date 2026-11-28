#include "frost/input/pointer.hpp"
#include "frost/config.hpp"
#include "frost/input.hpp"

namespace frost::input {

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

        if (libinput_device_config_accel_is_available(handle)) {
            const auto& accel_speed = is_touchpad ? touchpad_config.accel_speed : mouse_config.accel_speed;
            const auto& accel_profile = is_touchpad ? touchpad_config.accel_profile : mouse_config.accel_profile;

            const double speed = accel_speed.value_or(libinput_device_config_accel_get_default_speed(handle));
            libinput_device_config_accel_set_speed(handle, speed);

            const libinput_config_accel_profile profile =
                accel_profile.value_or(libinput_device_config_accel_get_default_profile(handle));
            if (libinput_device_config_accel_get_profiles(handle) & profile) {
                libinput_device_config_accel_set_profile(handle, profile);
            }
        }
    }

    void Pointer::on_destroy() {
        m_input.cursor().forget_device(m_device);
        m_input.remove_pointer(this);
    }

}
