#pragma once

#include "flux/wlroots.hpp"

#include <variant>

namespace flux {

    class Window;

    namespace cursor_state {
        struct Passthrough {};

        struct Move {
            Window* window = nullptr;
            double grab_x = 0.0;
            double grab_y = 0.0;
        };

        struct Resize {
            Window* window = nullptr;
            double grab_x = 0.0;
            double grab_y = 0.0;
            wlr_box initial_geometry{};
            uint32_t resize_edges = 0;
        };
    }

    using CursorState = std::variant<cursor_state::Passthrough, cursor_state::Move, cursor_state::Resize>;

}
