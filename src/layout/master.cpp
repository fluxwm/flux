#include "frost/layout.hpp"
#include "frost/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <unordered_map>

namespace frost::layout {

    namespace {

        struct Metrics {
            Window* master = nullptr;
            size_t tiled_count = 0;
            int area_x = 0;
            int area_y = 0;
            int area_w = 1;
            int area_h = 1;
            int avail_w = 1;
            int avail_h = 1;
            int master_w = 1;
            int stack_x = 0;
            int stack_w = 1;
            int inner_gap = 0;
            int border_width = 0;
            float total_stack_weight = 0.0f;

            [[nodiscard]] constexpr float effective_stack_weight() const noexcept {
                return (total_stack_weight > 0.0f) ? total_stack_weight
                                                   : static_cast<float>(tiled_count > 1 ? tiled_count - 1 : 0);
            }
        };

        struct Neighbors {
            Window* master = nullptr;
            Window* first_stack = nullptr;
            Window* last = nullptr;
            Window* prev = nullptr;
            Window* next = nullptr;
            size_t count = 0;
            size_t index = 0;
            bool found = false;
        };

        [[nodiscard]] int stack_slot_height(int avail_h, float weight, float total_weight) noexcept {
            return std::max(1, static_cast<int>(std::round(avail_h * (weight / total_weight))));
        }

        [[nodiscard]] Metrics compute_metrics(const wlr_box& usable, size_t total, const LayoutConfig& layout,
                                              float master_factor, float raw_stack_weight) noexcept {
            const bool solitary = (total <= 1);
            const auto& gaps = layout.gaps;
            const WorkArea area = work_area(usable, layout, solitary);

            Metrics m{
                .tiled_count = total,
                .area_x = area.box.x,
                .area_y = area.box.y,
                .area_w = area.box.width,
                .area_h = area.box.height,
                .inner_gap = gaps.inner,
                .border_width = area.border_width,
                .total_stack_weight = raw_stack_weight,
            };

            if (!solitary) {
                m.avail_w = std::max(1, m.area_w - gaps.inner);
                const int min_w = std::min(50, m.avail_w / 2);
                m.master_w = std::max(
                    1, std::clamp(static_cast<int>(std::round(m.avail_w * master_factor)), min_w, m.avail_w - min_w));
                m.stack_x = m.area_x + m.master_w + gaps.inner;
                m.stack_w = std::max(1, m.avail_w - m.master_w);
                m.avail_h = std::max(1, m.area_h - (static_cast<int>(total - 2) * gaps.inner));
            }
            return m;
        }

        [[nodiscard]] Neighbors tiled_neighbors(std::span<Window* const> windows, const Window* target) noexcept {
            Neighbors n;
            for (Window* w : windows) {
                if (!w->is_tiled()) {
                    continue;
                }
                if (n.count == 0) {
                    n.master = w;
                } else if (n.count == 1) {
                    n.first_stack = w;
                }
                if (w == target) {
                    n.found = true;
                    n.index = n.count;
                    n.prev = n.last;
                } else if (n.found && !n.next) {
                    n.next = w;
                }
                n.last = w;
                ++n.count;
            }
            return n;
        }

        [[nodiscard]] Window* top_stack_window(std::span<Window* const> focus_stack, const Window* master,
                                               Window* fallback) noexcept {
            auto it = std::ranges::find_if(focus_stack, [&](const Window* w) { return w != master && w->is_tiled(); });
            return (it != focus_stack.end()) ? *it : fallback;
        }

        class Master final : public Base {
        public:
            [[nodiscard]] Layout mode() const noexcept override { return Layout::Master; }

            [[nodiscard]] float master_factor() const noexcept { return m_master_factor; }
            void set_master_factor(float factor) noexcept { m_master_factor = factor; }

            [[nodiscard]] float weight_of(const Window* w) const noexcept {
                const auto it = m_weights.find(w);
                return (it != m_weights.end()) ? it->second : 1.0f;
            }

            void set_weight(const Window* w, float weight) {
                if (weight == 1.0f) {
                    m_weights.erase(w);
                } else {
                    m_weights[w] = weight;
                }
            }

            [[nodiscard]] Metrics metrics(const Context& ctx) const noexcept {
                size_t count = 0;
                Window* master = nullptr;
                float raw_stack_weight = 0.0f;
                for (Window* w : ctx.windows) {
                    if (!w->is_tiled()) {
                        continue;
                    }
                    if (count == 0) {
                        master = w;
                    } else {
                        raw_stack_weight += weight_of(w);
                    }
                    ++count;
                }
                Metrics m = compute_metrics(ctx.usable, count, ctx.config, m_master_factor, raw_stack_weight);
                m.master = master;
                return m;
            }

            void arrange(const Context& ctx) override {
                const Metrics m = metrics(ctx);
                if (m.tiled_count == 0) {
                    return;
                }
                for_each_tiled_slot(ctx.windows, m, [](Window* w, const wlr_box& box) {
                    w->set_geometry(box);
                    return true;
                });
            }

            void on_window_removed(const Window* window, size_t remaining_non_floating) noexcept override {
                m_weights.erase(window);
                if (remaining_non_floating <= 2) {
                    m_weights.clear();
                }
                if (remaining_non_floating <= 1) {
                    m_master_factor = 0.5f;
                }
            }

            void on_windows_swapped(Window* a, Window* b) override {
                const float wa = weight_of(a);
                const float wb = weight_of(b);
                set_weight(a, wb);
                set_weight(b, wa);
            }

            [[nodiscard]] wlr_box next_tile_box(const Context& ctx) const noexcept override {
                const Metrics cur = metrics(ctx);
                if (cur.tiled_count == 0) {
                    const WorkArea area = work_area(ctx.usable, ctx.config, true);
                    return inset_border(area.box, area.border_width);
                }

                const Metrics m = compute_metrics(ctx.usable, cur.tiled_count + 1, ctx.config, m_master_factor,
                                                  cur.total_stack_weight + 1.0f);
                const float effective_weight = m.effective_stack_weight();

                int cur_y = m.area_y;
                if (cur.tiled_count > 1) {
                    size_t idx = 0;
                    for (Window* w : ctx.windows) {
                        if (!w->is_tiled()) {
                            continue;
                        }
                        if (idx > 0) {
                            cur_y += stack_slot_height(m.avail_h, weight_of(w), effective_weight) + m.inner_gap;
                        }
                        ++idx;
                    }
                }

                const int height = std::max(1, (m.area_y + m.area_h) - cur_y);
                return inset_border({.x = m.stack_x, .y = cur_y, .width = m.stack_w, .height = height}, m.border_width);
            }

            [[nodiscard]] Window* floating_fallback(const Context& ctx, Direction dir,
                                                    Window* top_tiled) const noexcept override {
                if (dir != Direction::Left && dir != Direction::Right) {
                    return top_tiled;
                }
                const Neighbors n = tiled_neighbors(ctx.windows, nullptr);
                if (n.count <= 1) {
                    return top_tiled;
                }
                if (dir == Direction::Left) {
                    return n.master;
                }
                return top_stack_window(ctx.focus_stack, n.master, n.first_stack);
            }

            [[nodiscard]] Window* window_in_direction(const Context& ctx, const Window* focused, Direction dir,
                                                      bool has_adj_output) const noexcept override {
                const Neighbors n = tiled_neighbors(ctx.windows, focused);
                if (n.count <= 1 || !n.found) {
                    return nullptr;
                }

                auto wrap = [&](Window* target) -> Window* { return has_adj_output ? nullptr : target; };

                if (n.index == 0) {
                    switch (dir) {
                        case Direction::Right:
                            return top_stack_window(ctx.focus_stack, n.master, n.first_stack);
                        case Direction::Left:
                            return nullptr;
                        case Direction::Down:
                            return wrap(n.first_stack);
                        case Direction::Up:
                            return wrap(n.last);
                    }
                    return nullptr;
                }

                switch (dir) {
                    case Direction::Left:
                        return n.master;
                    case Direction::Right:
                        return nullptr;
                    case Direction::Up:
                        return (n.index == 1) ? wrap(n.last) : n.prev;
                    case Direction::Down:
                        return (n.index == n.count - 1) ? wrap(n.first_stack) : n.next;
                }
                return nullptr;
            }

            [[nodiscard]] Window* swap_target_in_direction(const Context& ctx, const Window* focused,
                                                           Direction dir) const noexcept override {
                const Neighbors n = tiled_neighbors(ctx.windows, focused);
                if (n.count <= 1 || !n.found) {
                    return nullptr;
                }

                if (n.index == 0) {
                    if (dir == Direction::Right) {
                        return top_stack_window(ctx.focus_stack, n.master, n.first_stack);
                    }
                    return nullptr;
                }

                switch (dir) {
                    case Direction::Left:
                        return n.master;
                    case Direction::Right:
                        return nullptr;
                    case Direction::Up:
                        return (n.index > 1) ? n.prev : nullptr;
                    case Direction::Down:
                        return n.next;
                }
                return nullptr;
            }

            [[nodiscard]] Window* window_at(const Context& ctx, double x, double y) const noexcept override {
                const Metrics m = metrics(ctx);
                if (m.tiled_count <= 1) {
                    return nullptr;
                }
                Window* found = nullptr;
                for_each_tiled_slot(ctx.windows, m, [&](Window* target, const wlr_box& slot) {
                    if (wlr_box_contains_point(&slot, x, y)) {
                        found = target;
                        return false;
                    }
                    return true;
                });
                return found;
            }

            [[nodiscard]] std::unique_ptr<ResizeSession> begin_resize(const Context& ctx, Window* window,
                                                                      double cursor_x, double cursor_y) override;

        private:
            template <typename Fn>
            void for_each_tiled_slot(std::span<Window* const> windows, const Metrics& m, const Fn& fn) const {
                const bool single_cell = (m.tiled_count <= 1);
                const float effective_weight = m.effective_stack_weight();

                size_t idx = 0;
                int cur_y = m.area_y;
                for (Window* w : windows) {
                    if (!w->is_tiled()) {
                        continue;
                    }
                    wlr_box cell{};
                    if (single_cell || idx == 0) {
                        cell = {
                            .x = m.area_x,
                            .y = m.area_y,
                            .width = single_cell ? m.area_w : m.master_w,
                            .height = m.area_h,
                        };
                    } else {
                        const int stack_h = stack_slot_height(m.avail_h, weight_of(w), effective_weight);
                        const int height =
                            (idx == m.tiled_count - 1) ? std::max(1, (m.area_y + m.area_h) - cur_y) : stack_h;
                        cell = {.x = m.stack_x, .y = cur_y, .width = m.stack_w, .height = height};
                        cur_y += stack_h + m.inner_gap;
                    }
                    if (!fn(w, inset_border(cell, m.border_width))) {
                        return;
                    }
                    ++idx;
                }
            }

            float m_master_factor = 0.5f;
            std::unordered_map<const Window*, float> m_weights;
        };

        [[nodiscard]] uint32_t compute_resize_edges(const Neighbors& n, const Window* window, double cursor_x,
                                                    double cursor_y) noexcept {
            if (n.index == 0) {
                return WLR_EDGE_RIGHT;
            }

            const size_t stack_idx = n.index - 1;
            const size_t stack_count = n.count - 1;
            if (stack_count <= 1) {
                return WLR_EDGE_LEFT;
            }

            const wlr_box box = window->geometry_box();
            uint32_t vert_edge = WLR_EDGE_NONE;
            if (stack_idx == 0) {
                vert_edge = WLR_EDGE_BOTTOM;
            } else if (stack_idx == stack_count - 1) {
                vert_edge = WLR_EDGE_TOP;
            } else {
                vert_edge = (cursor_y < box.y + (box.height / 2.0)) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
            }

            const double rel_x = cursor_x - box.x;
            const bool near_master_split = rel_x < std::max(40.0, box.width * 0.25);
            if (!near_master_split) {
                return vert_edge;
            }

            double split_y = box.y;
            if (vert_edge == WLR_EDGE_BOTTOM) {
                split_y = box.y + box.height;
            }
            const bool near_stack_split = std::abs(cursor_y - split_y) < std::max(40.0, box.height * 0.35);
            if (near_stack_split) {
                return WLR_EDGE_LEFT | vert_edge;
            }
            return WLR_EDGE_LEFT;
        }

        class MasterResizeSession final : public ResizeSession {
        public:
            MasterResizeSession(Master& layout, Window* window, uint32_t edges, float initial_master_factor,
                                Window* top_win, Window* bot_win, float top_weight, float bot_weight) noexcept
                : m_layout(layout), m_window(window), m_edges(edges), m_initial_master_factor(initial_master_factor),
                  m_top_win(top_win), m_bot_win(bot_win), m_top_weight(top_weight), m_bot_weight(bot_weight) {}

            [[nodiscard]] uint32_t edges() const noexcept override { return m_edges; }

            [[nodiscard]] bool update(const Context& ctx, int dx, int dy) override {
                const Metrics m = m_layout.metrics(ctx);
                if (m.tiled_count < 2) {
                    return false;
                }

                bool changed = false;
                if (m_edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
                    const float delta_factor = static_cast<float>(dx) / static_cast<float>(m.avail_w);
                    const float next_factor = std::clamp(m_initial_master_factor + delta_factor, 0.10f, 0.90f);
                    if (next_factor != m_layout.master_factor()) {
                        m_layout.set_master_factor(next_factor);
                        changed = true;
                    }
                }

                if ((m_edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && m.tiled_count > 2 && m_top_win && m_bot_win &&
                    m_top_win != m.master && m_bot_win != m.master && m_top_win->is_tiled() && m_bot_win->is_tiled()) {
                    if (m.total_stack_weight > 0.0f) {
                        const float sum_pair_weight = m_top_weight + m_bot_weight;
                        const float pair_h = static_cast<float>(m.avail_h) * (sum_pair_weight / m.total_stack_weight);
                        if (pair_h > 80.0f) {
                            const float initial_top_h = pair_h * (m_top_weight / sum_pair_weight);
                            const float new_top_h =
                                std::clamp(initial_top_h + static_cast<float>(dy), 40.0f, pair_h - 40.0f);
                            const float next_top = sum_pair_weight * (new_top_h / pair_h);
                            const float next_bot = sum_pair_weight * (1.0f - (new_top_h / pair_h));
                            if (next_top != m_layout.weight_of(m_top_win) ||
                                next_bot != m_layout.weight_of(m_bot_win)) {
                                m_layout.set_weight(m_top_win, next_top);
                                m_layout.set_weight(m_bot_win, next_bot);
                                changed = true;
                            }
                        }
                    }
                }

                return changed;
            }

            [[nodiscard]] bool is_valid(const Context& ctx) const noexcept override {
                const Neighbors n = tiled_neighbors(ctx.windows, m_window);
                if (n.count <= 1 || !n.found) {
                    return false;
                }
                auto valid_stack_member = [&](const Window* w) {
                    return !w || (w != n.master && std::ranges::contains(ctx.windows, w) && w->is_tiled());
                };
                return valid_stack_member(m_top_win) && valid_stack_member(m_bot_win);
            }

            [[nodiscard]] bool involves_window(const Window* window) const noexcept override {
                return m_window == window || m_top_win == window || m_bot_win == window;
            }

        private:
            Master& m_layout;
            Window* m_window = nullptr;
            uint32_t m_edges = WLR_EDGE_NONE;
            float m_initial_master_factor = 0.5f;
            Window* m_top_win = nullptr;
            Window* m_bot_win = nullptr;
            float m_top_weight = 1.0f;
            float m_bot_weight = 1.0f;
        };

        std::unique_ptr<ResizeSession> Master::begin_resize(const Context& ctx, Window* window, double cursor_x,
                                                            double cursor_y) {
            if (!window) {
                return nullptr;
            }
            const Neighbors n = tiled_neighbors(ctx.windows, window);
            if (n.count <= 1 || !n.found) {
                return nullptr;
            }
            const uint32_t edges = compute_resize_edges(n, window, cursor_x, cursor_y);

            Window* top_win = nullptr;
            Window* bot_win = nullptr;
            float top_weight = 1.0f;
            float bot_weight = 1.0f;
            if (n.count > 2 && n.index > 0) {
                if ((edges & WLR_EDGE_BOTTOM) && n.next) {
                    top_win = window;
                    bot_win = n.next;
                } else if ((edges & WLR_EDGE_TOP) && n.index > 1) {
                    top_win = n.prev;
                    bot_win = window;
                }
                if (top_win && bot_win) {
                    top_weight = weight_of(top_win);
                    bot_weight = weight_of(bot_win);
                }
            }

            return std::make_unique<MasterResizeSession>(*this, window, edges, m_master_factor, top_win, bot_win,
                                                         top_weight, bot_weight);
        }

    }

    namespace detail {

        std::unique_ptr<Base> create_master() {
            return std::make_unique<Master>();
        }

    }

}
