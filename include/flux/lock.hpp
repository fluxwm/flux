#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;
    class Output;

    class Lock {
    public:
        explicit Lock(Compositor& compositor);
        ~Lock();

        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

        [[nodiscard]] bool is_locked() const noexcept { return m_lock != nullptr || m_persistent; }

        void sync();
        void try_send_locked();
        [[nodiscard]] wlr_surface* target_surface() noexcept;
        [[nodiscard]] Output* output_from_surface(wlr_surface* surface) const noexcept;

    private:
        void on_new_lock(wlr_session_lock_v1* lock);
        void on_new_surface(wlr_session_lock_surface_v1* lock_surface);
        void on_unlock();
        void on_destroy();
        void clear_timer() noexcept;
        void send_locked();

        Compositor& m_compositor;
        wlr_session_lock_v1* m_lock = nullptr;
        bool m_persistent = false;
        bool m_locked_sent = false;
        bool m_unlocked = false;
        wl_event_source* m_timer = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_lock_listeners;
    };

}
