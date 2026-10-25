#pragma once

#include "flux/wlroots.hpp"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace flux {

    class IListener {
    public:
        virtual ~IListener() = default;
    };

    template <typename EventType = void>
    class WlListener final : public IListener {
    public:
        using ParamType = std::conditional_t<std::is_void_v<EventType>, void*, EventType*>;

        WlListener() {
            m_node.self = this;
            wl_list_init(&m_node.listener.link);
            m_node.listener.notify = &WlListener::handle_notify;
        }

        template <typename F>
        WlListener(wl_signal* signal, F&& callback) : WlListener() {
            m_callback = std::forward<F>(callback);
            connect(signal);
        }

        ~WlListener() override { disconnect(); }

        WlListener(const WlListener&) = delete;
        WlListener& operator=(const WlListener&) = delete;

        void connect(wl_signal* signal) {
            disconnect();
            wl_signal_add(signal, &m_node.listener);
        }

        void disconnect() {
            if (is_connected()) {
                wl_list_remove(&m_node.listener.link);
                wl_list_init(&m_node.listener.link);
            }
        }

        [[nodiscard]] bool is_connected() const noexcept { return !wl_list_empty(&m_node.listener.link); }

    private:
        struct ListenerNode {
            wl_listener listener{};
            void* self = nullptr;
        };

        static void handle_notify(wl_listener* listener, void* data) {
            auto* node = reinterpret_cast<ListenerNode*>(listener);
            auto* self = static_cast<WlListener*>(node->self);
            if (self && self->m_callback) {
                if constexpr (std::is_void_v<EventType>) {
                    self->m_callback(data);
                } else {
                    self->m_callback(static_cast<EventType*>(data));
                }
            }
        }

        ListenerNode m_node{};
        std::function<void(ParamType)> m_callback;
    };

    class ListenerGroup {
    public:
        ListenerGroup() = default;
        ~ListenerGroup() = default;

        ListenerGroup(const ListenerGroup&) = delete;
        ListenerGroup& operator=(const ListenerGroup&) = delete;
        ListenerGroup(ListenerGroup&&) noexcept = default;
        ListenerGroup& operator=(ListenerGroup&&) noexcept = default;

        template <typename EventType = void, typename F>
        void listen(wl_signal* signal, F&& callback) {
            m_listeners.push_back(std::make_unique<WlListener<EventType>>(signal, std::forward<F>(callback)));
        }

        template <typename Owner, typename EventType = void>
        void listen(wl_signal* signal, Owner* owner, void (Owner::*method)()) {
            listen<EventType>(signal, [owner, method](auto*) { (owner->*method)(); });
        }

        template <typename Owner, typename EventType>
        void listen(wl_signal* signal, Owner* owner, void (Owner::*method)(EventType*)) {
            listen<EventType>(signal, [owner, method](EventType* event) { (owner->*method)(event); });
        }

        template <typename Owner, typename EventType>
        void listen(wl_signal* signal, Owner* owner, void (Owner::*method)(const EventType*)) {
            listen<const EventType>(signal, [owner, method](const EventType* event) { (owner->*method)(event); });
        }

        void clear() noexcept { m_listeners.clear(); }

    private:
        std::vector<std::unique_ptr<IListener>> m_listeners;
    };

}
