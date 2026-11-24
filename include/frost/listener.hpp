#pragma once

#include "frost/wlroots.hpp"

#include <deque>
#include <functional>
#include <type_traits>
#include <utility>

namespace frost {

    class WlListener final {
    public:
        using Callback = std::move_only_function<void(void*)>;

        WlListener(wl_signal* signal, Callback callback)
            : m_hook{.listener = {}, .owner = this}, m_callback(std::move(callback)) {
            m_hook.listener.notify = &WlListener::handle_notify;
            wl_signal_add(signal, &m_hook.listener);
        }

        ~WlListener() { wl_list_remove(&m_hook.listener.link); }

        WlListener(const WlListener&) = delete;
        WlListener& operator=(const WlListener&) = delete;
        WlListener(WlListener&&) = delete;
        WlListener& operator=(WlListener&&) = delete;

    private:
        struct Hook {
            wl_listener listener{};
            WlListener* owner = nullptr;
        };
        static_assert(std::is_standard_layout_v<Hook>);

        static void handle_notify(wl_listener* listener, void* data) {
            auto* hook = reinterpret_cast<Hook*>(listener);
            if (hook->owner && hook->owner->m_callback) {
                hook->owner->m_callback(data);
            }
        }

        Hook m_hook{};
        Callback m_callback;
    };

    class ListenerGroup {
    public:
        ListenerGroup() = default;
        ~ListenerGroup() = default;

        ListenerGroup(const ListenerGroup&) = delete;
        ListenerGroup& operator=(const ListenerGroup&) = delete;
        ListenerGroup(ListenerGroup&&) noexcept = delete;
        ListenerGroup& operator=(ListenerGroup&&) noexcept = delete;

        template <typename F>
        void listen(wl_signal* signal, F&& callback) {
            m_listeners.emplace_back(signal, std::forward<F>(callback));
        }

        template <typename Owner, typename Base>
        void listen(wl_signal* signal, Owner* owner, void (Base::*method)()) {
            listen(signal, [owner, method](void*) { (owner->*method)(); });
        }

        template <typename Owner, typename Base, typename EventType>
        void listen(wl_signal* signal, Owner* owner, void (Base::*method)(EventType*)) {
            listen(signal, [owner, method](void* data) { (owner->*method)(static_cast<EventType*>(data)); });
        }

        void clear() noexcept { m_listeners.clear(); }

    private:
        std::deque<WlListener> m_listeners;
    };

}
