#include "flux/input/cursor.hpp"
#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace flux::input {

    Cursor::Cursor(Input& input) : m_input(input), m_wlr_cursor(wlr_cursor_create()) {
        if (const char* env_theme = std::getenv("XCURSOR_THEME"); env_theme && *env_theme != '\0') {
            m_env_xcursor_theme = env_theme;
        }
        wlr_cursor_attach_output_layout(m_wlr_cursor, m_input.compositor().output_layout());
        apply_config(m_input.compositor().config().cursor());

        m_listeners.listen(&m_wlr_cursor->events.motion, this, &Cursor::on_motion);
        m_listeners.listen(&m_wlr_cursor->events.motion_absolute, this, &Cursor::on_motion_absolute);
        m_listeners.listen(&m_wlr_cursor->events.button, this, &Cursor::on_button);
        m_listeners.listen(&m_wlr_cursor->events.axis, this, &Cursor::on_axis);
        m_listeners.listen(&m_wlr_cursor->events.frame, this, &Cursor::on_frame);
        m_listeners.listen(&m_wlr_cursor->events.swipe_begin, this, &Cursor::on_swipe_begin);
        m_listeners.listen(&m_wlr_cursor->events.swipe_update, this, &Cursor::on_swipe_update);
        m_listeners.listen(&m_wlr_cursor->events.swipe_end, this, &Cursor::on_swipe_end);
        m_listeners.listen(&m_wlr_cursor->events.pinch_begin, this, &Cursor::on_pinch_begin);
        m_listeners.listen(&m_wlr_cursor->events.pinch_update, this, &Cursor::on_pinch_update);
        m_listeners.listen(&m_wlr_cursor->events.pinch_end, this, &Cursor::on_pinch_end);
        m_listeners.listen(&m_wlr_cursor->events.hold_begin, this, &Cursor::on_hold_begin);
        m_listeners.listen(&m_wlr_cursor->events.hold_end, this, &Cursor::on_hold_end);
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

    void Cursor::apply_config(const CursorConfig& config) {
        if (m_cursor_mgr && m_config == config) {
            return;
        }
        m_config = config;

        const char* theme = !config.theme.empty() ? config.theme.c_str()
                                                  : (m_env_xcursor_theme ? m_env_xcursor_theme->c_str() : nullptr);
        if (theme) {
            setenv("XCURSOR_THEME", theme, 1);
        } else {
            unsetenv("XCURSOR_THEME");
        }
        setenv("XCURSOR_SIZE", std::to_string(config.size).c_str(), 1);
        if (m_cursor_mgr) {
            wlr_xcursor_manager_destroy(m_cursor_mgr);
        }
        m_cursor_mgr = wlr_xcursor_manager_create(theme, config.size);
        wlr_xcursor_manager_load(m_cursor_mgr, 1);

        if (m_current_shape) {
            wlr_cursor_set_xcursor(m_wlr_cursor, m_cursor_mgr, m_current_shape);
        } else if (!m_input.seat().focused_pointer_surface()) {
            set_xcursor("default");
        }
    }

    void Cursor::start_grab(Window* window, uint32_t button) {
        uint32_t edges = WLR_EDGE_NONE;
        if (button == btn_right) {
            const wlr_box box = window->geometry_box();
            const uint32_t horiz = (m_wlr_cursor->x < box.x + (box.width / 2.0)) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
            const uint32_t vert = (m_wlr_cursor->y < box.y + (box.height / 2.0)) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
            edges = horiz | vert;
        }

        m_grab_window = window;
        m_grab_button = button;
        m_grab_x = m_wlr_cursor->x;
        m_grab_y = m_wlr_cursor->y;
        m_input.seat().clear_pointer_focus();
        set_xcursor(edges != WLR_EDGE_NONE ? wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges)) : "grabbing");
        window->start_grab(edges);
    }

    void Cursor::end_grab() {
        if (!m_grab_window) {
            return;
        }
        Window* window = m_grab_window;
        m_grab_window = nullptr;
        m_grab_button = 0;
        window->end_grab();
        set_xcursor("default");
        update_focus();
    }

    void Cursor::cancel_grab(const Window* window) {
        if (m_grab_window && (!window || m_grab_window == window)) {
            end_grab();
        }
    }

    bool Cursor::apply_constraint(double& dx, double& dy) const {
        auto* constraint = m_input.seat().active_constraint();
        if (!constraint) {
            return true;
        }
        if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return false;
        }
        double sx = 0.0;
        double sy = 0.0;
        if (m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy) !=
            constraint->surface) {
            return true;
        }
        double confined_x = 0.0;
        double confined_y = 0.0;
        if (!wlr_region_confine(&constraint->region, sx, sy, sx + dx, sy + dy, &confined_x, &confined_y)) {
            return false;
        }
        dx = confined_x - sx;
        dy = confined_y - sy;
        return true;
    }

    void Cursor::on_motion(wlr_pointer_motion_event* event) {
        m_input.seat().notify_activity();
        double dx = event->delta_x;
        double dy = event->delta_y;
        if (apply_constraint(dx, dy)) {
            wlr_cursor_move(m_wlr_cursor, &event->pointer->base, dx, dy);
            if (!m_grab_window && !m_input.compositor().focused_window()) {
                m_input.compositor().set_focused_output(nullptr);
            }
            process_motion(event->time_msec);
        }
        m_input.seat().notify_relative_motion(event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx,
                                              event->unaccel_dy);
    }

    void Cursor::on_motion_absolute(wlr_pointer_motion_absolute_event* event) {
        m_input.seat().notify_activity();
        double lx = 0.0;
        double ly = 0.0;
        wlr_cursor_absolute_to_layout_coords(m_wlr_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
        double dx = lx - m_wlr_cursor->x;
        double dy = ly - m_wlr_cursor->y;
        if (apply_constraint(dx, dy)) {
            wlr_cursor_move(m_wlr_cursor, &event->pointer->base, dx, dy);
            if (!m_grab_window && !m_input.compositor().focused_window()) {
                m_input.compositor().set_focused_output(nullptr);
            }
            process_motion(event->time_msec);
        }
    }

    void Cursor::on_button(wlr_pointer_button_event* event) {
        if (m_grab_window) {
            m_input.seat().notify_activity();
            if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && event->button == m_grab_button) {
                m_input.compositor().set_focused_output(nullptr);
                end_grab();
            }
            return;
        }

        if (m_input.compositor().is_workspace_swiping()) {
            m_input.seat().notify_activity();
            return;
        }

        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            m_input.compositor().set_focused_output(nullptr);
            if (Output* out = m_input.compositor().focused_output(); out && out->animation().is_active()) {
                out->animation().cancel();
                m_input.compositor().on_animation_finished();
            }

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

                wlr_keyboard* keyboard = m_input.seat().keyboard();
                constexpr uint32_t mask = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;
                const uint32_t mods = keyboard ? (wlr_keyboard_get_modifiers(keyboard) & mask) : 0;
                if (mods == WLR_MODIFIER_LOGO && (event->button == btn_left || event->button == btn_right) &&
                    window->is_floating()) {
                    m_input.seat().notify_activity();
                    start_grab(window, event->button);
                    return;
                }
            } else if (surface != nullptr) {
                if (auto* layer = Layer::try_from(root_popup_parent(surface)); layer && layer->is_focusable()) {
                    m_input.compositor().focus_surface(surface);
                } else {
                    m_input.compositor().focus_window(m_input.compositor().focused_window());
                }
            } else {
                m_input.compositor().focus_window(nullptr);
            }
        }
        m_input.seat().notify_button(event->time_msec, event->button, event->state);
    }

    void Cursor::on_axis(wlr_pointer_axis_event* event) {
        m_input.seat().notify_axis(event->time_msec, event->orientation, event->delta, event->delta_discrete,
                                   event->source, event->relative_direction);
    }

    void Cursor::on_frame() {
        m_input.seat().notify_frame();
    }

    void Cursor::on_swipe_begin(wlr_pointer_swipe_begin_event* event) {
        if (event->fingers == 3 && !m_grab_window && m_input.compositor().begin_workspace_swipe()) {
            m_input.seat().notify_activity();
            m_input.seat().clear_pointer_focus();
            return;
        }
        m_input.seat().notify_swipe_begin(event->time_msec, event->fingers);
    }

    void Cursor::on_swipe_update(wlr_pointer_swipe_update_event* event) {
        if (m_input.compositor().is_workspace_swiping()) {
            m_input.seat().notify_activity();
            m_input.compositor().update_workspace_swipe(event->dx);
            return;
        }
        m_input.seat().notify_swipe_update(event->time_msec, event->dx, event->dy);
    }

    void Cursor::on_swipe_end(wlr_pointer_swipe_end_event* event) {
        if (m_input.compositor().is_workspace_swiping()) {
            m_input.seat().notify_activity();
            m_input.compositor().end_workspace_swipe();
            return;
        }
        m_input.seat().notify_swipe_end(event->time_msec, event->cancelled);
    }

    void Cursor::on_pinch_begin(wlr_pointer_pinch_begin_event* event) {
        m_input.seat().notify_pinch_begin(event->time_msec, event->fingers);
    }

    void Cursor::on_pinch_update(wlr_pointer_pinch_update_event* event) {
        m_input.seat().notify_pinch_update(event->time_msec, event->dx, event->dy, event->scale, event->rotation);
    }

    void Cursor::on_pinch_end(wlr_pointer_pinch_end_event* event) {
        m_input.seat().notify_pinch_end(event->time_msec, event->cancelled);
    }

    void Cursor::on_hold_begin(wlr_pointer_hold_begin_event* event) {
        m_input.seat().notify_hold_begin(event->time_msec, event->fingers);
    }

    void Cursor::on_hold_end(wlr_pointer_hold_end_event* event) {
        m_input.seat().notify_hold_end(event->time_msec, event->cancelled);
    }

    void Cursor::process_motion(uint32_t time_msec) {
        if (m_grab_window) {
            const int dx = static_cast<int>(std::round(m_wlr_cursor->x - m_grab_x));
            const int dy = static_cast<int>(std::round(m_wlr_cursor->y - m_grab_y));
            m_grab_window->update_grab(dx, dy);
            return;
        }

        if (m_input.compositor().is_workspace_swiping()) {
            return;
        }

        if (!m_input.compositor().exclusive_layer()) {
            if (auto* constraint = m_input.seat().active_constraint();
                constraint && constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
                return;
            }
        }

        double sx = 0.0;
        double sy = 0.0;
        wlr_surface* surface = m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy);
        if (surface) {
            if (auto* excl = m_input.compositor().exclusive_layer()) {
                if (root_popup_parent(surface) != excl->surface()) {
                    m_input.seat().clear_pointer_focus();
                    return;
                }
            }
            m_input.seat().notify_enter(surface, sx, sy);
            m_input.seat().notify_motion(time_msec, sx, sy);
        } else {
            m_input.seat().clear_pointer_focus();
        }
    }

    void Cursor::update_focus() {
        if (m_input.compositor().is_workspace_swiping()) {
            return;
        }

        if (m_grab_window) {
            if (m_input.compositor().exclusive_layer() || !m_grab_window->is_mapped() ||
                !m_grab_window->is_floating() || m_grab_window != m_input.compositor().focused_window()) {
                end_grab();
            }
            return;
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto time_msec =
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        process_motion(time_msec);
    }

}
