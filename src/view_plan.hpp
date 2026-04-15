#pragma once

#include <cstddef>
#include <tuple>

#include "view_traits.hpp"

namespace ecs {

namespace detail {

template <typename T>
struct type_tag {
    using type = T;
};

template <typename RegistryType, typename... Components>
struct ViewPlan {
    const RawPagedSparseArray* anchor_storage = nullptr;
    std::size_t anchor_index = 0;
    std::size_t anchor_size = 0;
};

template <typename RegistryType, typename... Components, std::size_t... Indices>
ViewPlan<RegistryType, Components...> make_view_plan_impl(RegistryType& registry, std::index_sequence<Indices...>) {
    ViewPlan<RegistryType, Components...> plan{};
    bool initialized = false;

    auto consider = [&](auto index_constant, auto component_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        using Component = typename decltype(component_constant)::type;
        using Base = component_base_t<Component>;

        const RawPagedSparseArray* storage = registry.storage(component_id<Base>());
        const std::size_t size = storage == nullptr ? 0 : storage->size();
        if (!initialized || size < plan.anchor_size) {
            initialized = true;
            plan.anchor_storage = storage;
            plan.anchor_index = index;
            plan.anchor_size = size;
        }
    };

    (consider(std::integral_constant<std::size_t, Indices>{}, type_tag<Components>{}), ...);
    return plan;
}

template <typename RegistryType, typename... Components>
ViewPlan<RegistryType, Components...> make_view_plan(RegistryType& registry) {
    return make_view_plan_impl<RegistryType, Components...>(registry, std::index_sequence_for<Components...>{});
}

}  // namespace detail

}  // namespace ecs
