#include "frost/compositor.hpp"
#include "frost/bell.hpp"
#include "frost/input.hpp"
#include "frost/ipc.hpp"
#include "frost/layer.hpp"
#include "frost/log.hpp"
#include "frost/output.hpp"
#include "frost/popup.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/window/xdg.hpp"
#include "frost/window/xwayland.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace frost {

    namespace {

        struct CFree {
            void operator()(void* ptr) const noexcept {
                std::free(ptr); // NOLINT(cppcoreguidelines-no-malloc)
            }
        };

        bool is_nested_backend(wlr_backend* backend) noexcept {
            if (!backend) {
                return false;
            }
            if (!wlr_backend_is_multi(backend)) {
                return wlr_backend_is_wl(backend) || wlr_backend_is_x11(backend);
            }
            bool nested = false;
            wlr_multi_for_each_backend(
                backend,
                [](wlr_backend* sub, void* data) {
                    if (wlr_backend_is_wl(sub) || wlr_backend_is_x11(sub)) {
                        *static_cast<bool*>(data) = true;
                    }
                },
                &nested);
            return nested;
        }

        bool is_valid_activation_token(const wlr_xdg_activation_token_v1* token) noexcept {
            if (!token || !token->seat) {
                return false;
            }
            auto client_matches_serial = [token](wlr_seat_client* seat_client) {
                if (!seat_client) {
                    return false;
                }
                if (token->surface && (!token->surface->resource ||
                                       wl_resource_get_client(token->surface->resource) != seat_client->client)) {
                    return false;
                }
                return wlr_seat_client_validate_event_serial(seat_client, token->serial);
            };
            return client_matches_serial(token->seat->keyboard_state.focused_client) ||
                   client_matches_serial(token->seat->pointer_state.focused_client);
        }

    }

    Compositor::Compositor() = default;

    Compositor::~Compositor() {
        cancel_animations();
        if (m_input) {
            m_input->cursor().clear_grab();
        }
        m_lock.reset();

        if (m_xwayland) {
            m_xwayland_listeners.clear();
            wlr_xwayland_destroy(m_xwayland);
            m_xwayland = nullptr;
        }
        if (m_display) {
            wl_display_destroy_clients(m_display);
        }

        m_listeners.clear();
        if (m_output_manager) {
            m_output_manager->clear_listeners();
        }

        while (!m_popups.empty()) {
            auto p = std::move(m_popups.back());
            m_popups.pop_back();
        }
        m_windows.clear();
        if (m_output_manager) {
            m_output_manager->clear_outputs();
        }
        m_bell.reset();
        m_input.reset();
        m_ipc.reset();
        m_config.reset();

        m_scene.reset();
        m_output_manager.reset();

        if (m_backend) {
            wlr_backend_destroy(m_backend);
        }
        if (m_allocator) {
            wlr_allocator_destroy(m_allocator);
        }
        if (m_renderer) {
            wlr_renderer_destroy(m_renderer);
        }
        for (wl_event_source* src : m_signal_sources) {
            wl_event_source_remove(src);
        }
        m_signal_sources.clear();
        if (m_display) {
            wl_display_destroy(m_display);
        }
    }

    bool Compositor::init(std::optional<std::filesystem::path> config_path) {
        m_display = wl_display_create();
        if (!m_display) {
            log_error("failed to create wl_display");
            return false;
        }

        wl_event_loop* event_loop = wl_display_get_event_loop(m_display);
        auto on_signal = [](int, void* data) {
            static_cast<Compositor*>(data)->quit();
            return 0;
        };
        for (const int sig : {SIGINT, SIGTERM}) {
            if (wl_event_source* src = wl_event_loop_add_signal(event_loop, sig, on_signal, this)) {
                m_signal_sources.push_back(src);
            }
        }
        auto on_sigchld = [](int, void* data) {
            auto* self = static_cast<Compositor*>(data);
            std::erase_if(self->m_spawned_pids, [](pid_t pid) { return waitpid(pid, nullptr, WNOHANG) != 0; });
            return 0;
        };
        if (wl_event_source* src = wl_event_loop_add_signal(event_loop, SIGCHLD, on_sigchld, this)) {
            m_signal_sources.push_back(src);
        }

        m_backend = wlr_backend_autocreate(event_loop, &m_session);
        if (!m_backend) {
            log_error("failed to create wlr_backend");
            return false;
        }

        if (is_nested_backend(m_backend)) {
            m_mod_key = WLR_MODIFIER_ALT;
            log_info("running nested, using Alt as Mod key");
        }

        m_config = std::make_unique<Config>(event_loop, config_path, m_mod_key);

        m_renderer = wlr_renderer_autocreate(m_backend);
        if (!m_renderer) {
            log_error("failed to create wlr_renderer");
            return false;
        }

        wlr_renderer_init_wl_shm(m_renderer, m_display);

        m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
        if (!m_allocator) {
            log_error("failed to create wlr_allocator");
            return false;
        }

        auto* wlr_comp = wlr_compositor_create(m_display, 6, m_renderer);
        wlr_subcompositor_create(m_display);
        wlr_data_device_manager_create(m_display);
        wlr_primary_selection_v1_device_manager_create(m_display);
        m_xwayland = wlr_xwayland_create(m_display, wlr_comp, true);
        if (m_xwayland) {
            setenv("DISPLAY", m_xwayland->display_name, 1);
            log_info("ready on X11 display: {}", m_xwayland->display_name);
            m_xwayland_listeners.listen(&m_xwayland->events.ready, [this](void*) {
                wlr_xwayland_set_seat(m_xwayland, m_input->seat().raw());
                m_input->cursor().set_xwayland_cursor(m_xwayland);
            });
            m_xwayland_listeners.listen(&m_xwayland->events.new_surface, [this](void* data) {
                auto* xsurface = static_cast<wlr_xwayland_surface*>(data);
                if (xsurface->override_redirect) {
                    create_popup(xsurface);
                } else {
                    create_window(xsurface);
                }
            });
        }
        wlr_ext_data_control_manager_v1_create(m_display, 1);
        wlr_data_control_manager_v1_create(m_display);
        wlr_ext_image_copy_capture_manager_v1_create(m_display, 1);
        wlr_ext_output_image_capture_source_manager_v1_create(m_display, 1);
        wlr_screencopy_manager_v1_create(m_display);
        wlr_export_dmabuf_manager_v1_create(m_display);
        wlr_viewporter_create(m_display);
        wlr_fractional_scale_manager_v1_create(m_display, 1);
        wlr_presentation_create(m_display, m_backend, 2);
        wlr_single_pixel_buffer_manager_v1_create(m_display);
        wlr_alpha_modifier_v1_create(m_display);
        wlr_content_type_manager_v1_create(m_display, 1);
        wlr_color_representation_manager_v1_create_with_renderer(m_display, 1, m_renderer);
        wlr_fixes_create(m_display, 1);
        m_foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(m_display, 1);
        auto* foreign_registry = wlr_xdg_foreign_registry_create(m_display);
        wlr_xdg_foreign_v1_create(m_display, foreign_registry);
        wlr_xdg_foreign_v2_create(m_display, foreign_registry);
        m_tearing_control_mgr = wlr_tearing_control_manager_v1_create(m_display, 1);

        m_workspace_manager = wlr_ext_workspace_manager_v1_create(m_display, 1);
        m_listeners.listen(&m_workspace_manager->events.commit, this, &Compositor::on_workspace_commit);

        m_output_manager = std::make_unique<output::Manager>(*this);

        m_scene = std::make_unique<Scene>(m_display, m_output_manager->layout());

        if (m_renderer->features.input_color_transform) {
            static constexpr auto render_intents = std::to_array({
                WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
            });
            size_t tf_len = 0;
            const std::unique_ptr<wp_color_manager_v1_transfer_function, CFree> transfer_functions(
                wlr_color_manager_v1_transfer_function_list_from_renderer(m_renderer, &tf_len));
            size_t primaries_len = 0;
            const std::unique_ptr<wp_color_manager_v1_primaries, CFree> primaries(
                wlr_color_manager_v1_primaries_list_from_renderer(m_renderer, &primaries_len));

            if (transfer_functions && primaries && tf_len > 0 && primaries_len > 0) {
                wlr_color_manager_v1_options options{};
                options.features.parametric = true;
                options.features.set_mastering_display_primaries = true;
                options.render_intents = render_intents.data();
                options.render_intents_len = render_intents.size();
                options.transfer_functions = transfer_functions.get();
                options.transfer_functions_len = tf_len;
                options.primaries = primaries.get();
                options.primaries_len = primaries_len;
                if (auto* color_mgr = wlr_color_manager_v1_create(m_display, 2, &options)) {
                    m_scene->set_color_manager_v1(color_mgr);
                }
            }
        }

        if (const int drm_fd = wlr_renderer_get_drm_fd(m_renderer); drm_fd >= 0) {
            if (wlr_renderer_get_texture_formats(m_renderer, WLR_BUFFER_CAP_DMABUF) != nullptr) {
                if (auto* linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(m_display, 5, m_renderer)) {
                    m_scene->set_linux_dmabuf_v1(linux_dmabuf);
                }
            }
            if (m_renderer->features.timeline && m_backend->features.timeline) {
                wlr_linux_drm_syncobj_manager_v1_create(m_display, 1, drm_fd);
            }
        }

        auto* xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&xdg_shell->events.new_toplevel, this, &Compositor::on_new_window);
        m_listeners.listen(&xdg_shell->events.new_popup,
                           [this](void* d) { create_popup(static_cast<wlr_xdg_popup*>(d)); });

        auto* layer_shell = wlr_layer_shell_v1_create(m_display, 5);
        m_listeners.listen(&layer_shell->events.new_surface, this, &Compositor::on_new_layer);

        auto* xdg_activation = wlr_xdg_activation_v1_create(m_display);
        m_listeners.listen(&xdg_activation->events.new_token, [this](void* data) {
            auto* token = static_cast<wlr_xdg_activation_token_v1*>(data);
            if (!is_session_locked() && is_valid_activation_token(token)) {
                token->data = token;
            }
        });
        m_listeners.listen(&xdg_activation->events.request_activate, this, &Compositor::on_request_activate);

        m_lock = std::make_unique<Lock>(*this);

        auto* xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(m_display);
        m_listeners.listen(&xdg_decoration_mgr->events.new_toplevel_decoration, [](void* data) {
            auto* decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);
            if (auto* window = Window::try_from(decoration->toplevel)) {
                window->set_decoration(decoration);
            }
        });

        auto* xdg_dialog_mgr = wlr_xdg_wm_dialog_v1_create(m_display, 1);
        m_listeners.listen(&xdg_dialog_mgr->events.new_dialog, [](void* data) {
            auto* dialog = static_cast<wlr_xdg_dialog_v1*>(data);
            if (auto* window = Window::try_from(dialog->xdg_toplevel)) {
                window->set_dialog(dialog);
            }
        });

        auto* xdg_tag_mgr = wlr_xdg_toplevel_tag_manager_v1_create(m_display, 1);
        m_listeners.listen(&xdg_tag_mgr->events.set_tag, [](void* data) {
            const auto* event = static_cast<wlr_xdg_toplevel_tag_manager_v1_set_tag_event*>(data);
            if (auto* window = Window::try_from(event->toplevel)) {
                window->set_tag(event->tag);
            }
        });
        m_listeners.listen(&xdg_tag_mgr->events.set_description, [](void* data) {
            const auto* event = static_cast<wlr_xdg_toplevel_tag_manager_v1_set_description_event*>(data);
            if (auto* window = Window::try_from(event->toplevel)) {
                window->set_description(event->description);
            }
        });

        m_input = std::make_unique<Input>(*this);
        m_bell = std::make_unique<Bell>(*this);
        m_config->set_on_reload([this] {
            if (!m_config->animations().enabled) {
                cancel_animations();
                restack_windows();
            }
            m_input->apply_config();
            m_output_manager->apply_configs();
        });

        const char* socket = wl_display_add_socket_auto(m_display);
        if (!socket) {
            return false;
        }

        if (!wlr_backend_start(m_backend)) {
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("XDG_CURRENT_DESKTOP", "frost", 1);
        setenv("XDG_SESSION_DESKTOP", "frost", 1);
        log_info("ready on Wayland socket: {}", socket);

        m_ipc = std::make_unique<ipc::Server>(*this, socket);
        if (m_ipc->is_running()) {
            setenv("FROST_SOCKET", m_ipc->socket_path().c_str(), 1);
            log_info("ready on IPC socket: {}", m_ipc->socket_path().c_str());
        } else {
            unsetenv("FROST_SOCKET");
        }

        return true;
    }

    void Compositor::run() {
        if (m_session) {
            std::vector<std::string> args = {
                "--systemd", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "XDG_SESSION_DESKTOP",
            };
            if (m_xwayland) {
                args.emplace_back("DISPLAY");
            }
            if (m_ipc && m_ipc->is_running()) {
                args.emplace_back("FROST_SOCKET");
            }
            for (const auto& [name, value] : m_config->environment()) {
                if (value && !std::ranges::contains(args, name)) {
                    args.push_back(name);
                }
            }
            spawn({
                .program = "dbus-update-activation-environment",
                .args = std::move(args),
            });
        }

        for (const auto& action : m_config->autostart()) {
            execute_action(action);
        }

        wl_display_run(m_display);
    }

    void Compositor::quit() noexcept {
        wl_display_terminate(m_display);
    }

    KeybindResult Compositor::handle_keybinding(uint32_t modifiers, xkb_keysym_t sym, bool is_repeat) {
        if (m_session && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            wlr_session_change_vt(m_session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return {.handled = true, .repeat = false};
        }

        const bool locked = is_locked();
        const uint32_t clean_mods = modifiers & key_modifier_mask;
        const xkb_keysym_t lower_sym = xkb_keysym_to_lower(sym);

        for (const auto& bind : m_config->keybinds()) {
            if ((locked && !bind.locked) || (is_repeat && !bind.repeat)) {
                continue;
            }
            if (bind.modifiers == clean_mods && bind.keysym == lower_sym) {
                for (const auto& action : bind.actions) {
                    execute_action(action);
                }
                return {.handled = true, .repeat = bind.repeat};
            }
        }
        return {.handled = false, .repeat = false};
    }

    void Compositor::execute_action(const Action& action) {
        std::visit(
            [this](const auto& act) {
                using T = std::decay_t<decltype(act)>;
                if constexpr (!std::is_same_v<T, action::Quit> && !std::is_same_v<T, action::Spawn>) {
                    if (is_locked()) {
                        return;
                    }
                }

                if constexpr (std::is_same_v<T, action::Quit>) {
                    quit();
                } else if constexpr (std::is_same_v<T, action::CloseWindow>) {
                    if (Window* win = focused_window()) {
                        win->close();
                    }
                } else if constexpr (std::is_same_v<T, action::CycleWindow>) {
                    if (Output* out = focused_output()) {
                        if (Window* next = out->active_workspace().cycle()) {
                            focus_window(next);
                        }
                    }
                } else if constexpr (std::is_same_v<T, action::MaximizeWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_maximize();
                    }
                } else if constexpr (std::is_same_v<T, action::FullscreenWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_fullscreen();
                    }
                } else if constexpr (std::is_same_v<T, action::ToggleFloating>) {
                    if (Window* win = focused_window()) {
                        win->toggle_floating();
                    }
                } else if constexpr (std::is_same_v<T, action::FocusWindow>) {
                    focus_window_in_direction(act.direction);
                } else if constexpr (std::is_same_v<T, action::SwapWindow>) {
                    swap_window_in_direction(act.direction);
                } else if constexpr (std::is_same_v<T, action::FocusWorkspace>) {
                    focus_workspace(act.workspace);
                } else if constexpr (std::is_same_v<T, action::MoveToWorkspace>) {
                    if (Window* win = focused_window()) {
                        move_to_workspace(win, act.workspace);
                    }
                } else if constexpr (std::is_same_v<T, action::Spawn>) {
                    spawn(act);
                } else {
                    static_assert(false, "Unhandled Action");
                }
            },
            action);
    }

    void Compositor::spawn(const action::Spawn& action) {
        std::vector<const char*> argv;
        argv.reserve(action.args.size() + 2);
        argv.push_back(action.program.c_str());
        for (const auto& arg : action.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        const std::string cursor_size = std::to_string(m_config->cursor().size);
        std::unordered_map<std::string_view, std::optional<std::string_view>> overrides;
        overrides.reserve(m_config->environment().size() + 2);
        if (const char* theme = m_input->cursor().effective_theme()) {
            overrides["XCURSOR_THEME"] = theme;
        } else {
            overrides["XCURSOR_THEME"] = std::nullopt;
        }
        overrides["XCURSOR_SIZE"] = cursor_size;
        for (const auto& env_var : m_config->environment()) {
            overrides[env_var.name] = env_var.value ? std::optional<std::string_view>(*env_var.value) : std::nullopt;
        }

        std::vector<std::string> env_storage;
        if (environ) {
            for (char* const* ep = environ; *ep != nullptr; ++ep) {
                std::string_view entry(*ep);
                const size_t eq = entry.find('=');
                const std::string_view key = (eq != std::string_view::npos) ? entry.substr(0, eq) : entry;
                if (!overrides.contains(key)) {
                    env_storage.emplace_back(entry);
                }
            }
        }
        for (const auto& [name, val] : overrides) {
            if (val) {
                std::string entry;
                entry.reserve(name.size() + 1 + val->size());
                entry.append(name);
                entry.push_back('=');
                entry.append(*val);
                env_storage.push_back(std::move(entry));
            }
        }

        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (auto& entry : env_storage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        const pid_t pid = fork();
        if (pid < 0) {
            log_error("failed to fork process '{}'", action.program);
            return;
        }

        if (pid == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);

            sigset_t set;
            sigemptyset(&set);
            sigprocmask(SIG_SETMASK, &set, nullptr);

            const int dev_null = open("/dev/null", O_RDWR);
            if (dev_null >= 0) {
                dup2(dev_null, STDIN_FILENO);
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
            }

            closefrom(3);

            execvpe(argv[0], const_cast<char* const*>(argv.data()), envp.data());
            _exit(1);
        }

        m_spawned_pids.push_back(pid);
    }

    void Compositor::focus_surface(wlr_surface* surface) {
        wlr_surface* prev_surface = m_input->seat().focused_surface();
        if (prev_surface == surface) {
            return;
        }
        if (prev_surface) {
            wlr_surface* prev_root = root_popup_parent(prev_surface);
            wlr_surface* new_root = surface ? root_popup_parent(surface) : nullptr;
            if (prev_root != new_root) {
                if (Window* prev_window = Window::try_from(prev_root)) {
                    prev_window->set_activated(false);
                } else if (auto* prev_xsurface = wlr_xwayland_surface_try_from_wlr_surface(prev_root)) {
                    wlr_xwayland_surface_activate(prev_xsurface, false);
                }
            }
        }
        m_input->seat().notify_keyboard_enter(surface);
    }

    Window* Compositor::focused_window() const noexcept {
        if (is_locked()) {
            return nullptr;
        }
        Output* out = focused_output();
        return out ? out->active_workspace().focused_window() : nullptr;
    }

    void Compositor::focus_workspace(Output* output, uint32_t id) {
        if (is_locked() || id == 0 || !output || output->active_workspace().id() == id) {
            return;
        }

        cancel_workspace_swipe();
        dismiss_popups();

        if (output->is_enabled()) {
            set_focused_output(output);
        }
        log_debug("output '{}': switching from workspace {} to {}", output->raw()->name,
                  output->active_workspace().id(), id);

        Workspace& old_ws = output->active_workspace();
        Workspace& new_ws = output->workspace(id);
        output->set_active_workspace(&new_ws);
        if (output->is_enabled() && m_config->animations().enabled) {
            output->animation().start(old_ws, new_ws, std::max(1, output->layout_box().width));
            wlr_output_schedule_frame(output->raw());
        } else {
            output->animation().cancel();
            old_ws.deactivate();
            new_ws.activate();
        }

        if (output->is_enabled()) {
            focus_window(new_ws.focused_window());
        } else {
            restack_windows();
        }
    }

    bool Compositor::begin_workspace_swipe() {
        if (m_swipe_output || !m_config->gestures().workspace_swipe || is_locked()) {
            return false;
        }

        Output* out = output_at(m_input->cursor().x(), m_input->cursor().y());
        if (!out) {
            out = focused_output();
        }
        if (!out) {
            return false;
        }

        m_swipe_output = out;
        out->animation().begin_swipe(*out);
        dismiss_popups();
        restack_windows();
        return true;
    }

    void Compositor::update_workspace_swipe(double dx) {
        if (m_swipe_output) {
            m_swipe_output->animation().update_swipe(*m_swipe_output, dx, m_config->touchpad().natural_scroll);
        }
    }

    void Compositor::end_workspace_swipe() {
        if (!m_swipe_output) {
            return;
        }

        Output* out = m_swipe_output;
        m_swipe_output = nullptr;

        if (Workspace* target = out->animation().end_swipe(*out)) {
            set_focused_output(out);
            focus_window(target->focused_window());
        } else if (!out->animation().is_active()) {
            refocus();
        }
    }

    void Compositor::cancel_workspace_swipe() {
        if (m_swipe_output) {
            m_swipe_output->animation().cancel();
            m_swipe_output = nullptr;
        }
    }

    bool Compositor::cancel_swipe_for(Output* output) {
        if (m_swipe_output != output) {
            return false;
        }
        cancel_workspace_swipe();
        return true;
    }

    void Compositor::cancel_animations() {
        cancel_workspace_swipe();
        for (const auto& out : outputs()) {
            out->animation().cancel();
            out->active_workspace().snap_animations();
        }
    }

    void Compositor::on_animation_finished() {
        restack_windows();
        m_input->cursor().update_focus();
    }

    void Compositor::move_to_workspace(Window* window, uint32_t id) {
        Workspace* old_ws = window ? window->workspace() : nullptr;
        if (is_locked() || !old_ws || id == 0 || old_ws->id() == id) {
            return;
        }

        cancel_workspace_swipe();
        dismiss_popups(window->scene_tree());

        Output& out = old_ws->output();
        out.animation().cancel();
        log_debug("moving window to workspace {} on output '{}'", id, out.raw()->name);

        Workspace& new_ws = out.workspace(id);
        window->set_workspace(&new_ws);
        window->set_activated(false);

        old_ws->arrange();
        new_ws.arrange();
        new_ws.restack();
        refocus();
    }

    void Compositor::restack_windows() {
        for (const auto& out : outputs()) {
            out->active_workspace().restack();
        }
        update_popup_positions();
        if (m_input) {
            m_input->seat().update_idle_inhibit();
        }
    }

    void Compositor::focus_window(Window* window) {
        if (is_locked()) {
            return;
        }

        if (window) {
            Workspace* ws = window->workspace();
            if (!ws || !ws->output().is_enabled()) {
                return;
            }
            window = ws->resolve_modal(window);
            set_focused_output(&ws->output());
            ws->promote(window);

            if (ws != &ws->output().active_workspace()) {
                focus_workspace(&ws->output(), ws->id());
                return;
            }
        }

        restack_windows();

        if (!window) {
            focus_surface(nullptr);
            m_input->cursor().update_focus();
            return;
        }

        window->set_activated(true);
        focus_surface(window->surface());
        m_input->cursor().update_focus();
    }

    void Compositor::focus_window_in_direction(Direction direction) {
        if (is_locked()) {
            return;
        }

        Output* cur = focused_output();
        if (!cur) {
            return;
        }

        if (Window* w = cur->active_workspace().window_in_direction(direction)) {
            focus_window(w);
        } else if (Output* adj = cur->adjacent_output(direction)) {
            set_focused_output(adj);
            focus_window(adj->active_workspace().focused_window());
        }
    }

    void Compositor::swap_window_in_direction(Direction direction) {
        if (is_locked()) {
            return;
        }

        Window* focused = focused_window();
        if (!focused) {
            return;
        }

        Output* cur = focused_output();
        if (!cur) {
            return;
        }

        if (focused->is_tiled() && cur->active_workspace().swap_window_in_direction(direction)) {
            update_cursor_focus();
            return;
        }

        if (Output* adj = cur->adjacent_output(direction)) {
            m_input->cursor().cancel_grab();
            cancel_workspace_swipe();
            dismiss_popups(focused->scene_tree());

            cur->animation().cancel();
            adj->animation().cancel();

            if (focused->is_floating()) {
                focused->set_placed(false);
            }

            focused->set_output(adj);
            focus_window(focused);
        }
    }

    void Compositor::map_window(Window* window) {
        Output* out = window->output();
        if (!out || !out->is_enabled()) {
            if (Output* enabled = focused_output()) {
                out = enabled;
            }
        }
        if (out) {
            Workspace& ws = out->active_workspace();
            window->set_workspace(&ws);
            ws.arrange();
            if (is_locked() || !out->is_enabled()) {
                restack_windows();
            }
        }
        focus_window(window);
    }

    void Compositor::unmap_window(Window* window) {
        if (m_output_manager) {
            m_output_manager->forget_window(window);
        }
        dismiss_popups(window->scene_tree());
        Workspace* ws = window->workspace();
        if (ws) {
            ws->remove(window);
            ws->arrange();
        }
        m_input->cursor().cancel_grab(window);
        refocus();
    }

    Layer* Compositor::exclusive_layer() const noexcept {
        constexpr std::array layers = {
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        };

        const auto all_outputs = outputs();
        Output* focused_hint = m_output_manager ? m_output_manager->focused_hint() : nullptr;
        const bool has_enabled = std::ranges::any_of(all_outputs, &Output::is_enabled);

        auto find_in_output = [&](const Output* out, zwlr_layer_shell_v1_layer layer) -> Layer* {
            if (!out || (has_enabled && !out->is_enabled())) {
                return nullptr;
            }
            for (const auto& ls : std::views::reverse(out->layers())) {
                if (ls->is_mapped() && ls->layer() == layer && ls->is_exclusive()) {
                    return ls.get();
                }
            }
            return nullptr;
        };

        for (auto layer : layers) {
            if (auto* excl = find_in_output(focused_hint, layer)) {
                return excl;
            }
            for (const auto& out : all_outputs) {
                if (out.get() == focused_hint) {
                    continue;
                }
                if (auto* excl = find_in_output(out.get(), layer)) {
                    return excl;
                }
            }
        }

        return nullptr;
    }

    bool Compositor::allows_tearing(const Output* output) const noexcept {
        if (!output || is_locked()) {
            return false;
        }

        const Window* w = output->active_workspace().focused_window();
        if (!w || !w->is_fullscreen() || w->is_animating() ||
            wlr_tearing_control_manager_v1_surface_hint_from_surface(m_tearing_control_mgr, w->surface()) !=
                WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC) {
            return false;
        }

        const wlr_box output_box = output->layout_box();
        if (output_box.width <= 0 || output_box.height <= 0) {
            return false;
        }

        return std::ranges::none_of(m_popups,
                                    [&output_box](const auto& popup) { return popup->is_visible_on(output_box); });
    }

    void Compositor::refocus() {
        if (is_session_locked()) {
            focus_surface(m_lock->target_surface());
            m_input->cursor().update_focus();
            m_input->seat().update_idle_inhibit();
            return;
        }

        if (auto* excl = exclusive_layer()) {
            restack_windows();
            focus_layer(excl);
            return;
        }

        if (wlr_surface* focused = m_input->seat().focused_surface()) {
            wlr_surface* root = root_popup_parent(focused);
            if (root && root->mapped) {
                const auto* layer = Layer::try_from(root);
                const auto* xsurf = wlr_xwayland_surface_try_from_wlr_surface(root);
                bool keep_focus = layer && layer->is_focusable() && layer->output()->is_enabled();
                if (!keep_focus && xsurf && xsurf->override_redirect &&
                    wlr_xwayland_surface_override_redirect_wants_focus(xsurf)) {
                    update_popup_positions();
                    int lx = 0;
                    int ly = 0;
                    keep_focus = m_scene->surface_coords(focused, lx, ly);
                }
                if (keep_focus) {
                    restack_windows();
                    m_input->cursor().update_focus();
                    return;
                }
            }
        }

        focus_window(focused_window());
    }

    void Compositor::remove_window(Window* window) {
        if (window) {
            if (m_output_manager) {
                m_output_manager->forget_window(window);
            }
            if (window->is_mapped()) {
                unmap_window(window);
            } else {
                dismiss_popups(window->scene_tree());
            }
        }
        erase_unique_ptr(m_windows, window);
    }

    void Compositor::create_window(wlr_xwayland_surface* xsurface) {
        m_windows.push_back(std::make_unique<window::XWayland>(*this, xsurface));
    }

    void Compositor::create_popup(wlr_xwayland_surface* xsurface) {
        m_popups.push_back(std::make_unique<Popup>(*this, xsurface));
    }

    void Compositor::recreate_as_popup(Window* window) {
        if (!window) {
            return;
        }
        auto* xsurface = window->xwayland_surface();
        remove_window(window);
        if (xsurface) {
            create_popup(xsurface);
        }
    }

    void Compositor::recreate_as_window(Popup* popup) {
        if (!popup) {
            return;
        }
        auto* xsurface = popup->xwayland_surface();
        remove_popup(popup);
        if (xsurface) {
            create_window(xsurface);
        }
    }

    void Compositor::remove_popup(Popup* popup) {
        if (erase_unique_ptr(m_popups, popup)) {
            update_cursor_focus();
        }
    }

    void Compositor::dismiss_popups(wlr_scene_tree* root, wlr_scene_tree* except_root) {
        while (true) {
            auto it = std::ranges::find_if(std::views::reverse(m_popups), [root, except_root](const auto& p) {
                if (!p->raw()) {
                    return false;
                }
                if (except_root && p->root_tree() == except_root) {
                    return false;
                }
                return !root || p->root_tree() == root;
            });
            if (it == m_popups.rend()) {
                break;
            }
            wlr_xdg_popup_destroy((*it)->raw());
        }
    }

    void Compositor::update_popup_positions() noexcept {
        for (const auto& popup : m_popups) {
            popup->update_position();
        }
    }

    void Compositor::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<window::Xdg>(*this, toplevel));
    }

    void Compositor::on_request_activate(wlr_xdg_activation_v1_request_activate_event* event) {
        if (!event->surface || !event->surface->mapped) {
            return;
        }

        auto* window = Window::try_from(event->surface);
        if (!window || !window->is_mapped()) {
            return;
        }

        if (event->token && event->token->data) {
            focus_window(window);
        } else {
            window->set_urgent(true);
        }
    }

    void Compositor::on_workspace_commit(wlr_ext_workspace_v1_commit_event* event) {
        Workspace* to_activate = nullptr;
        wlr_ext_workspace_v1_request* req = nullptr;
        wl_list_for_each(req, event->requests, link) {
            if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE && req->activate.workspace) {
                to_activate = static_cast<Workspace*>(req->activate.workspace->data);
            }
        }

        if (to_activate) {
            focus_workspace(&to_activate->output(), to_activate->id());
        }
    }

    void Compositor::create_popup(wlr_xdg_popup* popup) {
        if (!popup || !popup->parent) {
            return;
        }

        wlr_scene_tree* root_tree = nullptr;
        wlr_surface* root_surface = root_popup_parent(popup->parent);
        if (wlr_xdg_surface* toplevel = wlr_xdg_surface_try_from_wlr_surface(root_surface);
            toplevel && toplevel->data) {
            root_tree = static_cast<wlr_scene_tree*>(toplevel->data);
        } else if (auto* ls = Layer::try_from(root_surface)) {
            root_tree = ls->scene_tree();
        }

        if (!root_tree) {
            log_error("popup parent surface has no associated scene tree");
            return;
        }

        m_popups.push_back(std::make_unique<Popup>(*this, popup, root_tree));
    }

    void Compositor::on_new_layer(wlr_layer_surface_v1* layer_surface) {
        Output* output = layer_surface->output ? output_from_wlr(layer_surface->output) : focused_output();
        if (!output || output->is_config_disabled()) {
            log_error("no output available for layer surface");
            wlr_layer_surface_v1_destroy(layer_surface);
            return;
        }

        layer_surface->output = output->raw();
        output->add_layer(layer_surface);
    }

    void Compositor::focus_layer(Layer* layer) {
        if (!layer || !layer->is_mapped() || is_session_locked()) {
            return;
        }
        if (layer->output()) {
            set_focused_output(layer->output());
        }
        if (layer->is_exclusive()) {
            cancel_animations();
            m_input->seat().cancel_drag();
            dismiss_popups(nullptr, layer->scene_tree());
        }
        if (layer->scene_tree()) {
            wlr_scene_node_raise_to_top(&layer->scene_tree()->node);
        }
        focus_surface(layer->surface());
        m_input->cursor().update_focus();
    }

    void Compositor::update_cursor_focus() noexcept {
        m_input->cursor().update_focus();
    }

}
