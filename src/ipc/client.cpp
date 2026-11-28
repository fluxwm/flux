#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

    using nlohmann::json;

    constexpr size_t max_response_bytes = size_t{16} * 1024 * 1024;
    constexpr timeval io_timeout{.tv_sec = 5, .tv_usec = 0};

    struct UniqueFd {
        int fd = -1;

        UniqueFd() = default;
        explicit UniqueFd(int f) noexcept : fd(f) {}
        ~UniqueFd() {
            if (fd >= 0) {
                close(fd);
            }
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;
        UniqueFd(UniqueFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
        UniqueFd& operator=(UniqueFd&&) = delete;

        [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
    };

    UniqueFd connect_unix(const std::filesystem::path& path, std::string& error) {
        sockaddr_un addr{};
        if (path.native().size() >= sizeof(addr.sun_path)) {
            error = "socket path exceeds sun_path limit";
            return {};
        }

        UniqueFd sock(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!sock.valid()) {
            error = std::strerror(errno);
            return {};
        }

        setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
        setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.native().data(), path.native().size());

        if (connect(sock.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            error = std::strerror(errno);
            return {};
        }

        return sock;
    }

    UniqueFd resolve_and_connect(std::string& error) {
        if (const char* env_sock = std::getenv("FROST_SOCKET"); env_sock && *env_sock != '\0') {
            return connect_unix(std::filesystem::path(env_sock), error);
        }

        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime_dir || *runtime_dir == '\0') {
            error = "FROST_SOCKET and XDG_RUNTIME_DIR are not set";
            return {};
        }

        std::vector<std::pair<UniqueFd, std::filesystem::path>> live;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(runtime_dir, ec)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("frost.") || !name.ends_with(".sock")) {
                continue;
            }
            if (!std::filesystem::is_socket(entry.status(ec))) {
                continue;
            }
            std::string ignore;
            UniqueFd fd = connect_unix(entry.path(), ignore);
            if (fd.valid()) {
                live.emplace_back(std::move(fd), entry.path());
            }
        }

        if (live.empty()) {
            error = "no running Frost IPC socket found (set FROST_SOCKET)";
            return {};
        }
        if (live.size() > 1) {
            error = "multiple Frost instances running (set FROST_SOCKET):";
            for (const auto& [_, path] : live) {
                error.append("\n  ");
                error.append(path.string());
            }
            return {};
        }

        return std::move(live.front().first);
    }

    bool write_all(int fd, std::string_view data, std::string& error) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = std::strerror(errno);
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    std::optional<std::string> read_line(int fd, std::string& error) {
        std::string buf;
        std::array<char, 4096> tmp{};
        while (true) {
            const ssize_t n = recv(fd, tmp.data(), tmp.size(), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = std::strerror(errno);
                return std::nullopt;
            }
            if (n == 0) {
                break;
            }
            if (buf.size() + static_cast<size_t>(n) > max_response_bytes) {
                error = "IPC response exceeded size limit";
                return std::nullopt;
            }
            buf.append(tmp.data(), static_cast<size_t>(n));
            if (const size_t nl = buf.find('\n'); nl != std::string::npos) {
                buf.resize(nl);
                return buf;
            }
        }
        if (buf.empty()) {
            error = "unexpected EOF from IPC server";
            return std::nullopt;
        }
        return buf;
    }

    std::string_view get_string(const json& obj, std::string_view key, std::string_view fallback = {}) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_string()) {
            return it->get_ref<const std::string&>();
        }
        return fallback;
    }

    bool get_bool(const json& obj, std::string_view key, bool fallback = false) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_boolean()) {
            return it->get<bool>();
        }
        return fallback;
    }

    std::optional<int64_t> get_int(const json& obj, std::string_view key) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number_integer()) {
            return it->get<int64_t>();
        }
        return std::nullopt;
    }

    std::optional<double> get_number(const json& obj, std::string_view key) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number()) {
            return it->get<double>();
        }
        return std::nullopt;
    }

    void print_output(const json& out) {
        const std::string_view name = get_string(out, "name");
        std::string hw;
        for (const std::string_view part :
             {get_string(out, "make"), get_string(out, "model"), get_string(out, "serial")}) {
            if (!part.empty()) {
                if (!hw.empty()) {
                    hw.push_back(' ');
                }
                hw.append(part);
            }
        }

        std::string flags;
        if (get_bool(out, "focused")) {
            flags.append(" [focused]");
        }
        if (!get_bool(out, "enabled", true)) {
            flags.append(" [disabled]");
        }

        if (hw.empty()) {
            std::println("Output \"{}\"{}", name, flags);
        } else {
            std::println("Output \"{}\" ({}){}", name, hw, flags);
        }

        const auto modes_it = out.find("modes");
        const bool has_modes = modes_it != out.end() && modes_it->is_array() && !modes_it->empty();
        const std::optional<int64_t> cur_idx = get_int(out, "current_mode");

        if (has_modes && cur_idx && *cur_idx >= 0 && std::cmp_less(*cur_idx, modes_it->size())) {
            const auto& m = (*modes_it)[static_cast<size_t>(*cur_idx)];
            if (m.is_object()) {
                const int64_t w = get_int(m, "width").value_or(0);
                const int64_t h = get_int(m, "height").value_or(0);
                const int64_t refresh = get_int(m, "refresh_mhz").value_or(0);
                if (refresh > 0) {
                    std::println("  Current mode: {}x{} @ {:.3f} Hz", w, h, static_cast<double>(refresh) / 1000.0);
                } else {
                    std::println("  Current mode: {}x{}", w, h);
                }
            }
        }

        const auto logical_it = out.find("logical");
        const bool has_logical = logical_it != out.end() && logical_it->is_object();

        if (has_logical) {
            const auto& logical = *logical_it;
            std::println("  Logical position: {}, {}", get_int(logical, "x").value_or(0),
                         get_int(logical, "y").value_or(0));
            std::println("  Logical size: {}x{}", get_int(logical, "width").value_or(0),
                         get_int(logical, "height").value_or(0));
        }

        if (auto it = out.find("physical_size"); it != out.end() && it->is_array() && it->size() == 2 &&
                                                 (*it)[0].is_number_integer() && (*it)[1].is_number_integer()) {
            std::println("  Physical size: {}x{} mm", (*it)[0].get<int64_t>(), (*it)[1].get<int64_t>());
        }

        if (has_logical) {
            const auto& logical = *logical_it;
            std::println("  Scale: {:g}", get_number(logical, "scale").value_or(1.0));
            std::println("  Transform: {}", get_string(logical, "transform", "normal"));
        }

        if (has_modes) {
            std::println("  Available modes:");
            for (size_t i = 0; i < modes_it->size(); ++i) {
                const auto& m = (*modes_it)[i];
                if (!m.is_object()) {
                    continue;
                }
                const int64_t w = get_int(m, "width").value_or(0);
                const int64_t h = get_int(m, "height").value_or(0);
                const int64_t refresh = get_int(m, "refresh_mhz").value_or(0);
                const bool is_cur = cur_idx && std::cmp_equal(i, *cur_idx);
                const bool is_pref = get_bool(m, "preferred");

                std::string_view suffix;
                if (is_cur && is_pref) {
                    suffix = " (current, preferred)";
                } else if (is_cur) {
                    suffix = " (current)";
                } else if (is_pref) {
                    suffix = " (preferred)";
                }

                if (refresh > 0) {
                    std::println("    {}x{}@{:.3f}{}", w, h, static_cast<double>(refresh) / 1000.0, suffix);
                } else {
                    std::println("    {}x{}{}", w, h, suffix);
                }
            }
        }
    }

    void print_usage(std::string_view prog) {
        std::println("Usage: {} [-j|--json] [-h|--help] <command>", prog);
        std::println("");
        std::println("Commands:");
        std::println("  outputs  List outputs");
        std::println("  version  Show compositor version");
    }

}

int main(int argc, char* argv[]) {
    bool json_output = false;
    std::optional<std::string_view> cmd;
    const std::string_view prog = (argc > 0 && argv[0]) ? argv[0] : "frostctl";

    const std::span raw_args(argv + 1, static_cast<size_t>(argc > 1 ? argc - 1 : 0));
    for (const char* raw : raw_args) {
        const std::string_view arg = raw;
        if (arg == "-j" || arg == "--json") {
            json_output = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(prog);
            return 0;
        } else if (arg.starts_with('-')) {
            std::println(stderr, "frostctl: unknown option '{}'", arg);
            print_usage(prog);
            return 1;
        } else if (!cmd) {
            cmd = arg;
        } else {
            std::println(stderr, "frostctl: unexpected argument '{}'", arg);
            print_usage(prog);
            return 1;
        }
    }

    if (!cmd) {
        print_usage(prog);
        return 1;
    }

    if (*cmd != "outputs" && *cmd != "version") {
        std::println(stderr, "frostctl: unknown command '{}'", *cmd);
        print_usage(prog);
        return 1;
    }

    std::string error;
    UniqueFd sock = resolve_and_connect(error);
    if (!sock.valid()) {
        std::println(stderr, "frostctl: {}", error);
        return 1;
    }

    const json request{{"cmd", *cmd}};
    std::string payload = request.dump(-1, ' ', false, json::error_handler_t::replace);
    payload.push_back('\n');

    if (!write_all(sock.fd, payload, error)) {
        std::println(stderr, "frostctl: write failed: {}", error);
        return 1;
    }

    const auto line = read_line(sock.fd, error);
    if (!line) {
        std::println(stderr, "frostctl: read failed: {}", error);
        return 1;
    }

    json response;
    try {
        response = json::parse(*line);
    } catch (const std::exception& e) {
        std::println(stderr, "frostctl: invalid JSON response: {}", e.what());
        return 1;
    }

    if (!response.is_object() || !response.contains("ok") || !response["ok"].is_boolean()) {
        std::println(stderr, "frostctl: invalid IPC response");
        return 1;
    }

    if (!response["ok"].get<bool>()) {
        std::println(stderr, "frostctl: {}", get_string(response, "error", "unknown error"));
        return 1;
    }

    if (json_output) {
        if (response.contains("data")) {
            std::println("{}", response["data"].dump(-1, ' ', false, json::error_handler_t::replace));
        } else {
            std::println("{}", response.dump(-1, ' ', false, json::error_handler_t::replace));
        }
    } else if (*cmd == "outputs") {
        if (!response.contains("data") || !response["data"].is_array()) {
            std::println(stderr, "frostctl: invalid outputs response");
            return 1;
        }
        const auto& outputs = response["data"];
        if (outputs.empty()) {
            std::println("No outputs.");
        } else {
            bool first = true;
            for (const auto& out : outputs) {
                if (!out.is_object()) {
                    continue;
                }
                if (!first) {
                    std::println("");
                }
                first = false;
                print_output(out);
            }
        }
    } else if (*cmd == "version") {
        if (!response.contains("data") || !response["data"].is_object() ||
            get_string(response["data"], "version").empty()) {
            std::println(stderr, "frostctl: invalid version response");
            return 1;
        }
        std::println("frost {}", get_string(response["data"], "version"));
    }

    return 0;
}
