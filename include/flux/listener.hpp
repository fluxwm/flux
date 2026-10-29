#pragma once

#include "flux/wlroots.hpp"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace flux {

    class WlListener final {
    public:
        using Callback = std::function<void(void*)>;

        WlListener(wl_signal* signal, Callback callback) : m_callback(std::move(callback)) {
            m_listener.notify = &WlListener::handle_notify;
            wl_signal_add(signal, &m_listener);
        }

        ~WlListener() { wl_list_remove(&m_listener.link); }

        WlListener(const WlListener&) = delete;
        WlListener& operator=(const WlListener&) = delete;

    private:
        static void handle_notify(wl_listener* listener, void* data) {
            auto* self = reinterpret_cast<WlListener*>(listener);
            if (self->m_callback) {
                self->m_callback(data);
            }
        }

        wl_listener m_listener{};
        Callback m_callback;
    };

    class ListenerGroup {
    public:
        ListenerGroup() = default;
        ~ListenerGroup() = default;

        ListenerGroup(const ListenerGroup&) = delete;
        ListenerGroup& operator=(const ListenerGroup&) = delete;
        ListenerGroup(ListenerGroup&&) noexcept = default;
        ListenerGroup& operator=(ListenerGroup&&) noexcept = default;

        template <typename F>
        void listen(wl_signal* signal, F&& callback) {
            m_listeners.push_back(std::make_unique<WlListener>(signal, std::forward<F>(callback)));
        }

        template <typename Owner>
        void listen(wl_signal* signal, Owner* owner, void (Owner::*method)()) {
            listen(signal, [owner, method](void*) { (owner->*method)(); });
        }

        template <typename Owner, typename EventType>
        void listen(wl_signal* signal, Owner* owner, void (Owner::*method)(EventType*)) {
            listen(signal, [owner, method](void* data) { (owner->*method)(static_cast<EventType*>(data)); });
        }

        void clear() noexcept { m_listeners.clear(); }

    private:
        std::vector<std::unique_ptr<WlListener>> m_listeners;
    };

}
