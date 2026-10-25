#include "flux/server.hpp"

#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string_view>

int main(int argc, char* argv[]) {
    wlr_log_init(WLR_DEBUG, nullptr);
    std::optional<std::filesystem::path> config_path;

    const std::span args(argv + 1, static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-c" && i + 1 < args.size()) {
            config_path = args[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::println("Usage: {} [-c config file]", argv[0]);
            return 0;
        } else {
            std::println("Usage: {} [-c config file]", argv[0]);
            return 1;
        }
    }

    flux::Server server;
    if (!server.init(config_path)) {
        return 1;
    }

    server.run();
    return 0;
}
