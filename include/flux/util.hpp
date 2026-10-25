#pragma once

#include <vector>

namespace flux {

    template <typename Container, typename T>
    constexpr void erase_unique_ptr(Container& container, const T* target) noexcept {
        std::erase_if(container, [target](const auto& item) { return item.get() == target; });
    }

}
