#pragma once

#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace frost {

    class Compositor;

    class Bell {
    public:
        explicit Bell(Compositor& compositor);
        ~Bell();

        Bell(const Bell&) = delete;
        Bell& operator=(const Bell&) = delete;

    private:
        void on_ring(const wlr_xdg_system_bell_v1_ring_event* event);
        void trigger_sound();
        void worker_loop(const std::stop_token& stop);

        Compositor& m_compositor;
        ListenerGroup m_listeners;

        std::optional<std::chrono::steady_clock::time_point> m_last_ring;

        std::mutex m_mutex;
        std::condition_variable_any m_cv;
        bool m_pending_sound = false;
        std::jthread m_worker;
    };

}
