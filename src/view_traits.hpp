#pragma once

#include <string_view>
#include <type_traits>

namespace ecs {

namespace detail {

template <typename T>
struct type_tag {
    using type = T;
};

template <typename T>
constexpr std::string_view type_name() {
#if defined(__clang__)
    constexpr std::string_view prefix = "std::string_view ecs::detail::type_name() [T = ";
    constexpr std::string_view suffix = "]";
    constexpr std::string_view name = __PRETTY_FUNCTION__;
#elif defined(__GNUC__)
    constexpr std::string_view prefix = "constexpr std::string_view ecs::detail::type_name() [with T = ";
    constexpr std::string_view suffix = "]";
    constexpr std::string_view name = __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    constexpr std::string_view prefix = "class std::basic_string_view<char,struct std::char_traits<char> > __cdecl ecs::detail::type_name<";
    constexpr std::string_view suffix = ">(void)";
    constexpr std::string_view name = __FUNCSIG__;
#else
    return "unknown";
#endif
    const std::size_t start = prefix.size();
    const std::size_t end = name.rfind(suffix);
    const std::string_view body = name.substr(start, end - start);
    const std::size_t separator = body.find(';');
    const std::string_view trimmed = separator == std::string_view::npos ? body : body.substr(0, separator);
    const std::size_t scope = trimmed.rfind("::");
    return scope == std::string_view::npos ? trimmed : trimmed.substr(scope + 2);
}

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

template <typename Query, typename... Components>
inline constexpr bool contains_component_v = (std::is_same_v<Query, component_base_t<Components>> || ...);

template <typename Query, typename... Components>
struct component_index_of;

template <typename Query, typename... Rest>
struct component_index_of<Query, Query, Rest...> : std::integral_constant<std::size_t, 0> {};

template <typename Query, typename First, typename... Rest>
struct component_index_of<Query, First, Rest...> {
    static constexpr std::size_t value = 1 + component_index_of<Query, component_base_t<Rest>...>::value;
};

template <typename Query>
struct component_index_of<Query> {
    static_assert(!std::is_same_v<Query, Query>, "component is not part of this view");
};

}  // namespace detail

}  // namespace ecs
