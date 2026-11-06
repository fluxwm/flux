#include "flux/input/pointer.hpp"
#include "flux/config.hpp"
#include "flux/input.hpp"

namespace flux::input {

    Pointer::Pointer(Input& input, wlr_input_device* device, const TouchpadConfig& config)
        : m_input(input), m_device(device) {
        m_input.cursor().attach_device(device);
        apply_config(config);
        m_listeners.listen(&device->events.destroy, this, &Pointer::on_destroy);
    }

    void Pointer::apply_config(const TouchpadConfig& config) {
        if (!wlr_input_device_is_libinput(m_device)) {
            return;
        }

        libinput_device* handle = wlr_libinput_get_device_handle(m_device);
        if (!handle) {
            return;
        }

        if (libinput_device_config_tap_get_finger_count(handle) > 0) {
            const auto state = config.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED;
            if (libinput_device_config_tap_get_enabled(handle) != state) {
                libinput_device_config_tap_set_enabled(handle, state);
            }
        }
    }

    void Pointer::on_destroy() {
        m_input.remove_pointer(this);
    }

}
