#include "frost/layout.hpp"
#include "frost/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frost::layout {

    namespace {

        struct Node {
            Window* window = nullptr;
            Node* parent = nullptr;
            std::unique_ptr<Node> first;
            std::unique_ptr<Node> second;
            bool side_by_side = true;
            float ratio = 0.5f;
            wlr_box box{};

            [[nodiscard]] bool is_leaf() const noexcept { return window != nullptr; }
        };

        void compute_child_boxes(const wlr_box& box, bool side_by_side, float ratio, int inner_gap, wlr_box& first_box,
                                 wlr_box& second_box) noexcept {
            const int total = side_by_side ? box.width : box.height;
            const int avail = std::max(1, total - inner_gap);
            const int min_sz = std::min(50, avail / 2);
            const int first_sz =
                std::max(1, std::clamp(static_cast<int>(std::round(avail * ratio)), min_sz, avail - min_sz));
            const int second_sz = std::max(1, avail - first_sz);

            if (side_by_side) {
                first_box = {.x = box.x, .y = box.y, .width = first_sz, .height = box.height};
                second_box = {.x = box.x + first_sz + inner_gap, .y = box.y, .width = second_sz, .height = box.height};
            } else {
                first_box = {.x = box.x, .y = box.y, .width = box.width, .height = first_sz};
                second_box = {.x = box.x, .y = box.y + first_sz + inner_gap, .width = box.width, .height = second_sz};
            }
        }

        void compute_boxes(Node* node, const wlr_box& box, int inner_gap) noexcept {
            if (!node) {
                return;
            }
            node->box = box;
            if (node->is_leaf()) {
                return;
            }
            wlr_box first_box{};
            wlr_box second_box{};
            compute_child_boxes(box, node->side_by_side, node->ratio, inner_gap, first_box, second_box);
            compute_boxes(node->first.get(), first_box, inner_gap);
            compute_boxes(node->second.get(), second_box, inner_gap);
        }

        [[nodiscard]] std::optional<wlr_box> projected_box(const Node* node, const wlr_box& box, const Node* target,
                                                           int inner_gap) noexcept {
            if (!node || !target) {
                return std::nullopt;
            }
            if (node == target) {
                return box;
            }
            if (node->is_leaf()) {
                return std::nullopt;
            }
            wlr_box first_box{};
            wlr_box second_box{};
            compute_child_boxes(box, node->side_by_side, node->ratio, inner_gap, first_box, second_box);
            if (const auto res = projected_box(node->first.get(), first_box, target, inner_gap)) {
                return res;
            }
            return projected_box(node->second.get(), second_box, target, inner_gap);
        }

        void apply_geometries(const Node* node, int border_width) noexcept {
            if (!node) {
                return;
            }
            if (node->is_leaf()) {
                if (node->window->is_tiled()) {
                    node->window->set_geometry(inset_border(node->box, border_width));
                }
                return;
            }
            apply_geometries(node->first.get(), border_width);
            apply_geometries(node->second.get(), border_width);
        }

        [[nodiscard]] Node* rightmost_leaf(Node* node) noexcept {
            while (node && !node->is_leaf()) {
                node = node->second.get();
            }
            return node;
        }

        [[nodiscard]] Window* find_leaf_at(const Node* node, double x, double y) noexcept {
            if (!node || !wlr_box_contains_point(&node->box, x, y)) {
                return nullptr;
            }
            if (node->is_leaf()) {
                return node->window->is_tiled() ? node->window : nullptr;
            }
            if (Window* hit = find_leaf_at(node->first.get(), x, y)) {
                return hit;
            }
            return find_leaf_at(node->second.get(), x, y);
        }

        [[nodiscard]] size_t focus_rank(std::span<Window* const> focus_stack, const Window* window) noexcept {
            const auto it = std::ranges::find(focus_stack, window);
            return (it != focus_stack.end()) ? static_cast<size_t>(it - focus_stack.begin())
                                             : std::numeric_limits<size_t>::max();
        }

        class Dwindle final : public Base {
        public:
            [[nodiscard]] Layout mode() const noexcept override { return Layout::Dwindle; }

            [[nodiscard]] uint64_t generation() const noexcept { return m_generation; }

            [[nodiscard]] Node* find_leaf(const Window* window) const noexcept {
                const auto it = m_leaves.find(window);
                return (it != m_leaves.end()) ? it->second : nullptr;
            }

            void arrange(const Context& ctx) override {
                std::vector<const Window*> stale;
                for (const auto& [win, _] : m_leaves) {
                    if (!std::ranges::contains(ctx.windows, win) || !win->is_tiled()) {
                        stale.push_back(win);
                    }
                }
                for (const Window* win : stale) {
                    remove_leaf(win);
                }

                const size_t tiled_count = static_cast<size_t>(std::ranges::count_if(ctx.windows, &Window::is_tiled));
                if (tiled_count == 0) {
                    return;
                }

                const WorkArea area = work_area(ctx.usable, ctx.config, tiled_count <= 1);
                const bool seeding = !m_root;
                if (!seeding) {
                    compute_boxes(m_root.get(), area.box, ctx.config.gaps.inner);
                }

                const std::span<Window* const> split_stack = seeding ? std::span<Window* const>{} : ctx.focus_stack;
                for (Window* w : ctx.windows) {
                    if (w->is_tiled() && !m_leaves.contains(w)) {
                        insert_window(w, split_stack, area.box, ctx.config.gaps.inner);
                    }
                }

                apply_geometries(m_root.get(), area.border_width);
            }

            void on_window_removed(const Window* window, size_t /*remaining_non_floating*/) noexcept override {
                remove_leaf(window);
            }

            void on_windows_swapped(Window* a, Window* b) override {
                Node* na = find_leaf(a);
                Node* nb = find_leaf(b);
                if (!na || !nb || na == nb) {
                    return;
                }
                na->window = b;
                nb->window = a;
                m_leaves[a] = nb;
                m_leaves[b] = na;
                ++m_generation;
            }

            [[nodiscard]] wlr_box next_tile_box(const Context& ctx) const noexcept override {
                const size_t tiled_count = static_cast<size_t>(std::ranges::count_if(ctx.windows, &Window::is_tiled));
                if (tiled_count == 0 || !m_root) {
                    const WorkArea area = work_area(ctx.usable, ctx.config, true);
                    return inset_border(area.box, area.border_width);
                }

                const WorkArea area = work_area(ctx.usable, ctx.config, false);
                const Node* target = split_target(ctx.focus_stack);
                const wlr_box target_box =
                    projected_box(m_root.get(), area.box, target, ctx.config.gaps.inner).value_or(area.box);

                const bool side_by_side = (target_box.width >= target_box.height);
                wlr_box first_box{};
                wlr_box second_box{};
                compute_child_boxes(target_box, side_by_side, 0.5f, ctx.config.gaps.inner, first_box, second_box);
                return inset_border(second_box, area.border_width);
            }

            [[nodiscard]] Window* window_in_direction(const Context& ctx, const Window* focused, Direction dir,
                                                      bool /*has_adj_output*/) const noexcept override {
                return neighbor_in_direction(ctx, focused, dir);
            }

            [[nodiscard]] Window* swap_target_in_direction(const Context& ctx, const Window* focused,
                                                           Direction dir) const noexcept override {
                return neighbor_in_direction(ctx, focused, dir);
            }

            [[nodiscard]] Window* window_at(const Context& /*ctx*/, double x, double y) const noexcept override {
                if (!m_root || m_root->is_leaf()) {
                    return nullptr;
                }
                return find_leaf_at(m_root.get(), x, y);
            }

            [[nodiscard]] std::unique_ptr<ResizeSession> begin_resize(const Context& ctx, Window* window,
                                                                      double cursor_x, double cursor_y) override;

        private:
            [[nodiscard]] Node* split_target(std::span<Window* const> focus_stack) const noexcept {
                for (const Window* w : focus_stack) {
                    if (Node* leaf = find_leaf(w)) {
                        return leaf;
                    }
                }
                return rightmost_leaf(m_root.get());
            }

            void remove_leaf(const Window* window) noexcept {
                const auto it = m_leaves.find(window);
                if (it == m_leaves.end()) {
                    return;
                }
                Node* leaf = it->second;
                m_leaves.erase(it);
                ++m_generation;

                Node* parent = leaf->parent;
                if (!parent) {
                    m_root.reset();
                    return;
                }

                std::unique_ptr<Node> sibling =
                    (parent->first.get() == leaf) ? std::move(parent->second) : std::move(parent->first);
                Node* grandparent = parent->parent;
                sibling->parent = grandparent;
                if (!grandparent) {
                    m_root = std::move(sibling);
                } else if (grandparent->first.get() == parent) {
                    grandparent->first = std::move(sibling);
                } else {
                    grandparent->second = std::move(sibling);
                }
            }

            void insert_window(Window* window, std::span<Window* const> focus_stack, const wlr_box& root_box,
                               int inner_gap) {
                if (!m_root) {
                    m_root = std::make_unique<Node>();
                    m_root->window = window;
                    m_root->box = root_box;
                    m_leaves[window] = m_root.get();
                    ++m_generation;
                    return;
                }

                Node* target = split_target(focus_stack);
                if (!target) {
                    return;
                }

                Node* parent = target->parent;
                std::unique_ptr<Node>& slot =
                    !parent ? m_root : (parent->first.get() == target ? parent->first : parent->second);

                auto split = std::make_unique<Node>();
                split->parent = parent;
                split->side_by_side = (target->box.width >= target->box.height);
                split->first = std::move(slot);
                split->first->parent = split.get();

                auto added = std::make_unique<Node>();
                added->window = window;
                added->parent = split.get();
                m_leaves[window] = added.get();
                split->second = std::move(added);

                slot = std::move(split);
                ++m_generation;
                compute_boxes(m_root.get(), root_box, inner_gap);
            }

            [[nodiscard]] Window* neighbor_in_direction(const Context& ctx, const Window* focused,
                                                        Direction dir) const noexcept {
                const Node* src = find_leaf(focused);
                if (!src || m_leaves.size() <= 1) {
                    return nullptr;
                }

                const wlr_box& s = src->box;
                const double src_cx = s.x + (s.width / 2.0);
                const double src_cy = s.y + (s.height / 2.0);

                Window* best = nullptr;
                int best_dist = std::numeric_limits<int>::max();
                size_t best_rank = std::numeric_limits<size_t>::max();
                int best_neg_overlap = 0;

                for (const auto& entry : m_leaves) {
                    const Node* cand = entry.second;
                    Window* cand_win = cand->window;
                    if (cand == src || !cand_win->is_tiled()) {
                        continue;
                    }
                    const wlr_box& c = cand->box;
                    const double cand_cx = c.x + (c.width / 2.0);
                    const double cand_cy = c.y + (c.height / 2.0);

                    int dist = 0;
                    int overlap = 0;
                    bool aligned = false;
                    switch (dir) {
                        case Direction::Left:
                            dist = s.x - (c.x + c.width);
                            overlap = std::min(s.y + s.height, c.y + c.height) - std::max(s.y, c.y);
                            aligned = dist >= -1 && cand_cx < src_cx;
                            break;
                        case Direction::Right:
                            dist = c.x - (s.x + s.width);
                            overlap = std::min(s.y + s.height, c.y + c.height) - std::max(s.y, c.y);
                            aligned = dist >= -1 && cand_cx > src_cx;
                            break;
                        case Direction::Up:
                            dist = s.y - (c.y + c.height);
                            overlap = std::min(s.x + s.width, c.x + c.width) - std::max(s.x, c.x);
                            aligned = dist >= -1 && cand_cy < src_cy;
                            break;
                        case Direction::Down:
                            dist = c.y - (s.y + s.height);
                            overlap = std::min(s.x + s.width, c.x + c.width) - std::max(s.x, c.x);
                            aligned = dist >= -1 && cand_cy > src_cy;
                            break;
                    }

                    if (!aligned || overlap <= 0) {
                        continue;
                    }

                    const int primary = std::max(0, dist);
                    const size_t rank = focus_rank(ctx.focus_stack, cand_win);
                    const int neg_overlap = -overlap;
                    if (!best ||
                        std::tie(primary, rank, neg_overlap) < std::tie(best_dist, best_rank, best_neg_overlap)) {
                        best = cand_win;
                        best_dist = primary;
                        best_rank = rank;
                        best_neg_overlap = neg_overlap;
                    }
                }

                return best;
            }

            std::unique_ptr<Node> m_root;
            std::unordered_map<const Window*, Node*> m_leaves;
            uint64_t m_generation = 0;
        };

        class DwindleResizeSession final : public ResizeSession {
        public:
            DwindleResizeSession(Dwindle& layout, Window* window, uint32_t edges, uint64_t generation, Node* h_split,
                                 Node* v_split) noexcept
                : m_layout(layout), m_window(window), m_edges(edges), m_generation(generation), m_h_split(h_split),
                  m_v_split(v_split), m_initial_h_ratio(h_split ? h_split->ratio : 0.5f),
                  m_initial_v_ratio(v_split ? v_split->ratio : 0.5f) {}

            [[nodiscard]] uint32_t edges() const noexcept override { return m_edges; }

            [[nodiscard]] bool update(const Context& ctx, int dx, int dy) override {
                if (m_layout.generation() != m_generation) {
                    return false;
                }

                bool changed = false;
                if (m_h_split) {
                    const int avail_w = std::max(1, m_h_split->box.width - ctx.config.gaps.inner);
                    const float delta = static_cast<float>(dx) / static_cast<float>(avail_w);
                    const float next = std::clamp(m_initial_h_ratio + delta, 0.10f, 0.90f);
                    if (next != m_h_split->ratio) {
                        m_h_split->ratio = next;
                        changed = true;
                    }
                }
                if (m_v_split) {
                    const int avail_h = std::max(1, m_v_split->box.height - ctx.config.gaps.inner);
                    const float delta = static_cast<float>(dy) / static_cast<float>(avail_h);
                    const float next = std::clamp(m_initial_v_ratio + delta, 0.10f, 0.90f);
                    if (next != m_v_split->ratio) {
                        m_v_split->ratio = next;
                        changed = true;
                    }
                }
                return changed;
            }

            [[nodiscard]] bool is_valid(const Context& /*ctx*/) const noexcept override {
                return m_layout.generation() == m_generation;
            }

            [[nodiscard]] bool involves_window(const Window* window) const noexcept override {
                return m_window == window || m_layout.generation() != m_generation;
            }

        private:
            Dwindle& m_layout;
            Window* m_window = nullptr;
            uint32_t m_edges = WLR_EDGE_NONE;
            uint64_t m_generation = 0;
            Node* m_h_split = nullptr;
            Node* m_v_split = nullptr;
            float m_initial_h_ratio = 0.5f;
            float m_initial_v_ratio = 0.5f;
        };

        std::unique_ptr<ResizeSession> Dwindle::begin_resize(const Context& /*ctx*/, Window* window, double cursor_x,
                                                             double cursor_y) {
            const Node* leaf = find_leaf(window);
            if (!leaf || m_leaves.size() <= 1) {
                return nullptr;
            }

            Node* left_split = nullptr;
            Node* right_split = nullptr;
            Node* top_split = nullptr;
            Node* bottom_split = nullptr;
            for (const Node* cur = leaf; cur->parent; cur = cur->parent) {
                Node* p = cur->parent;
                const bool is_first = (p->first.get() == cur);
                if (p->side_by_side) {
                    if (is_first && !right_split) {
                        right_split = p;
                    } else if (!is_first && !left_split) {
                        left_split = p;
                    }
                } else {
                    if (is_first && !bottom_split) {
                        bottom_split = p;
                    } else if (!is_first && !top_split) {
                        top_split = p;
                    }
                }
            }

            const wlr_box& box = leaf->box;
            const bool prefer_left = cursor_x < box.x + (box.width / 2.0);
            const bool prefer_top = cursor_y < box.y + (box.height / 2.0);

            Node* h_split =
                prefer_left ? (left_split ? left_split : right_split) : (right_split ? right_split : left_split);
            uint32_t h_edge = WLR_EDGE_NONE;
            if (h_split) {
                h_edge = (h_split == left_split) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
            }

            Node* v_split =
                prefer_top ? (top_split ? top_split : bottom_split) : (bottom_split ? bottom_split : top_split);
            uint32_t v_edge = WLR_EDGE_NONE;
            if (v_split) {
                v_edge = (v_split == top_split) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
            }

            if (h_split && v_split) {
                const double edge_x = (h_edge == WLR_EDGE_LEFT) ? box.x : (box.x + box.width);
                const double edge_y = (v_edge == WLR_EDGE_TOP) ? box.y : (box.y + box.height);
                const double dist_x = std::abs(cursor_x - edge_x);
                const double dist_y = std::abs(cursor_y - edge_y);
                const bool near_h = dist_x < std::max(40.0, box.width * 0.35);
                const bool near_v = dist_y < std::max(40.0, box.height * 0.35);
                if (!(near_h && near_v)) {
                    const double norm_x = dist_x / std::max(1, box.width);
                    const double norm_y = dist_y / std::max(1, box.height);
                    if (norm_x <= norm_y) {
                        v_split = nullptr;
                        v_edge = WLR_EDGE_NONE;
                    } else {
                        h_split = nullptr;
                        h_edge = WLR_EDGE_NONE;
                    }
                }
            }

            const uint32_t edges = h_edge | v_edge;
            return std::make_unique<DwindleResizeSession>(*this, window, edges, m_generation, h_split, v_split);
        }

    }

    namespace detail {

        std::unique_ptr<Base> create_dwindle() {
            return std::make_unique<Dwindle>();
        }

    }

}
