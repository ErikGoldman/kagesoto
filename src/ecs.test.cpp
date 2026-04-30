#include "ecs/ecs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
struct HasViewWrite : std::false_type {};

template <typename View, typename T>
struct HasViewWrite<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template write<T>(std::declval<ecs::Entity>()))>>
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
struct HasViewContains : std::false_type {};

template <typename View, typename T>
struct HasViewContains<
    View,
    T,
    std::void_t<decltype(std::declval<View&>().template contains<T>(std::declval<ecs::Entity>()))>>
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

}  // namespace

namespace ecs {

template <>
struct is_singleton_component<GameTime> : std::true_type {};

}  // namespace ecs

TEST_CASE("entities are created, destroyed, and recycled with versions") {
    ecs::Registry registry;

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.alive(first));
    REQUIRE(registry.alive(second));
    REQUIRE(ecs::Registry::entity_index(first) == 8);
    REQUIRE(ecs::Registry::entity_index(second) == 9);
    REQUIRE(ecs::Registry::entity_version(first) == 1);

    REQUIRE(registry.destroy(first));
    REQUIRE_FALSE(registry.alive(first));

    const ecs::Entity recycled = registry.create();
    REQUIRE(registry.alive(recycled));
    REQUIRE(ecs::Registry::entity_index(recycled) == ecs::Registry::entity_index(first));
    REQUIRE(ecs::Registry::entity_version(recycled) == ecs::Registry::entity_version(first) + 1);
    REQUIRE_FALSE(registry.alive(first));
}

TEST_CASE("entity free list recycles destroyed indices in lifo order") {
    ecs::Registry registry;

    const ecs::Entity a = registry.create();
    const ecs::Entity b = registry.create();
    const ecs::Entity c = registry.create();

    REQUIRE(registry.destroy(a));
    REQUIRE(registry.destroy(c));

    const ecs::Entity first_reused = registry.create();
    const ecs::Entity second_reused = registry.create();

    REQUIRE(ecs::Registry::entity_index(first_reused) == ecs::Registry::entity_index(c));
    REQUIRE(ecs::Registry::entity_index(second_reused) == ecs::Registry::entity_index(a));
    REQUIRE(registry.alive(b));
}

TEST_CASE("typed components require explicit registration") {
    ecs::Registry registry;
    const ecs::Entity entity = registry.create();

    REQUIRE_THROWS_AS(registry.component<Position>(), std::logic_error);
    REQUIRE_THROWS_AS(registry.add<Position>(entity, Position{1, 2}), std::logic_error);
    REQUIRE_THROWS_AS(registry.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.write<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.remove<Position>(entity), std::logic_error);

    const ecs::Entity position_component = registry.register_component<Position>("Position");
    REQUIRE(registry.component<Position>() == position_component);
    REQUIRE(registry.component_info(position_component)->size == sizeof(Position));
    REQUIRE(registry.component_info(position_component)->alignment == alignof(Position));
}

TEST_CASE("component registration validates descriptors and duplicate names") {
    ecs::Registry registry;

    ecs::ComponentDesc zero_size;
    zero_size.name = "ZeroSize";
    zero_size.size = 0;
    zero_size.alignment = alignof(Position);
    REQUIRE_THROWS_AS(registry.register_component(std::move(zero_size)), std::invalid_argument);

    ecs::ComponentDesc zero_alignment;
    zero_alignment.name = "ZeroAlignment";
    zero_alignment.size = sizeof(Position);
    zero_alignment.alignment = 0;
    REQUIRE_THROWS_AS(registry.register_component(std::move(zero_alignment)), std::invalid_argument);

    ecs::ComponentDesc position_desc;
    position_desc.name = "Position";
    position_desc.size = sizeof(Position);
    position_desc.alignment = alignof(Position);
    const ecs::Entity runtime_position = registry.register_component(position_desc);

    REQUIRE(registry.register_component(position_desc) == runtime_position);
    REQUIRE(registry.register_component<Position>("Position") == runtime_position);
    REQUIRE(registry.component<Position>() == runtime_position);

    ecs::ComponentDesc different_size = position_desc;
    different_size.size = sizeof(Position) + 1;
    REQUIRE_THROWS_AS(registry.register_component(std::move(different_size)), std::logic_error);

    ecs::ComponentDesc different_alignment = position_desc;
    different_alignment.alignment = alignof(double);
    if (different_alignment.alignment != position_desc.alignment) {
        REQUIRE_THROWS_AS(registry.register_component(std::move(different_alignment)), std::logic_error);
    }

    REQUIRE_THROWS_AS(registry.register_component<Tracker>("Position"), std::logic_error);
    REQUIRE_THROWS_AS(registry.register_component<GameTime>("Position"), std::logic_error);
}

TEST_CASE("empty typed components are presence-only tags") {
    ecs::Registry registry;
    const ecs::Entity active_tag = registry.register_component<Active>("Active");

    REQUIRE(registry.component<Active>() == active_tag);
    REQUIRE(registry.component_info(active_tag)->size == 0);
    REQUIRE(registry.component_info(active_tag)->alignment == alignof(Active));
    REQUIRE(registry.component_info(active_tag)->tag);

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Active>(entity));
    REQUIRE(registry.has<Active>(entity));
    REQUIRE(registry.has(entity, active_tag));
    REQUIRE(registry.is_dirty<Active>(entity));
    REQUIRE(registry.debug_print(entity, active_tag) == "Active{}");

    REQUIRE(registry.clear_dirty<Active>(entity));
    REQUIRE_FALSE(registry.is_dirty<Active>(entity));
    REQUIRE(registry.remove<Active>(entity));
    REQUIRE_FALSE(registry.has<Active>(entity));
    REQUIRE(registry.is_dirty<Active>(entity));
    REQUIRE(registry.debug_print(entity, active_tag) == "<missing>");

    REQUIRE_THROWS_AS(registry.get(entity, active_tag), std::logic_error);
    REQUIRE_THROWS_AS(registry.write(entity, active_tag), std::logic_error);
    REQUIRE_THROWS_AS(registry.ensure(entity, active_tag), std::logic_error);
}

TEST_CASE("runtime tags can be registered added checked and removed") {
    ecs::Registry registry;
    const ecs::Entity visible = registry.register_tag("Visible");
    const ecs::Entity entity = registry.create();

    REQUIRE(registry.component_info(visible)->size == 0);
    REQUIRE(registry.component_info(visible)->alignment == 1);
    REQUIRE(registry.component_info(visible)->tag);
    REQUIRE(registry.register_tag("Visible") == visible);

    REQUIRE(registry.add_tag(entity, visible));
    REQUIRE(registry.has(entity, visible));
    REQUIRE(registry.remove_tag(entity, visible));
    REQUIRE_FALSE(registry.has(entity, visible));

    ecs::ComponentDesc desc;
    desc.name = "Visible";
    desc.size = sizeof(Position);
    desc.alignment = alignof(Position);
    REQUIRE_THROWS_AS(registry.register_component(std::move(desc)), std::logic_error);
    REQUIRE_THROWS_AS(registry.add(entity, visible), std::logic_error);
}

TEST_CASE("trivial components can be added, read, written, replaced, and removed") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity entity = registry.create();

    static_assert(HasRegistryContains<ecs::Registry, Position>::value, "regular components support contains");
    static_assert(HasRegistryTryGet<ecs::Registry, Position>::value, "regular components support optional reads");
    static_assert(!HasRegistryContains<ecs::Registry, GameTime>::value, "singletons do not need contains");
    static_assert(!HasRegistryTryGet<ecs::Registry, GameTime>::value, "singletons do not have optional reads");

    REQUIRE_FALSE(registry.contains<Position>(entity));
    REQUIRE(registry.try_get<Position>(entity) == nullptr);

    Position* added = registry.add<Position>(entity, Position{1, 2});
    REQUIRE(added != nullptr);
    REQUIRE(added->x == 1);
    REQUIRE(added->y == 2);
    REQUIRE(registry.contains<Position>(entity));
    REQUIRE(registry.try_get<Position>(entity)->x == 1);
    REQUIRE(registry.clear_dirty<Position>(entity));

    Position& writable = registry.write<Position>(entity);
    writable.x = 10;

    const Position& read = registry.get<Position>(entity);
    REQUIRE(read.x == 10);
    REQUIRE(read.y == 2);
    REQUIRE(registry.clear_dirty<Position>(entity));

    Position* replaced = registry.add<Position>(entity, Position{7, 8});
    REQUIRE(replaced == &writable);
    REQUIRE(replaced->x == 7);
    REQUIRE(replaced->y == 8);

    REQUIRE(registry.remove<Position>(entity));
    REQUIRE_FALSE(registry.contains<Position>(entity));
    REQUIRE(registry.is_dirty<Position>(entity));
    REQUIRE(registry.clear_dirty<Position>(entity));
    REQUIRE_FALSE(registry.clear_dirty<Position>(entity));
    REQUIRE_FALSE(registry.remove<Position>(entity));
}

TEST_CASE("dirty component iteration exposes current values and removal tombstones") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity destroyed = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 2}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{3, 4}) != nullptr);
    REQUIRE(registry.add<Position>(destroyed, Position{5, 6}) != nullptr);
    registry.clear_all_dirty<Position>();

    REQUIRE(registry.add<Position>(first, Position{7, 8}) != nullptr);
    REQUIRE(registry.remove<Position>(second));
    REQUIRE(registry.destroy(destroyed));

    std::vector<ecs::Entity> dirty_entities;
    std::vector<Position> dirty_values;
    registry.each_dirty<Position>([&](ecs::Entity entity, const void* value) {
        dirty_entities.push_back(entity);
        dirty_values.push_back(*static_cast<const Position*>(value));
    });

    REQUIRE(dirty_entities == std::vector<ecs::Entity>{first});
    REQUIRE(dirty_values.size() == 1);
    REQUIRE(dirty_values[0].x == 7);
    REQUIRE(dirty_values[0].y == 8);

    std::vector<ecs::Registry::ComponentRemoval> removals;
    registry.each_removed<Position>([&](ecs::Registry::ComponentRemoval removal) {
        removals.push_back(removal);
    });

    REQUIRE(removals.size() == 2);
    auto second_removal = std::find_if(removals.begin(), removals.end(), [&](const auto& removal) {
        return removal.entity_index == ecs::Registry::entity_index(second);
    });
    REQUIRE(second_removal != removals.end());
    REQUIRE_FALSE(second_removal->entity_destroyed);

    auto destroyed_removal = std::find_if(removals.begin(), removals.end(), [&](const auto& removal) {
        return removal.entity_index == ecs::Registry::entity_index(destroyed);
    });
    REQUIRE(destroyed_removal != removals.end());
    REQUIRE(destroyed_removal->entity_destroyed);

    registry.clear_all_dirty<Position>();

    int dirty_count = 0;
    int removal_count = 0;
    registry.each_dirty<Position>([&](ecs::Entity, const void*) {
        ++dirty_count;
    });
    registry.each_removed<Position>([&](ecs::Registry::ComponentRemoval) {
        ++removal_count;
    });
    REQUIRE(dirty_count == 0);
    REQUIRE(removal_count == 0);
}

TEST_CASE("runtime components use component entities and the shared write path") {
    ecs::Registry registry;

    ecs::ComponentDesc desc;
    desc.name = "Velocity";
    desc.size = sizeof(Velocity);
    desc.alignment = alignof(Velocity);
    const ecs::Entity velocity_component = registry.register_component(std::move(desc));

    const ecs::Entity entity = registry.create();
    const Velocity initial{1.5f, 2.5f};

    void* added = registry.add(entity, velocity_component, &initial);
    REQUIRE(added != nullptr);
    REQUIRE(static_cast<const Velocity*>(registry.get(entity, velocity_component))->dx == 1.5f);

    REQUIRE(registry.clear_dirty(entity, velocity_component));
    Velocity* writable = static_cast<Velocity*>(registry.write(entity, velocity_component));
    REQUIRE(writable != nullptr);
    writable->dy = 9.0f;
    REQUIRE(static_cast<const Velocity*>(registry.get(entity, velocity_component))->dy == 9.0f);

    REQUIRE(registry.remove(entity, velocity_component));
    REQUIRE(registry.get(entity, velocity_component) == nullptr);
}

TEST_CASE("runtime ensure creates zeroed components and returns existing storage") {
    ecs::Registry registry;

    ecs::ComponentDesc desc;
    desc.name = "Velocity";
    desc.size = sizeof(Velocity);
    desc.alignment = alignof(Velocity);
    const ecs::Entity velocity_component = registry.register_component(std::move(desc));

    const ecs::Entity invalid{};
    REQUIRE(registry.ensure(invalid, velocity_component) == nullptr);

    const ecs::Entity stale = registry.create();
    REQUIRE(registry.destroy(stale));
    REQUIRE(registry.ensure(stale, velocity_component) == nullptr);

    const ecs::Entity entity = registry.create();
    Velocity* ensured = static_cast<Velocity*>(registry.ensure(entity, velocity_component));
    REQUIRE(ensured != nullptr);
    REQUIRE(ensured->dx == 0.0f);
    REQUIRE(ensured->dy == 0.0f);
    REQUIRE(registry.is_dirty(entity, velocity_component));

    ensured->dx = 3.5f;
    REQUIRE(registry.clear_dirty(entity, velocity_component));

    Velocity* existing = static_cast<Velocity*>(registry.ensure(entity, velocity_component));
    REQUIRE(existing == ensured);
    REQUIRE(existing->dx == 3.5f);
    REQUIRE(registry.is_dirty(entity, velocity_component));
}

TEST_CASE("runtime ensure uses singleton storage for singleton component entities") {
    ecs::Registry registry;
    const ecs::Entity game_time_component = registry.register_component<GameTime>("GameTime");
    const ecs::Entity entity = registry.create();

    REQUIRE(registry.clear_dirty<GameTime>());

    GameTime* ensured = static_cast<GameTime*>(registry.ensure(entity, game_time_component));
    REQUIRE(ensured == &registry.write<GameTime>());
    ensured->tick = 17;

    REQUIRE(registry.get<GameTime>().tick == 17);
    REQUIRE(registry.is_dirty<GameTime>());
}

TEST_CASE("runtime byte add rejects non-trivial typed components") {
    ecs::Registry registry;
    const ecs::Entity tracker_component = registry.register_component<Tracker>("Tracker");
    const ecs::Entity entity = registry.create();
    TrackerCounts counts;
    Tracker value{counts, 1};

    REQUIRE_THROWS_AS(registry.add(entity, tracker_component, &value), std::logic_error);
}

TEST_CASE("dirty bits move with components in dense storage") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 1}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 2}) != nullptr);

    registry.clear_all_dirty<Position>();

    registry.write<Position>(second);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second).x == 2);
    REQUIRE(registry.is_dirty<Position>(second));
    REQUIRE(registry.is_dirty<Position>(first));
    REQUIRE(registry.clear_dirty<Position>(first));
    REQUIRE_FALSE(registry.is_dirty<Position>(first));
}

TEST_CASE("dirty bits are preserved for clean moved components during dense removal") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 1}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 2}) != nullptr);

    registry.clear_all_dirty<Position>();
    registry.write<Position>(first);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second).x == 2);
    REQUIRE_FALSE(registry.is_dirty<Position>(second));
}

TEST_CASE("singleton components are created at registration and expose no-entity access") {
    static_assert(HasSingletonGet<ecs::Registry, GameTime>::value, "singleton components can be read without entity");
    static_assert(HasSingletonWrite<ecs::Registry, GameTime>::value, "singleton components can be written without entity");
    static_assert(!HasSingletonGet<ecs::Registry, Position>::value, "regular components cannot be read without entity");
    static_assert(!HasSingletonWrite<ecs::Registry, Position>::value, "regular components cannot be written without entity");
    static_assert(!HasRegistryAdd<ecs::Registry, GameTime>::value, "singletons cannot be added per entity");
    static_assert(!HasRegistryRemove<ecs::Registry, GameTime>::value, "singletons cannot be removed per entity");
    static_assert(HasRegistryAdd<ecs::Registry, Position>::value, "regular components can be added per entity");
    static_assert(HasRegistryRemove<ecs::Registry, Position>::value, "regular components can be removed per entity");

    ecs::Registry registry;
    const ecs::Entity game_time_component = registry.register_component<GameTime>("GameTime");

    const GameTime& initial = registry.get<GameTime>();
    REQUIRE(initial.tick == 0);
    REQUIRE(registry.is_dirty<GameTime>());

    REQUIRE(registry.clear_dirty<GameTime>());
    REQUIRE_FALSE(registry.is_dirty<GameTime>());

    GameTime& writable = registry.write<GameTime>();
    writable.tick = 42;

    REQUIRE(registry.get<GameTime>().tick == 42);
    REQUIRE(registry.is_dirty<GameTime>());

    const ecs::Entity entity = registry.create();
    const GameTime replacement{99};
    REQUIRE(registry.add(entity, game_time_component, &replacement) != nullptr);
    REQUIRE(registry.get<GameTime>().tick == 99);
    REQUIRE_FALSE(registry.remove(entity, game_time_component));
}

TEST_CASE("views iterate entities that contain every requested component") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity position_only = registry.create();
    const ecs::Entity both_a = registry.create();
    const ecs::Entity velocity_only = registry.create();
    const ecs::Entity both_b = registry.create();

    REQUIRE(registry.add<Position>(position_only, Position{1, 1}) != nullptr);
    REQUIRE(registry.add<Position>(both_a, Position{2, 3}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_a, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Velocity>(velocity_only, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(both_b, Position{4, 5}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_b, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    auto view = registry.view<const Position, Velocity>();
    view.each([&](ecs::Entity entity, const Position& position, Velocity& velocity) {
        visited.push_back(entity);
        velocity.dy = static_cast<float>(position.x + position.y);
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == both_a);
    REQUIRE(visited[1] == both_b);
    REQUIRE(registry.get<Velocity>(both_a).dy == 5.0f);
    REQUIRE(registry.get<Velocity>(both_b).dy == 9.0f);
}

TEST_CASE("views filter on included and excluded typed tags") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Active>("Active");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity active = registry.create();
    const ecs::Entity inactive = registry.create();
    const ecs::Entity disabled = registry.create();

    REQUIRE(registry.add<Position>(active, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Position>(inactive, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Position>(disabled, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Active>(active));
    REQUIRE(registry.add<Active>(disabled));
    REQUIRE(registry.add<Disabled>(disabled));

    std::vector<ecs::Entity> visited;
    registry.view<const Position>()
        .with_tags<const Active>()
        .without_tags<const Disabled>()
        .each([&](ecs::Entity entity, const Position& position) {
            visited.push_back(entity);
            REQUIRE(position.x == 1);
        });

    REQUIRE(visited == std::vector<ecs::Entity>{active});
}

TEST_CASE("tag filter constness controls view-level tag mutation") {
    using MutableView = decltype(std::declval<ecs::Registry::View<Position>&>().template with_tags<Active>());
    using ConstView = decltype(std::declval<ecs::Registry::View<Position>&>().template with_tags<const Active>());
    using MutableWithoutView =
        decltype(std::declval<ecs::Registry::View<Position>&>().template without_tags<Disabled>());

    static_assert(HasViewTagAdd<MutableView, Active>::value, "non-const with tag can be added");
    static_assert(HasViewTagRemove<MutableView, Active>::value, "non-const with tag can be removed");
    static_assert(!HasViewTagAdd<ConstView, Active>::value, "const with tag cannot be added");
    static_assert(!HasViewTagRemove<ConstView, Active>::value, "const with tag cannot be removed");
    static_assert(HasViewTagAdd<MutableWithoutView, Disabled>::value, "non-const without tag can be added");

    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Active>("Active");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Active>(entity));

    int calls = 0;
    registry.view<Position>().with_tags<Active>().each([&](auto& view, ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        REQUIRE(view.template has_tag<Active>(current));
        REQUIRE(view.template remove_tag<Active>(current));
        ++calls;
    });

    REQUIRE(calls == 1);
    REQUIRE_FALSE(registry.has<Active>(entity));
}

TEST_CASE("runtime tag filters support mutable and readonly view access") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity selected_tag = registry.register_tag("Selected");
    const ecs::Entity hidden_tag = registry.register_tag("Hidden");

    const ecs::Entity selected = registry.create();
    const ecs::Entity hidden = registry.create();
    REQUIRE(registry.add<Position>(selected, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Position>(hidden, Position{2, 0}) != nullptr);
    REQUIRE(registry.add_tag(selected, selected_tag));
    REQUIRE(registry.add_tag(hidden, selected_tag));
    REQUIRE(registry.add_tag(hidden, hidden_tag));

    std::vector<ecs::Entity> readonly;
    auto readonly_view = registry.view<const Position>().with_tags({selected_tag}).without_tags({hidden_tag});
    readonly_view.each([&](auto& view, ecs::Entity entity, const Position&) {
        REQUIRE(view.has_tag(entity, selected_tag));
        readonly.push_back(entity);
    });
    REQUIRE(readonly == std::vector<ecs::Entity>{selected});
    REQUIRE_THROWS_AS(readonly_view.remove_tag(selected, selected_tag), std::logic_error);

    auto mutable_view = registry.view<Position>().with_mutable_tags({selected_tag});
    int calls = 0;
    mutable_view.each([&](auto& view, ecs::Entity entity, Position&) {
        REQUIRE(view.remove_tag(entity, selected_tag));
        ++calls;
    });

    REQUIRE(calls == 2);
    REQUIRE_FALSE(registry.has(selected, selected_tag));
    REQUIRE_FALSE(registry.has(hidden, selected_tag));
}

TEST_CASE("runtime tag filters refresh cached storage after view construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity selected_tag = registry.register_tag("Selected");
    const ecs::Entity hidden_tag = registry.register_tag("Hidden");

    const ecs::Entity selected = registry.create();
    const ecs::Entity hidden = registry.create();
    REQUIRE(registry.add<Position>(selected, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Position>(hidden, Position{2, 0}) != nullptr);

    auto selected_view = registry.view<const Position>().with_tags({selected_tag});
    auto visible_view = registry.view<const Position>().without_tags({hidden_tag});

    REQUIRE(registry.add_tag(selected, selected_tag));
    REQUIRE(registry.add_tag(hidden, hidden_tag));

    std::vector<ecs::Entity> selected_entities;
    selected_view.each([&](ecs::Entity entity, const Position&) {
        selected_entities.push_back(entity);
    });
    REQUIRE(selected_entities == std::vector<ecs::Entity>{selected});

    std::vector<ecs::Entity> visible_entities;
    visible_view.each([&](ecs::Entity entity, const Position&) {
        visible_entities.push_back(entity);
    });
    REQUIRE(visible_entities == std::vector<ecs::Entity>{selected});
}

TEST_CASE("mutable runtime tag filters refresh cached storage after view add") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity selected_tag = registry.register_tag("Selected");

    const ecs::Entity selected = registry.create();
    REQUIRE(registry.add<Position>(selected, Position{1, 0}) != nullptr);

    auto mutable_view = registry.view<Position>().with_mutable_tags({selected_tag});
    REQUIRE(mutable_view.add_tag(selected, selected_tag));

    int calls = 0;
    mutable_view.each([&](ecs::Entity entity, Position&) {
        REQUIRE(entity == selected);
        ++calls;
    });
    REQUIRE(calls == 1);
}

TEST_CASE("access views preserve access components while filtering tags") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Active>("Active");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Active>(entity));

    int calls = 0;
    registry.view<const Position>()
        .access<Velocity>()
        .with_tags<const Active>()
        .each([&](auto& view, ecs::Entity current, const Position& position) {
            Velocity& velocity = view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
            ++calls;
        });

    REQUIRE(calls == 1);
    REQUIRE(registry.get<Velocity>(entity).dx == 3.0f);
}

TEST_CASE("views expose gated get and write access for listed components") {
    using View = ecs::Registry::View<const Position, Velocity>;
    using AccessView = decltype(std::declval<ecs::Registry::View<Position>&>().template access<const Velocity, Health>());
    using ExplicitWriteView = decltype(std::declval<ecs::Registry::View<const Position>&>().template access<Position>());
    using SingletonView = ecs::Registry::View<const Position, GameTime>;
    using SingletonAccessView = decltype(std::declval<ecs::Registry::View<Position>&>().template access<GameTime>());

    static_assert(HasViewGet<View, Position>::value, "listed readonly component can be read");
    static_assert(HasViewGet<View, const Position>::value, "listed readonly component can be read as const");
    static_assert(HasViewGet<View, Velocity>::value, "listed mutable component can be read");
    static_assert(HasViewContains<View, Position>::value, "listed regular component can be checked");
    static_assert(HasViewTryGet<View, Position>::value, "listed regular component can be read optionally");
    static_assert(!HasViewGet<View, Tracker>::value, "absent component cannot be read through the view");
    static_assert(!HasViewWrite<View, Position>::value, "readonly component cannot be written through the view");
    static_assert(!HasViewWrite<View, const Velocity>::value, "const write query is not writable");
    static_assert(HasViewWrite<View, Velocity>::value, "mutable component can be written through the view");
    static_assert(!HasViewWrite<View, Tracker>::value, "absent component cannot be written through the view");
    static_assert(HasViewGet<AccessView, Position>::value, "iterated component stays readable on access view");
    static_assert(HasViewWrite<AccessView, Position>::value, "mutable iterated component stays writable on access view");
    static_assert(HasViewGet<AccessView, Velocity>::value, "access component can be read");
    static_assert(!HasViewWrite<AccessView, Velocity>::value, "const access component cannot be written");
    static_assert(HasViewWrite<AccessView, Health>::value, "mutable access component can be written");
    static_assert(!HasViewGet<AccessView, Tracker>::value, "absent component cannot be read through access view");
    static_assert(HasViewGet<ExplicitWriteView, Position>::value, "overlapped iterated component can be read");
    static_assert(HasViewWrite<ExplicitWriteView, Position>::value, "mutable access overlap can be written explicitly");
    static_assert(HasViewSingletonGet<SingletonView, GameTime>::value, "listed singleton can be read without entity");
    static_assert(HasViewSingletonWrite<SingletonView, GameTime>::value, "mutable listed singleton can be written");
    static_assert(!HasViewSingletonGet<SingletonView, Position>::value, "regular listed component still requires entity");
    static_assert(HasViewSingletonGet<SingletonAccessView, GameTime>::value, "access singleton can be read without entity");
    static_assert(HasViewSingletonWrite<SingletonAccessView, GameTime>::value, "access singleton can be written without entity");
    static_assert(!HasViewContains<SingletonView, GameTime>::value, "singletons do not need view contains");
    static_assert(!HasViewTryGet<SingletonView, GameTime>::value, "singletons do not have view optional reads");
    static_assert(
        ecs::detail::access_components_allowed<ecs::detail::type_list<const Position>>::template with<Position>::value,
        "readonly iteration can overlap mutable access");
    static_assert(
        !ecs::detail::access_components_allowed<ecs::detail::type_list<Position>>::template with<Position>::value,
        "mutable iteration cannot overlap mutable access");
    static_assert(
        !ecs::detail::access_components_allowed<ecs::detail::type_list<const Position>>::template with<const Position>::
            value,
        "readonly iteration cannot overlap readonly access");

    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{8, 9}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 2.0f}) != nullptr);

    auto view = registry.view<const Position, Velocity>();
    REQUIRE(view.contains<Position>(entity));
    REQUIRE(view.try_get<Position>(entity)->x == 8);
    REQUIRE(view.get<Position>(entity).x == 8);

    Velocity& writable = view.write<Velocity>(entity);
    writable.dx = 6.0f;
    REQUIRE(registry.get<Velocity>(entity).dx == 6.0f);
}

TEST_CASE("singleton components in views are callback arguments without filtering entities") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<GameTime>("GameTime");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Position>(first, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{3, 0}) != nullptr);
    registry.clear_dirty<GameTime>();

    std::vector<ecs::Entity> visited;
    registry.view<const Position, GameTime>().each([&](ecs::Entity entity, const Position& position, GameTime& time) {
        visited.push_back(entity);
        time.tick += position.x;
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == first);
    REQUIRE(visited[1] == second);
    REQUIRE(registry.get<GameTime>().tick == 5);
    REQUIRE(registry.is_dirty<GameTime>());
}

TEST_CASE("singleton-only views call once with an invalid entity") {
    ecs::Registry registry;
    registry.register_component<GameTime>("GameTime");

    int calls = 0;
    registry.view<GameTime>().each([&](ecs::Entity entity, GameTime& time) {
        REQUIRE_FALSE(entity);
        time.tick = 7;
        ++calls;
    });

    REQUIRE(calls == 1);
    REQUIRE(registry.get<GameTime>().tick == 7);
}

TEST_CASE("mutable view iteration marks listed components dirty automatically") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{8, 9}) != nullptr);
    registry.clear_all_dirty<Position>();
    REQUIRE_FALSE(registry.is_dirty<Position>(entity));

    int calls = 0;
    registry.view<Position>().each([&](ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        REQUIRE(position.x == 8);
        ++calls;
    });

    REQUIRE(calls == 1);
    REQUIRE(registry.is_dirty<Position>(entity));
}

TEST_CASE("views over registered components with missing storage are empty") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    int calls = 0;
    registry.view<Position, Velocity>().each([&](ecs::Entity, Position&, Velocity&) {
        ++calls;
    });

    REQUIRE(calls == 0);
}

TEST_CASE("views can access extra components without changing iteration") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");
    registry.register_component<GameTime>("GameTime");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity target = registry.create();

    REQUIRE(registry.add<Position>(first, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(target, Velocity{4.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(target, Health{10}) != nullptr);

    int calls = 0;
    auto view = registry.view<Position>().access<const Velocity, Health, GameTime>();
    view.each([&](auto& active_view, ecs::Entity entity, Position& position) {
        REQUIRE(active_view.template get<Velocity>(target).dx == 4.0f);
        REQUIRE_FALSE(active_view.template contains<Health>(entity));

        Health& health = active_view.template write<Health>(target);
        health.value += position.x;
        active_view.template write<GameTime>().tick += position.x;
        ++calls;
    });

    REQUIRE(calls == 2);
    REQUIRE(registry.get<Health>(target).value == 15);
    REQUIRE(registry.get<GameTime>().tick == 5);
}

TEST_CASE("const iterated components can be explicitly written through access") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{2, 3}) != nullptr);
    registry.clear_all_dirty<Position>();

    auto view = registry.view<const Position>().access<Position>();

    int read_calls = 0;
    view.each([&](auto& active_view, ecs::Entity current, const Position& position) {
        static_assert(
            std::is_const<typename std::remove_reference<decltype(position)>::type>::value,
            "overlapped mutable access must keep the iterated callback argument const");

        REQUIRE(current == entity);
        REQUIRE(position.x == 2);
        REQUIRE(active_view.template get<Position>(current).y == 3);
        REQUIRE_FALSE(registry.is_dirty<Position>(current));
        ++read_calls;
    });

    REQUIRE(read_calls == 1);
    REQUIRE_FALSE(registry.is_dirty<Position>(entity));

    int write_calls = 0;
    view.each([&](auto& active_view, ecs::Entity current, const Position& position) {
        Position& writable = active_view.template write<Position>(current);
        writable.x = position.x + 5;
        ++write_calls;
    });

    REQUIRE(write_calls == 1);
    REQUIRE(registry.get<Position>(entity).x == 7);
    REQUIRE(registry.is_dirty<Position>(entity));
}

TEST_CASE("missing access-only storage does not suppress view iteration") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    int calls = 0;
    registry.view<Position>().access<Health>().each([&](auto& active_view, ecs::Entity current, Position&) {
        REQUIRE_FALSE(active_view.template contains<Health>(current));
        ++calls;
    });

    REQUIRE(calls == 1);
}

TEST_CASE("views keep storage pointers captured at construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    auto view_before_storage = registry.view<Position>();

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    int stale_view_calls = 0;
    view_before_storage.each([&](ecs::Entity, Position&) {
        ++stale_view_calls;
    });
    REQUIRE(stale_view_calls == 0);

    auto view_after_storage = registry.view<Position>();
    int live_view_calls = 0;
    view_after_storage.each([&](ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        REQUIRE(position.x == 1);
        ++live_view_calls;
    });
    REQUIRE(live_view_calls == 1);
}

TEST_CASE("view iteration tolerates removing non-driver components from current entity") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        visited.push_back(entity);
        REQUIRE(registry.remove<Velocity>(entity));
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == first);
    REQUIRE(visited[1] == second);
    REQUIRE(registry.contains<Position>(first));
    REQUIRE(registry.contains<Position>(second));
    REQUIRE_FALSE(registry.contains<Velocity>(first));
    REQUIRE_FALSE(registry.contains<Velocity>(second));
}

TEST_CASE("view iteration tolerates later entities gaining non-driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Velocity>(later).dx == 2.0f);
}

TEST_CASE("view iteration tolerates later entities losing non-driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Velocity>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE(registry.contains<Position>(later));
    REQUIRE_FALSE(registry.contains<Velocity>(later));
}

TEST_CASE("view iteration tolerates later entities gaining driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Position>(later).x == 2);
}

TEST_CASE("view iteration tolerates removing driver components from current entity") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        REQUIRE(registry.remove<Position>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ecs::Entity entity : visited) {
        REQUIRE_FALSE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }

    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ecs::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }
}

TEST_CASE("view iteration tolerates later entities losing driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Position>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE_FALSE(registry.contains<Position>(later));
    REQUIRE(registry.contains<Velocity>(later));
}

TEST_CASE("jobs run views in order and preserve insertion order for ties") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    std::vector<int> calls;
    registry.job<Position>(10).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(10);
    });
    registry.job<Position>(-1).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(-1);
    });
    registry.job<Position>(10).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(11);
    });

    REQUIRE(calls.empty());
    registry.run_jobs();

    REQUIRE(calls == std::vector<int>{-1, 10, 11});
}

TEST_CASE("job registration returns alive entities and the orchestrator schedules them") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity job = registry.job<const Position>(0).each([](ecs::Entity, const Position&) {});

    REQUIRE(registry.alive(job));
    REQUIRE(registry.has(job, registry.system_tag()));

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();
    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{job});
}

TEST_CASE("run jobs can exclude jobs by job entity tag") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Disabled>(job));

    const ecs::Entity disabled = registry.component<Disabled>();
    ecs::RunJobsOptions options;
    options.excluded_job_tags = &disabled;
    options.excluded_job_tag_count = 1;
    registry.run_jobs(options);
    REQUIRE(calls == 0);

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("run jobs for entities can exclude jobs by job entity tag") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Disabled>(job));

    const ecs::Entity disabled = registry.component<Disabled>();
    ecs::RunJobsOptions options;
    options.excluded_job_tags = &disabled;
    options.excluded_job_tag_count = 1;
    registry.run_jobs_for_entities({entity}, options);
    REQUIRE(calls == 0);

    registry.run_jobs_for_entities({entity});
    REQUIRE(calls == 1);
}

TEST_CASE("jobs can filter iterated entities by typed tags") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");
    registry.register_component<Active>("Active");
    registry.register_component<Disabled>("Disabled");

    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 4; ++i) {
        const ecs::Entity entity = registry.create();
        entities.push_back(entity);
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        REQUIRE(registry.add<Health>(entity, Health{0}) != nullptr);
    }
    REQUIRE(registry.add<Disabled>(entities[1]));
    REQUIRE(registry.add<Disabled>(entities[3]));

    registry.job<Position>(0)
        .without_tags<const Disabled>()
        .access<Health>()
        .max_threads(4)
        .min_entities_per_thread(1)
        .each([](auto& view, ecs::Entity entity, Position& position) {
            position.x += 10;
            view.template write<Health>(entity).value += 1;
        });
    registry.job<const Position>(1)
        .without_tags<const Disabled>()
        .structural<Active>()
        .each([](auto& view, ecs::Entity entity, const Position&) {
            REQUIRE(view.template add<Active>(entity));
        });

    registry.set_job_thread_executor([](const std::vector<ecs::JobThreadTask>& tasks) {
        for (const ecs::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(registry.get<Position>(entities[0]).x == 10);
    REQUIRE(registry.get<Position>(entities[1]).x == 1);
    REQUIRE(registry.get<Position>(entities[2]).x == 12);
    REQUIRE(registry.get<Position>(entities[3]).x == 3);
    REQUIRE(registry.get<Health>(entities[0]).value == 1);
    REQUIRE(registry.get<Health>(entities[1]).value == 0);
    REQUIRE(registry.get<Health>(entities[2]).value == 1);
    REQUIRE(registry.get<Health>(entities[3]).value == 0);
    REQUIRE(registry.has<Active>(entities[0]));
    REQUIRE_FALSE(registry.has<Active>(entities[1]));
    REQUIRE(registry.has<Active>(entities[2]));
    REQUIRE_FALSE(registry.has<Active>(entities[3]));
}

TEST_CASE("orchestrator returns no stages when no jobs are registered") {
    ecs::Registry registry;

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.empty());
}

TEST_CASE("orchestrator batches read-only jobs for parallel execution") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.job<const Position>(10).each([](ecs::Entity, const Position&) {});
    const ecs::Entity second = registry.job<const Position>(-1).each([](ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{second, first});
}

TEST_CASE("orchestrator reuses stable schedules and invalidates them after job registration") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity reader = registry.job<const Position>(0).each([](ecs::Entity, const Position&) {});

    const ecs::JobSchedule first_schedule = ecs::Orchestrator(registry).schedule();
    const ecs::JobSchedule second_schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(first_schedule.stages.size() == 1);
    REQUIRE(first_schedule.stages[0].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(second_schedule.stages.size() == first_schedule.stages.size());
    REQUIRE(second_schedule.stages[0].jobs == first_schedule.stages[0].jobs);

    const ecs::Entity writer = registry.job<Position>(1).access<Velocity>().each(
        [](auto&, ecs::Entity, Position&) {});

    const ecs::JobSchedule updated_schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(updated_schedule.stages.size() == 2);
    REQUIRE(updated_schedule.stages[0].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(updated_schedule.stages[1].jobs == std::vector<ecs::Entity>{writer});
}

TEST_CASE("orchestrator orders conflicting read and write jobs by canonical job order") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity writer = registry.job<Position>(10).each([](ecs::Entity, Position&) {});
    const ecs::Entity reader = registry.job<const Position>(20).each([](ecs::Entity, const Position&) {});
    const ecs::Entity later_writer = registry.job<Position>(20).each([](ecs::Entity, Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 3);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(schedule.stages[2].jobs == std::vector<ecs::Entity>{later_writer});
}

TEST_CASE("orchestrator batches independent writes") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity position_writer = registry.job<Position>(0).each([](ecs::Entity, Position&) {});
    const ecs::Entity velocity_writer = registry.job<Velocity>(1).each([](ecs::Entity, Velocity&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{position_writer, velocity_writer});
}

TEST_CASE("orchestrator includes access view components in job conflicts") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity velocity_writer = registry.job<const Position>(0).access<Velocity>().each(
        [](auto&, ecs::Entity, const Position&) {});
    const ecs::Entity velocity_reader = registry.job<const Velocity>(1).each([](ecs::Entity, const Velocity&) {});
    const ecs::Entity position_reader = registry.job<const Position>(2).access<const Velocity>().each(
        [](auto&, ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{velocity_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{velocity_reader, position_reader});
}

TEST_CASE("jobs are persistent and use access views") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access<Velocity>().each(
        [&](auto& active_view, ecs::Entity current, const Position& position) {
            Velocity& velocity = active_view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
        });

    registry.run_jobs();
    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity).dx == 5.0f);
}

TEST_CASE("jobs use live views when they run") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    int calls = 0;
    registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("mutable job views mark iterated components dirty") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{3, 0}) != nullptr);
    registry.clear_all_dirty<Position>();

    registry.job<Position>(0).each([&](ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        REQUIRE(position.x == 3);
    });

    REQUIRE_FALSE(registry.is_dirty<Position>(entity));
    registry.run_jobs();
    REQUIRE(registry.is_dirty<Position>(entity));
}

TEST_CASE("jobs added while jobs are running wait until the next run") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int outer_calls = 0;
    int inner_calls = 0;
    registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++outer_calls;
        registry.job<Position>(-1).each([&](ecs::Entity, Position&) {
            ++inner_calls;
        });
    });

    registry.run_jobs();
    REQUIRE(outer_calls == 1);
    REQUIRE(inner_calls == 0);

    registry.run_jobs();
    REQUIRE(outer_calls == 2);
    REQUIRE(inner_calls == 1);
}

TEST_CASE("run jobs batches independent jobs through the executor") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<Position>(0).each([](ecs::Entity, Position&) {});
    registry.job<Velocity>(1).each([](ecs::Entity, Velocity&) {});

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ecs::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ecs::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{2});
}

TEST_CASE("threaded jobs split entity ranges using max threads and minimum entity counts") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 5; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    registry.job<Position>(0).max_threads(3).min_entities_per_thread(2).each([](ecs::Entity, Position& position) {
        position.y = position.x + 10;
    });

    std::vector<std::size_t> thread_indices;
    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ecs::JobThreadTask>& tasks) {
        for (const ecs::JobThreadTask& task : tasks) {
            thread_indices.push_back(task.thread_index);
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_indices == std::vector<std::size_t>{0, 1, 2});
    REQUIRE(thread_counts == std::vector<std::size_t>{3, 3, 3});

    int visited = 0;
    registry.view<const Position>().each([&](ecs::Entity, const Position& position) {
        REQUIRE(position.y == position.x + 10);
        ++visited;
    });
    REQUIRE(visited == 5);
}

TEST_CASE("force single threaded run ignores executor chunking") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    int calls = 0;
    registry.job<Position>(0).max_threads(4).min_entities_per_thread(1).each([&](ecs::Entity, Position&) {
        ++calls;
    });

    int executor_calls = 0;
    registry.set_job_thread_executor([&](const std::vector<ecs::JobThreadTask>& tasks) {
        ++executor_calls;
        for (const ecs::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs(ecs::RunJobsOptions{true});

    REQUIRE(calls == 4);
    REQUIRE(executor_calls == 0);
}

TEST_CASE("structural jobs expose declared add and remove operations and stay single threaded") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.job<const Position>(0).max_threads(4).min_entities_per_thread(1).structural<Disabled>().each(
        [](auto& view, ecs::Entity current, const Position&) {
            REQUIRE(view.template add<Disabled>(current));
        });

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ecs::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ecs::JobThreadTask& task : tasks) {
            REQUIRE(task.thread_count == 1);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{1});
    REQUIRE(registry.has<Disabled>(entity));

    registry.job<const Position>(1).structural<Disabled>().each([](auto& view, ecs::Entity current, const Position&) {
        REQUIRE(view.template remove<Disabled>(current));
    });

    registry.run_jobs();

    REQUIRE_FALSE(registry.has<Disabled>(entity));
}

TEST_CASE("structural jobs are isolated from otherwise independent jobs") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity structural =
        registry.job<const Position>(0).structural<Disabled>().each([](auto&, ecs::Entity, const Position&) {});
    const ecs::Entity independent = registry.job<Velocity>(1).each([](ecs::Entity, Velocity&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{structural});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{independent});
}

TEST_CASE("structural access jobs can use access views and declared structural operations") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access<Velocity>().structural<Disabled>().each(
        [](auto& view, ecs::Entity current, const Position& position) {
            Velocity& velocity = view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
            REQUIRE(view.template add<Disabled>(current));
        });

    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity).dx == 3.0f);
    REQUIRE(registry.has<Disabled>(entity));
}

TEST_CASE("declared owned groups are used by matching views and track membership") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity position_only = registry.create();
    const ecs::Entity both_a = registry.create();
    const ecs::Entity both_b = registry.create();

    REQUIRE(registry.add<Position>(position_only, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Position>(both_a, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_a, Velocity{20.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_b, Velocity{30.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(both_b, Position{3, 0}) != nullptr);

    registry.clear_all_dirty<Position>();
    registry.clear_all_dirty<Velocity>();

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position& position, Velocity& velocity) {
        visited.push_back(entity);
        velocity.dy = static_cast<float>(position.x);
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(registry.view<Position, Velocity>().get<Position>(both_a).x == 2);
    REQUIRE(registry.view<Position, Velocity>().write<Velocity>(both_b).dx == 30.0f);
    REQUIRE(registry.is_dirty<Position>(both_a));
    REQUIRE(registry.is_dirty<Velocity>(both_a));
    REQUIRE(registry.get<Velocity>(both_a).dy == 2.0f);
    REQUIRE(registry.get<Velocity>(both_b).dy == 3.0f);

    REQUIRE(registry.remove<Velocity>(both_a));
    REQUIRE(registry.add<Velocity>(position_only, Velocity{10.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> after_changes;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after_changes.push_back(entity);
    });
    REQUIRE(after_changes.size() == 2);
    REQUIRE(std::find(after_changes.begin(), after_changes.end(), position_only) != after_changes.end());
    REQUIRE(std::find(after_changes.begin(), after_changes.end(), both_b) != after_changes.end());
}

TEST_CASE("owned group view iteration tolerates later entities gaining components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Velocity>(later).dx == 2.0f);
}

TEST_CASE("owned group view iteration tolerates later entities losing components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Velocity>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE(registry.contains<Position>(later));
    REQUIRE_FALSE(registry.contains<Velocity>(later));
}

TEST_CASE("owned group view iteration tolerates removing components from current entity") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        REQUIRE(registry.remove<Velocity>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ecs::Entity entity : visited) {
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE_FALSE(registry.contains<Velocity>(entity));
    }

    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ecs::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }
}

TEST_CASE("owned group declarations allow identical groups but reject shared ownership") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{10}) != nullptr);

    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Velocity, Position>();

    REQUIRE_THROWS_AS((registry.declare_owned_group<Position>()), std::logic_error);
    REQUIRE_THROWS_AS((registry.declare_owned_group<Position, Velocity, Health>()), std::logic_error);
    REQUIRE_THROWS_AS((registry.declare_owned_group<Velocity, Health>()), std::logic_error);

    ecs::Registry conflict;
    conflict.register_component<Position>("Position");
    conflict.register_component<Velocity>("Velocity");
    conflict.register_component<Health>("Health");
    conflict.declare_owned_group<Position, Velocity>();
    REQUIRE_THROWS_AS((conflict.declare_owned_group<Position, Health>()), std::logic_error);
}

TEST_CASE("owned groups preserve non-trivial components while packing") {
    TrackerCounts counts;

    {
        ecs::Registry registry;
        registry.register_component<Tracker>("Tracker");
        registry.register_component<Position>("Position");

        const ecs::Entity first = registry.create();
        const ecs::Entity second = registry.create();
        const ecs::Entity third = registry.create();

        REQUIRE(registry.add<Tracker>(first, counts, 3) != nullptr);
        REQUIRE(registry.add<Position>(first, Position{3, 0}) != nullptr);
        REQUIRE(registry.add<Position>(second, Position{1, 0}) != nullptr);
        REQUIRE(registry.add<Tracker>(second, counts, 1) != nullptr);
        REQUIRE(registry.add<Tracker>(third, counts, 2) != nullptr);
        REQUIRE(registry.add<Position>(third, Position{2, 0}) != nullptr);

        registry.declare_owned_group<Tracker, Position>();

        std::vector<int> tracker_values;
        registry.view<Tracker, Position>().each([&](ecs::Entity, Tracker& tracker, Position& position) {
            tracker_values.push_back(tracker.value);
            REQUIRE(tracker.value == position.x);
        });

        REQUIRE(tracker_values.size() == 3);
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 1) != tracker_values.end());
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 2) != tracker_values.end());
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 3) != tracker_values.end());
        REQUIRE(registry.remove<Position>(second));
        int remaining = 0;
        registry.view<Tracker, Position>().each([&](ecs::Entity, Tracker&, Position&) {
            ++remaining;
        });
        REQUIRE(remaining == 2);
    }

    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial components are constructed, moved, and destroyed explicitly") {
    ecs::Registry registry;
    registry.register_component<Tracker>("Tracker");
    TrackerCounts counts;

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Tracker>(first, counts, 1) != nullptr);
    REQUIRE(registry.add<Tracker>(second, counts, 2) != nullptr);
    REQUIRE(registry.get<Tracker>(first).value == 1);
    REQUIRE(registry.get<Tracker>(second).value == 2);

    REQUIRE(registry.remove<Tracker>(first));
    REQUIRE_FALSE(registry.contains<Tracker>(first));
    REQUIRE(registry.get<Tracker>(second).value == 2);

    REQUIRE(registry.destroy(second));
    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial component replacement destroys the old value") {
    ecs::Registry registry;
    registry.register_component<Tracker>("Tracker");
    TrackerCounts counts;

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Tracker>(entity, counts, 1) != nullptr);
    REQUIRE(registry.get<Tracker>(entity).value == 1);

    Tracker* replacement = registry.add<Tracker>(entity, counts, 2);
    REQUIRE(replacement != nullptr);
    REQUIRE(replacement == &registry.get<Tracker>(entity));
    REQUIRE(replacement->value == 2);
    REQUIRE(counts.constructed > counts.destroyed);

    REQUIRE(registry.remove<Tracker>(entity));
    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial storage growth and middle removal preserve live values") {
    TrackerCounts counts;
    std::vector<ecs::Entity> entities;

    {
        ecs::Registry registry;
        registry.register_component<Tracker>("Tracker");

        for (int i = 0; i < 10; ++i) {
            const ecs::Entity entity = registry.create();
            entities.push_back(entity);
            REQUIRE(registry.add<Tracker>(entity, counts, i) != nullptr);
        }

        for (int i = 0; i < 10; ++i) {
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)]).value == i);
        }

        REQUIRE(registry.remove<Tracker>(entities[4]));
        REQUIRE_FALSE(registry.contains<Tracker>(entities[4]));
        for (int i = 0; i < 10; ++i) {
            if (i == 4) {
                continue;
            }
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)]).value == i);
        }
    }

    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("destroying an entity removes all of its components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<std::unique_ptr<int>>("OwnedInt");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{3, 4}) != nullptr);
    REQUIRE(registry.add<std::unique_ptr<int>>(entity, new int(9)) != nullptr);

    REQUIRE(registry.destroy(entity));
    REQUIRE_FALSE(registry.contains<Position>(entity));
    REQUIRE_FALSE(registry.contains<std::unique_ptr<int>>(entity));
    REQUIRE_FALSE(registry.remove<Position>(entity));
}

TEST_CASE("destroying a component entity unregisters the component") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const ecs::Entity entity = registry.create();

    REQUIRE(registry.add<Position>(entity, Position{5, 6}) != nullptr);
    REQUIRE(registry.destroy(position_component));
    REQUIRE(registry.component_info(position_component) == nullptr);
    REQUIRE_THROWS_AS(registry.component<Position>(), std::logic_error);
    REQUIRE_THROWS_AS(registry.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.add<Position>(entity, Position{1, 1}), std::logic_error);
}

TEST_CASE("moved registries retain entities components metadata singletons and dirty bits") {
    ecs::Registry source;
    const ecs::Entity position_component = source.register_component<Position>("Position");
    const ecs::Entity game_time_component = source.register_component<GameTime>("GameTime");
    const ecs::Entity entity = source.create();

    REQUIRE(source.add<Position>(entity, Position{5, 6}) != nullptr);
    REQUIRE(source.clear_dirty<Position>(entity));
    (void)source.write<Position>(entity);
    REQUIRE(source.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), source.primitive_type(ecs::PrimitiveType::I32), 1}}));
    source.write<GameTime>().tick = 12;

    ecs::Registry moved(std::move(source));

    REQUIRE(moved.alive(entity));
    REQUIRE(moved.component<Position>() == position_component);
    REQUIRE(moved.component<GameTime>() == game_time_component);
    REQUIRE(moved.get<Position>(entity).x == 5);
    REQUIRE(moved.get<Position>(entity).y == 6);
    REQUIRE(moved.is_dirty<Position>(entity));
    REQUIRE(moved.get<GameTime>().tick == 12);
    REQUIRE(moved.is_dirty<GameTime>());

    const std::vector<ecs::ComponentField>* fields = moved.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");
}

TEST_CASE("registry snapshots restore entities components metadata groups singletons and dirty bits") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    const ecs::Entity game_time_component = registry.register_component<GameTime>("GameTime");

    REQUIRE(registry.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ecs::PrimitiveType::I32), 1}}));

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity kept = registry.create();
    const ecs::Entity removed_before_snapshot = registry.create();
    REQUIRE(registry.add<Position>(kept, Position{1, 2}) != nullptr);
    REQUIRE(registry.add<Velocity>(kept, Velocity{3.0f, 4.0f}) != nullptr);
    REQUIRE(registry.clear_dirty<Position>(kept));
    registry.write<GameTime>().tick = 7;
    REQUIRE(registry.destroy(removed_before_snapshot));

    auto snapshot = registry.snapshot();

    const ecs::Entity reused_after_snapshot = registry.create();
    REQUIRE(ecs::Registry::entity_index(reused_after_snapshot) == ecs::Registry::entity_index(removed_before_snapshot));
    REQUIRE(registry.add<Position>(reused_after_snapshot, Position{9, 9}) != nullptr);
    REQUIRE(registry.remove<Velocity>(kept));
    registry.write<Position>(kept).x = 42;
    registry.write<GameTime>().tick = 99;
    registry.register_component<Health>("Health");

    registry.restore(snapshot);

    REQUIRE(registry.alive(kept));
    REQUIRE_FALSE(registry.alive(reused_after_snapshot));
    REQUIRE(registry.get<Position>(kept).x == 1);
    REQUIRE(registry.get<Position>(kept).y == 2);
    REQUIRE(registry.get<Velocity>(kept).dx == 3.0f);
    REQUIRE(registry.get<Velocity>(kept).dy == 4.0f);
    REQUIRE_FALSE(registry.is_dirty<Position>(kept));
    REQUIRE(registry.is_dirty<Velocity>(kept));
    REQUIRE(registry.component<Position>() == position_component);
    REQUIRE(registry.component<GameTime>() == game_time_component);
    REQUIRE(registry.get<GameTime>().tick == 7);
    REQUIRE(registry.is_dirty<GameTime>());
    REQUIRE_THROWS_AS(registry.component<Health>(), std::logic_error);

    const std::vector<ecs::ComponentField>* fields = registry.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");

    std::vector<ecs::Entity> grouped;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        grouped.push_back(entity);
    });
    REQUIRE(grouped == std::vector<ecs::Entity>{kept});

    const ecs::Entity reused_after_restore = registry.create();
    REQUIRE(reused_after_restore == reused_after_snapshot);
}

TEST_CASE("registry snapshots restore tag presence and dirty bits") {
    ecs::Registry registry;
    const ecs::Entity active_tag = registry.register_component<Active>("Active");
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Active>(entity));
    REQUIRE(registry.clear_dirty<Active>(entity));

    auto snapshot = registry.snapshot();

    REQUIRE(registry.remove<Active>(entity));
    REQUIRE_FALSE(registry.has<Active>(entity));

    registry.restore(snapshot);

    REQUIRE(registry.component<Active>() == active_tag);
    REQUIRE(registry.has<Active>(entity));
    REQUIRE_FALSE(registry.is_dirty<Active>(entity));
}

TEST_CASE("registry snapshots deep copy copyable non-trivial component storage") {
    ecs::Registry registry;
    registry.register_component<CopyableName>("CopyableName");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<CopyableName>(entity, CopyableName{"before"}) != nullptr);

    auto snapshot = registry.snapshot();
    registry.write<CopyableName>(entity).value = "after";

    registry.restore(snapshot);

    REQUIRE(registry.get<CopyableName>(entity).value == "before");
    registry.write<CopyableName>(entity).value = "restored";
    registry.restore(snapshot);
    REQUIRE(registry.get<CopyableName>(entity).value == "before");
}

TEST_CASE("registry snapshots reject move-only non-trivial component storage") {
    ecs::Registry registry;
    registry.register_component<std::unique_ptr<int>>("OwnedInt");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<std::unique_ptr<int>>(entity, new int(5)) != nullptr);

    REQUIRE_THROWS_AS(registry.snapshot(), std::logic_error);
}

TEST_CASE("restoring a registry snapshot leaves registered jobs unchanged") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    registry.job<Position>(0).each([&](ecs::Registry::View<Position>& view, ecs::Entity current, Position&) {
        ++calls;
        view.write<Position>(current).x += 1;
    });

    auto snapshot = registry.snapshot();
    REQUIRE(registry.add<Position>(entity, Position{10, 0}) != nullptr);

    registry.restore(snapshot);
    registry.run_jobs();

    REQUIRE(calls == 1);
    REQUIRE(registry.get<Position>(entity).x == 2);
}

TEST_CASE("registry snapshots exclude system-tagged job bookkeeping entities") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Position>(job, Position{99, 0}) != nullptr);

    auto snapshot = registry.snapshot();

    (void)registry.write<Position>(entity);
    registry.write<Position>(entity).x = 7;
    registry.write<Position>(job).x = 100;

    registry.restore(snapshot);

    REQUIRE(registry.alive(job));
    REQUIRE(registry.has(job, registry.system_tag()));
    REQUIRE(registry.get<Position>(entity).x == 1);
    REQUIRE_FALSE(registry.contains<Position>(job));

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();
    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{job});

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("delta snapshots restore dirty values additions removals and destroyed entities") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    source.register_component<Health>("Health");
    source.declare_owned_group<Position, Velocity>();

    const ecs::Entity updated = source.create();
    const ecs::Entity removed_component = source.create();
    const ecs::Entity destroyed = source.create();
    REQUIRE(source.add<Position>(updated, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Velocity>(updated, Velocity{1.0f, 1.0f}) != nullptr);
    REQUIRE(source.add<Position>(removed_component, Position{2, 2}) != nullptr);
    REQUIRE(source.add<Velocity>(removed_component, Velocity{2.0f, 2.0f}) != nullptr);
    REQUIRE(source.add<Position>(destroyed, Position{3, 3}) != nullptr);
    REQUIRE(source.add<Health>(destroyed, Health{30}) != nullptr);
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();
    source.clear_all_dirty<Health>();

    auto baseline = source.snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.register_component<Velocity>("Velocity");
    replay.register_component<Health>("Health");
    replay.declare_owned_group<Position, Velocity>();
    replay.restore(baseline);

    source.write<Position>(updated).x = 10;
    const ecs::Entity added = source.create();
    REQUIRE(source.add<Position>(added, Position{4, 4}) != nullptr);
    REQUIRE(source.add<Velocity>(added, Velocity{4.0f, 4.0f}) != nullptr);
    REQUIRE(source.remove<Velocity>(removed_component));
    REQUIRE(source.destroy(destroyed));

    auto delta = source.delta_snapshot(baseline);
    replay.restore(delta);

    REQUIRE(replay.alive(updated));
    REQUIRE(replay.get<Position>(updated).x == 10);
    REQUIRE(replay.get<Position>(updated).y == 1);
    REQUIRE(replay.get<Velocity>(updated).dx == 1.0f);
    REQUIRE(replay.alive(added));
    REQUIRE(replay.get<Position>(added).x == 4);
    REQUIRE(replay.get<Velocity>(added).dx == 4.0f);
    REQUIRE(replay.alive(removed_component));
    REQUIRE(replay.get<Position>(removed_component).x == 2);
    REQUIRE_FALSE(replay.contains<Velocity>(removed_component));
    REQUIRE_FALSE(replay.alive(destroyed));
    REQUIRE_FALSE(replay.contains<Position>(destroyed));
    REQUIRE_FALSE(replay.contains<Health>(destroyed));
    REQUIRE(replay.is_dirty<Position>(updated));
    REQUIRE(replay.is_dirty<Position>(added));
    REQUIRE(replay.is_dirty<Velocity>(removed_component));

    std::vector<ecs::Entity> grouped;
    replay.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        grouped.push_back(entity);
    });
    REQUIRE(std::find(grouped.begin(), grouped.end(), updated) != grouped.end());
    REQUIRE(std::find(grouped.begin(), grouped.end(), added) != grouped.end());
    REQUIRE(std::find(grouped.begin(), grouped.end(), removed_component) == grouped.end());
}

TEST_CASE("delta snapshots restore tag additions and removals") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Active>("Active");

    const ecs::Entity kept = source.create();
    const ecs::Entity removed = source.create();
    REQUIRE(source.add<Position>(kept, Position{1, 0}) != nullptr);
    REQUIRE(source.add<Position>(removed, Position{2, 0}) != nullptr);
    REQUIRE(source.add<Active>(removed));
    source.clear_all_dirty<Active>();
    source.clear_all_dirty<Position>();

    auto baseline = source.snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.register_component<Active>("Active");
    replay.restore(baseline);

    REQUIRE(source.add<Active>(kept));
    REQUIRE(source.remove<Active>(removed));

    auto delta = source.delta_snapshot(baseline);
    replay.restore(delta);

    REQUIRE(replay.has<Active>(kept));
    REQUIRE_FALSE(replay.has<Active>(removed));
    REQUIRE(replay.is_dirty<Active>(kept));
    REQUIRE(replay.is_dirty<Active>(removed));

    std::vector<ecs::Entity> active_entities;
    replay.view<const Position>().with_tags<const Active>().each([&](ecs::Entity entity, const Position&) {
        active_entities.push_back(entity);
    });
    REQUIRE(active_entities == std::vector<ecs::Entity>{kept});
}

TEST_CASE("delta snapshots restore singleton dirty values") {
    ecs::Registry source;
    source.register_component<GameTime>("GameTime");
    source.clear_all_dirty<GameTime>();

    auto baseline = source.snapshot();
    ecs::Registry replay;
    replay.register_component<GameTime>("GameTime");
    replay.restore(baseline);

    source.write<GameTime>().tick = 55;
    auto delta = source.delta_snapshot(baseline);
    replay.restore(delta);

    REQUIRE(replay.get<GameTime>().tick == 55);
    REQUIRE(replay.is_dirty<GameTime>());
}

TEST_CASE("delta snapshots exclude system-tagged job bookkeeping entities") {
    ecs::Registry source;
    source.register_component<Position>("Position");

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 0}) != nullptr);

    const ecs::Entity job = source.job<Position>(0).each([](ecs::Entity, Position&) {});
    REQUIRE(source.add<Position>(job, Position{99, 0}) != nullptr);
    source.clear_all_dirty<Position>();

    auto baseline = source.snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.restore(baseline);

    source.write<Position>(entity).x = 5;
    source.write<Position>(job).x = 100;

    auto delta = source.delta_snapshot(baseline);
    replay.restore(delta);

    REQUIRE(replay.get<Position>(entity).x == 5);
    REQUIRE_FALSE(replay.alive(job));
    REQUIRE_FALSE(replay.contains<Position>(job));
}

TEST_CASE("delta restore validates baseline token component metadata and removal state") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Velocity>(entity, Velocity{1.0f, 1.0f}) != nullptr);
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();

    auto baseline = source.snapshot();
    source.write<Position>(entity).x = 2;
    REQUIRE(source.remove<Velocity>(entity));
    auto delta = source.delta_snapshot(baseline);

    ecs::Registry wrong_baseline;
    wrong_baseline.register_component<Position>("Position");
    wrong_baseline.register_component<Velocity>("Velocity");
    REQUIRE_THROWS_AS(wrong_baseline.restore(delta), std::logic_error);

    ecs::Registry missing_component;
    missing_component.register_component<Position>("Position");
    missing_component.restore(baseline);
    REQUIRE(missing_component.destroy(missing_component.component<Velocity>()));
    REQUIRE_THROWS_AS(missing_component.restore(delta), std::logic_error);

    ecs::Registry missing_removed_value;
    missing_removed_value.register_component<Position>("Position");
    missing_removed_value.register_component<Velocity>("Velocity");
    missing_removed_value.restore(baseline);
    REQUIRE(missing_removed_value.remove<Velocity>(entity));
    REQUIRE_THROWS_AS(missing_removed_value.restore(delta), std::logic_error);
}

TEST_CASE("delta snapshots reject dirty move-only non-trivial component storage") {
    ecs::Registry source;
    source.register_component<std::unique_ptr<int>>("OwnedInt");
    auto baseline = source.snapshot();

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<std::unique_ptr<int>>(entity, new int(1)) != nullptr);

    REQUIRE_THROWS_AS(source.delta_snapshot(baseline), std::logic_error);
}

TEST_CASE("field metadata supports simple debug printing") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");

    REQUIRE(registry.set_component_fields(
        position_component,
        {
            ecs::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ecs::PrimitiveType::I32), 1},
            ecs::ComponentField{"y", offsetof(Position, y), registry.primitive_type(ecs::PrimitiveType::I32), 1},
        }));

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{11, 12}) != nullptr);

    REQUIRE(registry.debug_print(entity, position_component) == "Position{x=11, y=12}");
}

TEST_CASE("field metadata accessors append replace and reject invalid components") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const ecs::Entity invalid{};

    REQUIRE(registry.component_fields(invalid) == nullptr);
    REQUIRE_FALSE(registry.set_component_fields(invalid, {}));
    REQUIRE_FALSE(registry.add_component_field(invalid, ecs::ComponentField{}));

    REQUIRE(registry.component_fields(position_component)->empty());

    REQUIRE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ecs::PrimitiveType::I32), 1}));
    REQUIRE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"y", offsetof(Position, y), registry.primitive_type(ecs::PrimitiveType::I32), 1}));

    const std::vector<ecs::ComponentField>* appended = registry.component_fields(position_component);
    REQUIRE(appended != nullptr);
    REQUIRE(appended->size() == 2);
    REQUIRE((*appended)[0].name == "x");
    REQUIRE((*appended)[1].name == "y");

    REQUIRE(registry.set_component_fields(
        position_component,
        {ecs::ComponentField{"only_y", offsetof(Position, y), registry.primitive_type(ecs::PrimitiveType::I32), 1}}));

    const std::vector<ecs::ComponentField>* replaced = registry.component_fields(position_component);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->size() == 1);
    REQUIRE((*replaced)[0].name == "only_y");

    REQUIRE(registry.destroy(position_component));
    REQUIRE(registry.component_fields(position_component) == nullptr);
    REQUIRE_FALSE(registry.set_component_fields(position_component, {}));
    REQUIRE_FALSE(registry.add_component_field(position_component, ecs::ComponentField{}));
}

TEST_CASE("debug printing supports primitive scalars and unprintable fields") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity scalars_component = registry.register_component<DebugScalars>("DebugScalars");

    REQUIRE(registry.set_component_fields(
        scalars_component,
        {
            ecs::ComponentField{
                "enabled",
                offsetof(DebugScalars, enabled),
                registry.primitive_type(ecs::PrimitiveType::Bool),
                1},
            ecs::ComponentField{"u32", offsetof(DebugScalars, u32), registry.primitive_type(ecs::PrimitiveType::U32), 1},
            ecs::ComponentField{"i64", offsetof(DebugScalars, i64), registry.primitive_type(ecs::PrimitiveType::I64), 1},
            ecs::ComponentField{"u64", offsetof(DebugScalars, u64), registry.primitive_type(ecs::PrimitiveType::U64), 1},
            ecs::ComponentField{"f32", offsetof(DebugScalars, f32), registry.primitive_type(ecs::PrimitiveType::F32), 1},
            ecs::ComponentField{"f64", offsetof(DebugScalars, f64), registry.primitive_type(ecs::PrimitiveType::F64), 1},
        }));

    const ecs::Entity entity = registry.create();
    REQUIRE(
        registry.add<DebugScalars>(
            entity,
            DebugScalars{true, 23U, -45, 67U, 1.5f, 2.25}) != nullptr);

    REQUIRE(
        registry.debug_print(entity, scalars_component) ==
        "DebugScalars{enabled=true, u32=23, i64=-45, u64=67, f32=1.5, f64=2.25}");

    const ecs::Entity missing = registry.create();
    REQUIRE(registry.debug_print(missing, scalars_component) == "<missing>");

    const ecs::Entity nested_component = registry.register_component<DebugNested>("DebugNested");
    REQUIRE(registry.set_component_fields(
        nested_component,
        {
            ecs::ComponentField{"position", offsetof(DebugNested, position), registry.component<Position>(), 1},
            ecs::ComponentField{"values", offsetof(DebugNested, values), registry.primitive_type(ecs::PrimitiveType::I32), 2},
        }));

    REQUIRE(registry.add<DebugNested>(entity, DebugNested{Position{1, 2}, {3, 4}}) != nullptr);
    REQUIRE(registry.debug_print(entity, nested_component) == "DebugNested{position=<unprintable>, values=<unprintable>}");
}

TEST_CASE("invalid and stale entity operations fail without throwing once component is registered") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity invalid{};
    REQUIRE_FALSE(registry.alive(invalid));
    REQUIRE(registry.add<Position>(invalid, Position{1, 1}) == nullptr);
    REQUIRE_FALSE(registry.contains<Position>(invalid));
    REQUIRE_FALSE(registry.contains<Position>(invalid));
    REQUIRE_FALSE(registry.clear_dirty<Position>(invalid));
    REQUIRE_FALSE(registry.remove<Position>(invalid));
    REQUIRE_FALSE(registry.destroy(invalid));

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.destroy(entity));
    REQUIRE(registry.add<Position>(entity, Position{1, 1}) == nullptr);
    REQUIRE_FALSE(registry.contains<Position>(entity));
    REQUIRE_FALSE(registry.contains<Position>(entity));
    REQUIRE_FALSE(registry.clear_dirty<Position>(entity));
    REQUIRE_FALSE(registry.remove<Position>(entity));
    REQUIRE_FALSE(registry.destroy(entity));
}
