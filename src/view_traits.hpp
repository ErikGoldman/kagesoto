#pragma once

#include <type_traits>

namespace ecs {

namespace detail {

template <typename T>
using component_base_t = std::remove_const_t<T>;

template <typename RegistryType, typename Component>
inline constexpr bool component_is_const_v =
    std::is_const_v<std::remove_reference_t<RegistryType>> || std::is_const_v<Component>;

template <typename RegistryType, typename Component>
using component_pointer_t = std::conditional_t<component_is_const_v<RegistryType, Component>,
                                               const component_base_t<Component>*,
                                               const component_base_t<Component>*>;

template <typename... Components>
struct unique_component_types : std::true_type {};

template <typename First, typename... Rest>
struct unique_component_types<First, Rest...>
    : std::bool_constant<((!std::is_same_v<component_base_t<First>, component_base_t<Rest>>) && ...) &&
                         unique_component_types<Rest...>::value> {};

}  // namespace detail

}  // namespace ecs
