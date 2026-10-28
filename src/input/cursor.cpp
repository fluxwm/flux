#include "flux/input/cursor.hpp"
#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/layer.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <limits>

namespace flux::input {

    Cursor::Cursor(Input& input)
        : m_input(input), m_wlr_cursor(wlr_cursor_create()), m_cursor_mgr(wlr_xcursor_manager_create(nullptr, 24)) {
        wlr_cursor_attach_output_layout(m_wlr_cursor, m_input.compositor().output_layout());
        wlr_xcursor_manager_load(m_cursor_mgr, 1);

        m_listeners.listen(&m_wlr_cursor->events.motion, this, &Cursor::on_motion);
        m_listeners.listen(&m_wlr_cursor->events.motion_absolute, this, &Cursor::on_motion_absolute);
        m_listeners.listen(&m_wlr_cursor->events.button, this, &Cursor::on_button);
        m_listeners.listen(&m_wlr_cursor->events.axis, this, &Cursor::on_axis);
        m_listeners.listen(&m_wlr_cursor->events.frame, this, &Cursor::on_frame);
    }

    Cursor::~Cursor() {
        m_listeners.clear();
        if (m_cursor_mgr) {
            wlr_xcursor_manager_destroy(m_cursor_mgr);
        }
        if (m_wlr_cursor) {
            wlr_cursor_destroy(m_wlr_cursor);
        }
    }

    bool Cursor::is_passthrough() const noexcept {
        return std::holds_alternative<Passthrough>(m_state);
    }

    Window* Cursor::grabbed_window() const noexcept {
        return std::visit(
            [](const auto& state) -> Window* {
                using T = std::decay_t<decltype(state)>;
                if constexpr (!std::is_same_v<T, Passthrough>) {
                    return state.window;
                }
                return nullptr;
            },
            m_state);
    }

    void Cursor::begin_move(Window* window) {
        if (!window || m_input.seat().button_count() == 0 || window->is_maximized() || window->is_fullscreen()) {
            return;
        }
        m_state = Move{
            .window = window,
            .grab_x = m_wlr_cursor->x - window->x(),
            .grab_y = m_wlr_cursor->y - window->y(),
        };
        set_xcursor("grabbing");
    }

    void Cursor::begin_resize(Window* window, uint32_t edges) {
        if (!window || m_input.seat().button_count() == 0 || window->is_maximized() || window->is_fullscreen()) {
            return;
        }
        const int win_w = window->width();
        const int win_h = window->height();

        const double border_x = window->x() + ((edges & WLR_EDGE_RIGHT) ? win_w : 0);
        const double border_y = window->y() + ((edges & WLR_EDGE_BOTTOM) ? win_h : 0);

        m_state = Resize{
            .window = window,
            .grab_x = m_wlr_cursor->x - border_x,
            .grab_y = m_wlr_cursor->y - border_y,
            .initial_geometry =
                wlr_box{
                    .x = window->x(),
                    .y = window->y(),
                    .width = win_w,
                    .height = win_h,
                },
            .resize_edges = edges,
        };

        const char* resize_name = wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges));
        set_xcursor(resize_name);
    }

    void Cursor::reset_mode() {
        if (Window* window = grabbed_window()) {
            window->apply_pending_position();
        }
        m_state = Passthrough{};
        set_xcursor("default");
        update_focus();
    }

    void Cursor::on_motion(wlr_pointer_motion_event* event) {
        wlr_cursor_move(m_wlr_cursor, &event->pointer->base, event->delta_x, event->delta_y);
        process_motion(event->time_msec);
    }

    void Cursor::on_motion_absolute(wlr_pointer_motion_absolute_event* event) {
        wlr_cursor_warp_absolute(m_wlr_cursor, &event->pointer->base, event->x, event->y);
        process_motion(event->time_msec);
    }

    void Cursor::on_button(wlr_pointer_button_event* event) {
        constexpr uint32_t btn_left = BTN_LEFT;
        constexpr uint32_t btn_right = BTN_RIGHT;

        if (is_passthrough()) {
            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                double sx = 0.0;
                double sy = 0.0;
                wlr_surface* surface = nullptr;
                Window* window =
                    m_input.compositor().scene().window_at(m_wlr_cursor->x, m_wlr_cursor->y, &surface, &sx, &sy);

                if (auto* excl = m_input.compositor().exclusive_layer()) {
                    if (surface && root_popup_parent(surface) == excl->surface()) {
                        m_input.compositor().focus_surface(surface);
                    }
                } else if (window != nullptr) {
                    m_input.compositor().focus_window(window);
                } else if (surface != nullptr) {
                    wlr_surface* root = root_popup_parent(surface);
                    if (auto* ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
                        ls && ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
                        m_input.compositor().focus_surface(surface);
                    }
                } else {
                    m_input.compositor().focus_window(nullptr);
                }

                wlr_keyboard* keyboard = m_input.seat().keyboard();
                const uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
                const bool mod_down = (modifiers & WLR_MODIFIER_LOGO) != 0;

                if (window != nullptr && mod_down && !m_input.compositor().exclusive_layer() &&
                    !window->is_maximized() && !window->is_fullscreen()) {
                    if (event->button == btn_left || event->button == btn_right) {
                        m_input.seat().clear_pointer_focus();
                        m_input.seat().notify_button(event->time_msec, event->button, event->state);

                        if (event->button == btn_left) {
                            begin_move(window);
                        } else {
                            const int width = window->width();
                            const int height = window->height();
                            const double rx = m_wlr_cursor->x - window->x();
                            const double ry = m_wlr_cursor->y - window->y();

                            uint32_t edges = 0;
                            if (rx < width / 3.0) {
                                edges |= WLR_EDGE_LEFT;
                            } else if (rx > 2.0 * width / 3.0) {
                                edges |= WLR_EDGE_RIGHT;
                            }
                            if (ry < height / 3.0) {
                                edges |= WLR_EDGE_TOP;
                            } else if (ry > 2.0 * height / 3.0) {
                                edges |= WLR_EDGE_BOTTOM;
                            }
                            if (edges == 0) {
                                edges = (rx < width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                                        (ry < height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
                            }

                            begin_resize(window, edges);
                        }
                        return;
                    }
                }
            }
            m_input.seat().notify_button(event->time_msec, event->button, event->state);
        } else {
            m_input.seat().notify_button(event->time_msec, event->button, event->state);
            if (m_input.seat().button_count() == 0) {
                reset_mode();
            }
        }
    }

    void Cursor::on_axis(wlr_pointer_axis_event* event) {
        m_input.seat().notify_axis(event->time_msec, event->orientation, event->delta, event->delta_discrete,
                                   event->source, event->relative_direction);
    }

    void Cursor::on_frame() {
        m_input.seat().notify_frame();
    }

    void Cursor::process_move(const Move& state) {
        state.window->set_position(m_wlr_cursor->x - state.grab_x, m_wlr_cursor->y - state.grab_y);
    }

    void Cursor::process_resize(const Resize& state) {
        const double border_x = m_wlr_cursor->x - state.grab_x;
        const double border_y = m_wlr_cursor->y - state.grab_y;
        int new_left = state.initial_geometry.x;
        int new_right = state.initial_geometry.x + state.initial_geometry.width;
        int new_top = state.initial_geometry.y;
        int new_bottom = state.initial_geometry.y + state.initial_geometry.height;

        if (state.resize_edges & WLR_EDGE_TOP) {
            new_top = std::min(static_cast<int>(border_y), new_bottom - 1);
        } else if (state.resize_edges & WLR_EDGE_BOTTOM) {
            new_bottom = std::max(static_cast<int>(border_y), new_top + 1);
        }
        if (state.resize_edges & WLR_EDGE_LEFT) {
            new_left = std::min(static_cast<int>(border_x), new_right - 1);
        } else if (state.resize_edges & WLR_EDGE_RIGHT) {
            new_right = std::max(static_cast<int>(border_x), new_left + 1);
        }

        auto clamp_dim = [](int& start, int& end, int min_val, int max_val, bool from_start) {
            const int upper = max_val > 0 ? max_val : std::numeric_limits<int>::max();
            const int size = std::clamp(end - start, min_val, upper);
            if (from_start) {
                start = end - size;
            } else {
                end = start + size;
            }
            return size;
        };

        int new_w = clamp_dim(new_left, new_right, std::max(1, state.window->min_width()), state.window->max_width(),
                              state.resize_edges & WLR_EDGE_LEFT);
        int new_h = clamp_dim(new_top, new_bottom, std::max(1, state.window->min_height()), state.window->max_height(),
                              state.resize_edges & WLR_EDGE_TOP);

        if (state.resize_edges & (WLR_EDGE_TOP | WLR_EDGE_LEFT)) {
            state.window->set_pending_position(new_left, new_top);
        }
        state.window->set_size(new_w, new_h);
    }

    void Cursor::process_motion(uint32_t time_msec) {
        bool handled = std::visit(
            [this](const auto& state) -> bool {
                using T = std::decay_t<decltype(state)>;
                if constexpr (std::is_same_v<T, Move>) {
                    process_move(state);
                    return true;
                } else if constexpr (std::is_same_v<T, Resize>) {
                    process_resize(state);
                    return true;
                }
                return false;
            },
            m_state);

        if (handled) {
            return;
        }

        double sx = 0.0;
        double sy = 0.0;
        wlr_surface* surface = m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy);
        if (surface) {
            m_input.seat().notify_enter(surface, sx, sy);
            m_input.seat().notify_motion(time_msec, sx, sy);
        } else {
            set_xcursor("default");
            m_input.seat().clear_pointer_focus();
        }
    }

    void Cursor::update_focus() {
        if (!is_passthrough()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto time_msec =
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        process_motion(time_msec);
    }

}
