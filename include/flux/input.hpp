#pragma once

#include "flux/input/cursor.hpp"
#include "flux/input/keyboard.hpp"
#include "flux/input/pointer.hpp"
#include "flux/input/seat.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <memory>
#include <vector>

namespace flux {

    class Compositor;

    class Input {
    public:
        explicit Input(Compositor& compositor);
        ~Input() = default;

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;

        [[nodiscard]] Compositor& compositor() noexcept { return m_compositor; }
        [[nodiscard]] const Compositor& compositor() const noexcept { return m_compositor; }

        [[nodiscard]] input::Seat& seat() noexcept { return *m_seat; }
        [[nodiscard]] const input::Seat& seat() const noexcept { return *m_seat; }

        [[nodiscard]] input::Cursor& cursor() noexcept { return *m_cursor; }
        [[nodiscard]] const input::Cursor& cursor() const noexcept { return *m_cursor; }

        void remove_keyboard(input::Keyboard* keyboard);
        void remove_pointer(input::Pointer* pointer);

    private:
        void on_new_input(wlr_input_device* device);
        void update_capabilities();

        Compositor& m_compositor;
        std::unique_ptr<input::Seat> m_seat;
        std::unique_ptr<input::Cursor> m_cursor;
        std::vector<std::unique_ptr<input::Keyboard>> m_keyboards;
        std::vector<std::unique_ptr<input::Pointer>> m_pointers;

        ListenerGroup m_listeners;
    };

}
