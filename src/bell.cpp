#include "frost/bell.hpp"
#include "frost/compositor.hpp"
#include "frost/log.hpp"
#include "frost/window.hpp"

#include <canberra.h>

namespace frost {

    namespace {

        constexpr auto bell_cooldown = std::chrono::milliseconds(100);

    }

    Bell::Bell(Compositor& compositor) : m_compositor(compositor) {
        auto* mgr = wlr_xdg_system_bell_v1_create(compositor.display(), 1);
        if (!mgr) {
            log_error("failed to create wlr_xdg_system_bell_v1");
            return;
        }
        m_listeners.listen(&mgr->events.ring, [this](void* data) {
            on_ring(static_cast<const wlr_xdg_system_bell_v1_ring_event*>(data));
        });
    }

    Bell::~Bell() {
        m_listeners.clear();
        if (m_worker.joinable()) {
            m_worker.request_stop();
            m_worker.join();
        }
    }

    void Bell::on_ring(const wlr_xdg_system_bell_v1_ring_event* event) {
        if (!event) {
            return;
        }
        if (auto* window = Window::try_from(event->surface)) {
            window->notify_urgent();
        }
        trigger_sound();
    }

    void Bell::trigger_sound() {
        if (m_compositor.is_locked()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_last_ring && (now - *m_last_ring) < bell_cooldown) {
            return;
        }
        m_last_ring = now;

        {
            const std::scoped_lock lock(m_mutex);
            m_pending_sound = true;
        }
        if (!m_worker.joinable()) {
            m_worker = std::jthread([this](const std::stop_token& stop) { worker_loop(stop); });
        } else {
            m_cv.notify_one();
        }
    }

    void Bell::worker_loop(const std::stop_token& stop) {
        ca_context* ctx = nullptr;
        ca_proplist* props = nullptr;

        while (true) {
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, stop, [this] { return m_pending_sound; });
                if (stop.stop_requested()) {
                    break;
                }
                m_pending_sound = false;
            }

            if (!ctx) {
                const int ctx_ret = ca_context_create(&ctx);
                if (ctx_ret != CA_SUCCESS || !ctx) {
                    log_error("failed to create libcanberra context: {}", ca_strerror(ctx_ret));
                    ctx = nullptr;
                    continue;
                }
                ca_context_change_props(ctx, CA_PROP_APPLICATION_NAME, "frost", CA_PROP_MEDIA_NAME, "System Bell",
                                        CA_PROP_EVENT_DESCRIPTION, "Wayland system bell", CA_PROP_MEDIA_ROLE, "event",
                                        nullptr);
                const int prop_ret = ca_proplist_create(&props);
                if (prop_ret != CA_SUCCESS || !props) {
                    log_error("failed to allocate libcanberra proplist: {}", ca_strerror(prop_ret));
                    ca_context_destroy(ctx);
                    ctx = nullptr;
                    props = nullptr;
                    continue;
                }
                ca_proplist_sets(props, CA_PROP_EVENT_ID, "bell-window-system");
            }

            const int ret = ca_context_play_full(ctx, 0, props, nullptr, nullptr);
            if (ret != CA_SUCCESS) {
                log_debug("bell playback failed: {}", ca_strerror(ret));
            }
        }

        if (props) {
            ca_proplist_destroy(props);
        }
        if (ctx) {
            ca_context_destroy(ctx);
        }
    }

}
