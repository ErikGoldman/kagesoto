#pragma once

#include "ecs/ecs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct Position {
    int x = 0;
    int y = 0;
};

struct Velocity {
    float dx = 0.0f;
    float dy = 0.0f;
};

struct Health {
    int value = 0;
};

struct Active {};

struct Disabled {};

struct GameTime {
    int tick = 0;
};

struct DebugScalars {
    bool enabled = false;
    std::uint32_t u32 = 0;
    std::int64_t i64 = 0;
    std::uint64_t u64 = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
};

struct DebugNested {
    Position position;
    std::int32_t values[2]{};
};

struct TrackerCounts {
    int constructed = 0;
    int destroyed = 0;
};

struct Tracker {
    TrackerCounts* counts = nullptr;
    int value = 0;

    Tracker(TrackerCounts& tracker_counts, int next_value)
        : counts(&tracker_counts), value(next_value) {
        ++counts->constructed;
    }

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    Tracker(Tracker&& other) noexcept
        : counts(other.counts), value(other.value) {
        if (counts != nullptr) {
            ++counts->constructed;
        }
    }

    Tracker& operator=(Tracker&& other) noexcept {
        if (this != &other) {
            if (counts != nullptr) {
                ++counts->destroyed;
            }

            counts = other.counts;
            value = other.value;
        }

        return *this;
    }

    ~Tracker() {
        if (counts != nullptr) {
            ++counts->destroyed;
        }
    }
};

struct CopyableName {
    std::string value;
};

template <typename View, typename T, typename = void>
struct HasViewGet : std::false_type {};

template <typename View, typename T>
struct HasViewGet<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template get<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewCurrentGet : std::false_type {};

template <typename View, typename T>
struct HasViewCurrentGet<View, T, std::void_t<decltype(std::declval<View&>().template get<T>())>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewWrite : std::false_type {};

template <typename View, typename T>
struct HasViewWrite<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template write<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewCurrentWrite : std::false_type {};

template <typename View, typename T>
struct HasViewCurrentWrite<View, T, std::void_t<decltype(std::declval<View&>().template write<T>())>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewTryGet : std::false_type {};

template <typename View, typename T>
struct HasViewTryGet<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template try_get<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewCurrentTryGet : std::false_type {};

template <typename View, typename T>
struct HasViewCurrentTryGet<View, T, std::void_t<decltype(std::declval<View&>().template try_get<T>())>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewContains : std::false_type {};

template <typename View, typename T>
struct HasViewContains<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template contains<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewCurrentContains : std::false_type {};

template <typename View, typename T>
struct HasViewCurrentContains<View, T, std::void_t<decltype(std::declval<View&>().template contains<T>())>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewTagAdd : std::false_type {};

template <typename View, typename T>
struct HasViewTagAdd<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template add_tag<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewTagRemove : std::false_type {};

template <typename View, typename T>
struct HasViewTagRemove<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template remove_tag<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasSingletonGet : std::false_type {};

template <typename Registry, typename T>
struct HasSingletonGet<Registry, T, std::void_t<decltype(std::declval<Registry&>().template get<T>())>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasSingletonWrite : std::false_type {};

template <typename Registry, typename T>
struct HasSingletonWrite<Registry, T, std::void_t<decltype(std::declval<Registry&>().template write<T>())>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasRegistryTryGet : std::false_type {};

template <typename Registry, typename T>
struct HasRegistryTryGet<
    Registry,
    T,
    std::void_t<decltype(std::declval<Registry&>().template try_get<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasRegistryContains : std::false_type {};

template <typename Registry, typename T>
struct HasRegistryContains<
    Registry,
    T,
    std::void_t<decltype(std::declval<Registry&>().template contains<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasRegistryAdd : std::false_type {};

template <typename Registry, typename T>
struct HasRegistryAdd<
    Registry,
    T,
    std::void_t<decltype(std::declval<Registry&>().template add<T>(std::declval<ecs::Entity>(), T{}))>>
    : std::true_type {};

template <typename Registry, typename T, typename = void>
struct HasRegistryRemove : std::false_type {};

template <typename Registry, typename T>
struct HasRegistryRemove<
    Registry,
    T,
    std::void_t<decltype(std::declval<Registry&>().template remove<T>(std::declval<ecs::Entity>()))>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewSingletonGet : std::false_type {};

template <typename View, typename T>
struct HasViewSingletonGet<View, T, std::void_t<decltype(std::declval<View&>().template get<T>())>>
    : std::true_type {};

template <typename View, typename T, typename = void>
struct HasViewSingletonWrite : std::false_type {};

template <typename View, typename T>
struct HasViewSingletonWrite<View, T, std::void_t<decltype(std::declval<View&>().template write<T>())>>
    : std::true_type {};

template <typename View, typename Components, typename = void>
struct HasViewNestedView : std::false_type {};

template <typename View, typename... Components>
struct HasViewNestedView<
    View,
    std::tuple<Components...>,
    std::void_t<decltype(std::declval<View&>().template view<Components...>())>>
    : std::true_type {};

}  // namespace

namespace ecs {

template <>
struct is_singleton_component<GameTime> : std::true_type {};

}  // namespace ecs
