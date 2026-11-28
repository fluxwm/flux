#pragma once

#include "frost/wlroots.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost {

    class Compositor;

    namespace ipc {

        class Server {
        public:
            Server(Compositor& compositor, std::string_view wayland_display);
            ~Server();

            Server(const Server&) = delete;
            Server& operator=(const Server&) = delete;
            Server(Server&&) = delete;
            Server& operator=(Server&&) = delete;

            [[nodiscard]] bool is_running() const noexcept { return m_listen_fd >= 0; }
            [[nodiscard]] const std::filesystem::path& socket_path() const noexcept { return m_socket_path; }

        private:
            struct Client;

            void on_accept();
            void on_client_event(Client* client, uint32_t mask);
            void remove_client(Client* client);
            bool flush_write(Client* client);
            [[nodiscard]] std::string handle_request(std::string_view line);

            Compositor& m_compositor;
            std::filesystem::path m_socket_path;
            int m_listen_fd = -1;
            wl_event_source* m_listen_source = nullptr;
            std::vector<std::unique_ptr<Client>> m_clients;
        };

    }

}
