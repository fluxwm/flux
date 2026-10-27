#include "flux/input/pointer.hpp"
#include "flux/input.hpp"

namespace flux::input {

    Pointer::Pointer(Input& input, wlr_input_device* device) : m_input(input) {
        m_input.cursor().attach_device(device);
        m_listeners.listen(&device->events.destroy, this, &Pointer::on_destroy);
    }

    void Pointer::on_destroy() {
        m_input.remove_pointer(this);
    }

}
