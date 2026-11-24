#include "frost/compositor.hpp"
#include "frost/log.hpp"

#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string_view>

int main(int argc, char* argv[]) {
    wlr_log_importance log_level = WLR_INFO;
    std::optional<std::filesystem::path> config_path;

    const auto print_usage = [prog = argv[0]] {
        std::println("Usage: {} [-c|--config file] [-d|--debug] [-v|--version] [-h|--help]", prog);
    };

    const std::span args(argv + 1, static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < args.size()) {
            config_path = args[++i];
        } else if (arg == "-d" || arg == "--debug") {
            log_level = WLR_DEBUG;
        } else if (arg == "-v" || arg == "-V" || arg == "--version") {
            std::println("frost {}", FROST_VERSION);
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            print_usage();
            return 1;
        }
    }

    frost::log_init(log_level);
    wlr_log(WLR_INFO, "starting version %s", FROST_VERSION);

    frost::Compositor compositor;
    if (!compositor.init(config_path)) {
        return 1;
    }

    compositor.run();
    return 0;
}
