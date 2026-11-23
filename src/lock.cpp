#include "frost/lock.hpp"
#include "frost/compositor.hpp"
#include "frost/input.hpp"
#include "frost/output.hpp"
#include "frost/scene.hpp"

#include <utility>

namespace frost {

    Lock::Lock(Compositor& compositor) : m_compositor(compositor) {
        auto* manager = wlr_session_lock_manager_v1_create(m_compositor.display());
        m_listeners.listen(&manager->events.new_lock, this, &Lock::on_new_lock);
    }

    Lock::~Lock() {
        clear_timer();
        m_lock_listeners.clear();
        for (const auto& out : m_compositor.outputs()) {
            out->destroy_lock_surface();
        }
    }

    void Lock::on_new_lock(wlr_session_lock_v1* lock) {
        if (m_lock) {
            wlr_log(WLR_ERROR, "session lock already active, rejecting new lock");
            wlr_session_lock_v1_destroy(lock);
            return;
        }

        m_lock = lock;
        m_locked_sent = false;
        m_unlocked = false;

        if (m_persistent) {
            wlr_log(WLR_INFO, "session lock client attached to existing locked session");
        } else {
            m_compositor.scene().set_session_lock_active(true);
            m_compositor.scene().update_lock_background(m_compositor.outputs());
            m_compositor.cancel_animations();
            m_compositor.refocus();
        }
        m_compositor.dismiss_popups();

        m_lock_listeners.listen(&lock->events.new_surface, this, &Lock::on_new_surface);
        m_lock_listeners.listen(&lock->events.unlock, this, &Lock::on_unlock);
        m_lock_listeners.listen(&lock->events.destroy, this, &Lock::on_destroy);

        try_send_locked();

        if (!m_locked_sent) {
            wl_event_loop* loop = wl_display_get_event_loop(m_compositor.display());
            if (loop) {
                m_timer = wl_event_loop_add_timer(
                    loop,
                    [](void* data) -> int {
                        auto* self = static_cast<Lock*>(data);
                        self->send_locked();
                        return 0;
                    },
                    this);
                if (m_timer) {
                    wl_event_source_timer_update(m_timer, 1000);
                }
            }
        }
    }

    void Lock::on_new_surface(wlr_session_lock_surface_v1* lock_surface) {
        Output* output = m_compositor.output_from_wlr(lock_surface->output);
        if (!output) {
            wlr_log(WLR_ERROR, "lock surface requested for unknown output");
            return;
        }
        if (output->lock_surface()) {
            wlr_log(WLR_ERROR, "duplicate lock surface requested for output");
            return;
        }
        output->set_lock_surface(lock_surface);
    }

    void Lock::on_unlock() {
        m_unlocked = true;
    }

    void Lock::on_destroy() {
        m_lock_listeners.clear();
        clear_timer();
        m_lock = nullptr;
        m_locked_sent = false;
        const bool unlocked = std::exchange(m_unlocked, false);

        for (const auto& out : m_compositor.outputs()) {
            out->destroy_lock_surface();
        }

        if (unlocked || !m_persistent) {
            m_persistent = false;
            m_compositor.scene().set_session_lock_active(false);
        } else {
            wlr_log(WLR_INFO, "session lock client died; session remains locked");
        }
        m_compositor.refocus();
    }

    void Lock::clear_timer() noexcept {
        if (m_timer) {
            wl_event_source_remove(m_timer);
            m_timer = nullptr;
        }
    }

    void Lock::try_send_locked() {
        if (m_locked_sent || !m_lock) {
            return;
        }
        for (const auto& out : m_compositor.outputs()) {
            if (!out->is_enabled()) {
                continue;
            }
            if (!out->is_lock_surface_mapped()) {
                return;
            }
        }
        send_locked();
    }

    void Lock::send_locked() {
        if (m_locked_sent || !m_lock) {
            return;
        }
        m_locked_sent = true;
        m_persistent = true;
        clear_timer();
        wlr_session_lock_v1_send_locked(m_lock);
    }

    void Lock::sync() {
        if (!is_locked()) {
            return;
        }
        m_compositor.scene().update_lock_background(m_compositor.outputs());
        for (const auto& out : m_compositor.outputs()) {
            out->configure_lock_surface();
        }
        try_send_locked();
    }

    wlr_surface* Lock::target_surface() noexcept {
        if (!m_lock) {
            return nullptr;
        }
        Output* focused = m_compositor.focused_output();
        m_compositor.set_focused_output(focused);
        if (focused && focused->is_lock_surface_mapped()) {
            return focused->lock_surface()->surface;
        }
        wlr_surface* cur = m_compositor.input().seat().focused_surface();
        if (const auto* out = output_from_surface(cur); out && out->is_enabled()) {
            return cur;
        }
        for (const auto& out : m_compositor.outputs()) {
            if (out->is_enabled() && out->is_lock_surface_mapped()) {
                return out->lock_surface()->surface;
            }
        }
        return nullptr;
    }

    Output* Lock::output_from_surface(wlr_surface* surface) const noexcept {
        if (!surface) {
            return nullptr;
        }
        wlr_surface* root = wlr_surface_get_root_surface(surface);
        for (const auto& out : m_compositor.outputs()) {
            if (out->lock_surface() && out->lock_surface()->surface == root) {
                return out.get();
            }
        }
        return nullptr;
    }

}
