#include "frost/ipc.hpp"
#include "frost/compositor.hpp"
#include "frost/log.hpp"
#include "frost/output.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace frost::ipc {

    using nlohmann::json;

    namespace {

        constexpr size_t max_clients = 64;
        constexpr size_t max_request_bytes = size_t{64} * 1024;
        constexpr int client_timeout_ms = 5000;

        std::string dump_json(const json& val) {
            return val.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        std::string make_ok(const json& data) {
            return dump_json(json{{"ok", true}, {"data", data}});
        }

        std::string make_error(std::string_view message) {
            return dump_json(json{{"ok", false}, {"error", message}});
        }

        std::string_view transform_to_string(wl_output_transform transform) noexcept {
            switch (transform) {
                case WL_OUTPUT_TRANSFORM_NORMAL:
                    return "normal";
                case WL_OUTPUT_TRANSFORM_90:
                    return "90";
                case WL_OUTPUT_TRANSFORM_180:
                    return "180";
                case WL_OUTPUT_TRANSFORM_270:
                    return "270";
                case WL_OUTPUT_TRANSFORM_FLIPPED:
                    return "flipped";
                case WL_OUTPUT_TRANSFORM_FLIPPED_90:
                    return "flipped-90";
                case WL_OUTPUT_TRANSFORM_FLIPPED_180:
                    return "flipped-180";
                case WL_OUTPUT_TRANSFORM_FLIPPED_270:
                    return "flipped-270";
            }
            return "normal";
        }

        json output_to_json(const Output* output, const Compositor& compositor) {
            const wlr_output* raw = output->raw();
            const wlr_box box = output->layout_box();
            const bool enabled = output->is_enabled();

            json physical_size = nullptr;
            if (raw->phys_width > 0 && raw->phys_height > 0) {
                physical_size = json::array({raw->phys_width, raw->phys_height});
            }

            json logical = nullptr;
            if (enabled && box.width > 0 && box.height > 0) {
                logical = {
                    {"x", box.x},           {"y", box.y},          {"width", box.width},
                    {"height", box.height}, {"scale", raw->scale}, {"transform", transform_to_string(raw->transform)},
                };
            }

            json modes = json::array();
            std::optional<size_t> current_mode;
            wlr_output_mode* mode = nullptr;
            wl_list_for_each(mode, &raw->modes, link) {
                if (enabled && mode == raw->current_mode) {
                    current_mode = modes.size();
                }
                modes.push_back({
                    {"width", mode->width},
                    {"height", mode->height},
                    {"refresh_mhz", mode->refresh},
                    {"preferred", mode->preferred},
                });
            }

            return {
                {"name", raw->name ? raw->name : ""},
                {"make", raw->make ? raw->make : ""},
                {"model", raw->model ? raw->model : ""},
                {"serial", (raw->serial && *raw->serial != '\0') ? json(raw->serial) : json(nullptr)},
                {"physical_size", std::move(physical_size)},
                {"enabled", enabled},
                {"focused", output == compositor.focused_output()},
                {"logical", std::move(logical)},
                {"modes", std::move(modes)},
                {"current_mode", current_mode ? json(*current_mode) : json(nullptr)},
            };
        }

    }

    struct Server::Client {
        Server* server = nullptr;
        int fd = -1;
        wl_event_source* fd_source = nullptr;
        wl_event_source* timeout_source = nullptr;
        std::string read_buf;
        std::string write_buf;
        size_t write_offset = 0;

        Client() = default;
        ~Client() {
            if (timeout_source) {
                wl_event_source_remove(timeout_source);
            }
            if (fd_source) {
                wl_event_source_remove(fd_source);
            }
            if (fd >= 0) {
                close(fd);
            }
        }

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        Client(Client&&) = delete;
        Client& operator=(Client&&) = delete;
    };

    Server::Server(Compositor& compositor, std::string_view wayland_display) : m_compositor(compositor) {
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime_dir || *runtime_dir == '\0') {
            log_error("IPC disabled: XDG_RUNTIME_DIR is not set");
            return;
        }

        m_socket_path = std::filesystem::path(runtime_dir) / std::format("frost.{}.{}.sock", wayland_display, getpid());

        sockaddr_un addr{};
        if (m_socket_path.native().size() >= sizeof(addr.sun_path)) {
            log_error("IPC socket path exceeds sun_path limit: '{}'", m_socket_path.c_str());
            m_socket_path.clear();
            return;
        }

        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            log_error("failed to create IPC socket: {}", std::strerror(errno));
            m_socket_path.clear();
            return;
        }

        ::unlink(m_socket_path.c_str());

        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, m_socket_path.native().data(), m_socket_path.native().size());

        if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            log_error("failed to bind IPC socket '{}': {}", m_socket_path.c_str(), std::strerror(errno));
            close(fd);
            m_socket_path.clear();
            return;
        }

        chmod(m_socket_path.c_str(), 0600);

        if (listen(fd, 16) < 0) {
            log_error("failed to listen on IPC socket '{}': {}", m_socket_path.c_str(), std::strerror(errno));
            close(fd);
            ::unlink(m_socket_path.c_str());
            m_socket_path.clear();
            return;
        }

        wl_event_loop* event_loop = wl_display_get_event_loop(m_compositor.display());
        m_listen_fd = fd;
        m_listen_source = wl_event_loop_add_fd(
            event_loop, m_listen_fd, WL_EVENT_READABLE,
            [](int, uint32_t, void* data) {
                static_cast<Server*>(data)->on_accept();
                return 0;
            },
            this);

        if (!m_listen_source) {
            log_error("failed to add IPC socket to event loop");
            close(m_listen_fd);
            m_listen_fd = -1;
            ::unlink(m_socket_path.c_str());
            m_socket_path.clear();
        }
    }

    Server::~Server() {
        m_clients.clear();
        if (m_listen_source) {
            wl_event_source_remove(m_listen_source);
            m_listen_source = nullptr;
        }
        if (m_listen_fd >= 0) {
            close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (!m_socket_path.empty()) {
            ::unlink(m_socket_path.c_str());
        }
    }

    void Server::on_accept() {
        wl_event_loop* event_loop = wl_display_get_event_loop(m_compositor.display());

        while (true) {
            const int client_fd = accept4(m_listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (m_clients.size() >= max_clients) {
                close(client_fd);
                continue;
            }

            auto client = std::make_unique<Client>();
            client->server = this;
            client->fd = client_fd;
            Client* raw_client = client.get();

            client->fd_source = wl_event_loop_add_fd(
                event_loop, client_fd, WL_EVENT_READABLE,
                [](int, uint32_t mask, void* data) {
                    auto* c = static_cast<Client*>(data);
                    c->server->on_client_event(c, mask);
                    return 0;
                },
                raw_client);

            client->timeout_source = wl_event_loop_add_timer(
                event_loop,
                [](void* data) {
                    auto* c = static_cast<Client*>(data);
                    c->server->remove_client(c);
                    return 0;
                },
                raw_client);

            if (!client->fd_source || !client->timeout_source) {
                continue;
            }

            wl_event_source_timer_update(client->timeout_source, client_timeout_ms);
            m_clients.push_back(std::move(client));
        }
    }

    void Server::on_client_event(Client* client, uint32_t mask) {
        bool eof = false;
        bool io_error = false;

        if (client->write_buf.empty() && (mask & WL_EVENT_READABLE) != 0) {
            std::array<char, 4096> tmp{};
            while (true) {
                const ssize_t n = recv(client->fd, tmp.data(), tmp.size(), 0);
                if (n > 0) {
                    if (client->read_buf.size() + static_cast<size_t>(n) > max_request_bytes) {
                        client->write_buf = make_error("request too large");
                        client->write_buf.push_back('\n');
                        client->read_buf.clear();
                        break;
                    }
                    client->read_buf.append(tmp.data(), static_cast<size_t>(n));
                    if (client->read_buf.contains('\n')) {
                        break;
                    }
                    continue;
                }
                if (n == 0) {
                    eof = true;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                io_error = true;
                break;
            }

            if (client->write_buf.empty()) {
                if (const size_t nl = client->read_buf.find('\n'); nl != std::string::npos) {
                    std::string line = client->read_buf.substr(0, nl);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    client->read_buf.clear();
                    client->write_buf = handle_request(line);
                    client->write_buf.push_back('\n');
                } else if (eof && !client->read_buf.empty()) {
                    if (client->read_buf.back() == '\r') {
                        client->read_buf.pop_back();
                    }
                    client->write_buf = handle_request(client->read_buf);
                    client->write_buf.push_back('\n');
                    client->read_buf.clear();
                }
            }
        }

        if (io_error || (mask & WL_EVENT_ERROR) != 0) {
            remove_client(client);
            return;
        }

        if (!client->write_buf.empty()) {
            if (!flush_write(client) || client->write_buf.empty() || (mask & WL_EVENT_HANGUP) != 0) {
                remove_client(client);
                return;
            }
            wl_event_source_fd_update(client->fd_source, WL_EVENT_WRITABLE);
            wl_event_source_timer_update(client->timeout_source, client_timeout_ms);
            return;
        }

        if (eof || (mask & WL_EVENT_HANGUP) != 0) {
            remove_client(client);
            return;
        }
        wl_event_source_timer_update(client->timeout_source, client_timeout_ms);
    }

    void Server::remove_client(Client* client) {
        std::erase_if(m_clients, [client](const auto& c) { return c.get() == client; });
    }

    bool Server::flush_write(Client* client) {
        while (client->write_offset < client->write_buf.size()) {
            const char* ptr = client->write_buf.data() + client->write_offset;
            const size_t remaining = client->write_buf.size() - client->write_offset;
            const ssize_t n = send(client->fd, ptr, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                client->write_offset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            return false;
        }

        client->write_buf.clear();
        client->write_offset = 0;
        return true;
    }

    std::string Server::handle_request(std::string_view line) {
        try {
            const json req = json::parse(line);
            if (!req.is_object()) {
                return make_error("request must be a JSON object");
            }
            auto cmd_it = req.find("cmd");
            if (cmd_it == req.end() || !cmd_it->is_string()) {
                return make_error("missing or invalid 'cmd' field");
            }

            const std::string& cmd = cmd_it->get_ref<const std::string&>();
            if (cmd == "outputs") {
                json list = json::array();
                for (const auto& out : m_compositor.outputs()) {
                    list.push_back(output_to_json(out.get(), m_compositor));
                }
                return make_ok(list);
            }
            if (cmd == "version") {
                return make_ok(json{{"version", FROST_VERSION}});
            }

            return make_error(std::format("unknown command '{}'", cmd));
        } catch (const std::exception& e) {
            return make_error(std::format("invalid JSON request: {}", e.what()));
        }
    }

}
