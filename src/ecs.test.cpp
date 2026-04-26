#include "ecs/ecs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
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
    REQUIRE(ecs::Registry::entity_index(first) == 7);
    REQUIRE(ecs::Registry::entity_index(second) == 8);
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

TEST_CASE("trivial components can be added, read, written, replaced, and removed") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity entity = registry.create();

    Position* added = registry.add<Position>(entity, Position{1, 2});
    REQUIRE(added != nullptr);
    REQUIRE(added->x == 1);
    REQUIRE(added->y == 2);
    REQUIRE(registry.clear_dirty<Position>(entity));

    Position* writable = registry.write<Position>(entity);
    REQUIRE(writable != nullptr);
    writable->x = 10;

    const Position* read = registry.get<Position>(entity);
    REQUIRE(read != nullptr);
    REQUIRE(read->x == 10);
    REQUIRE(read->y == 2);
    REQUIRE(registry.clear_dirty<Position>(entity));

    Position* replaced = registry.add<Position>(entity, Position{7, 8});
    REQUIRE(replaced == writable);
    REQUIRE(replaced->x == 7);
    REQUIRE(replaced->y == 8);

    REQUIRE(registry.remove<Position>(entity));
    REQUIRE(registry.get<Position>(entity) == nullptr);
    REQUIRE_FALSE(registry.clear_dirty<Position>(entity));
    REQUIRE_FALSE(registry.remove<Position>(entity));
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
    REQUIRE(ensured == registry.write<GameTime>());
    ensured->tick = 17;

    REQUIRE(registry.get<GameTime>()->tick == 17);
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

    REQUIRE(registry.write<Position>(second) != nullptr);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second)->x == 2);
    REQUIRE(registry.is_dirty<Position>(second));
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
    REQUIRE(registry.write<Position>(first) != nullptr);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second)->x == 2);
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

    const GameTime* initial = registry.get<GameTime>();
    REQUIRE(initial != nullptr);
    REQUIRE(initial->tick == 0);
    REQUIRE(registry.is_dirty<GameTime>());

    REQUIRE(registry.clear_dirty<GameTime>());
    REQUIRE_FALSE(registry.is_dirty<GameTime>());

    GameTime* writable = registry.write<GameTime>();
    REQUIRE(writable != nullptr);
    writable->tick = 42;

    REQUIRE(registry.get<GameTime>()->tick == 42);
    REQUIRE(registry.is_dirty<GameTime>());

    const ecs::Entity entity = registry.create();
    const GameTime replacement{99};
    REQUIRE(registry.add(entity, game_time_component, &replacement) != nullptr);
    REQUIRE(registry.get<GameTime>()->tick == 99);
    REQUIRE_FALSE(registry.remove(entity, game_time_component));
    REQUIRE(registry.get<GameTime>() != nullptr);
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
    REQUIRE(registry.get<Velocity>(both_a)->dy == 5.0f);
    REQUIRE(registry.get<Velocity>(both_b)->dy == 9.0f);
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
    REQUIRE(view.get<Position>(entity)->x == 8);

    Velocity* writable = view.write<Velocity>(entity);
    REQUIRE(writable != nullptr);
    writable->dx = 6.0f;
    REQUIRE(registry.get<Velocity>(entity)->dx == 6.0f);
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
    REQUIRE(registry.get<GameTime>()->tick == 5);
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
    REQUIRE(registry.get<GameTime>()->tick == 7);
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
        REQUIRE(active_view.template get<Velocity>(target)->dx == 4.0f);
        REQUIRE(active_view.template get<Health>(entity) == nullptr);
        REQUIRE(active_view.template get<GameTime>() != nullptr);

        Health* health = active_view.template write<Health>(target);
        REQUIRE(health != nullptr);
        health->value += position.x;
        active_view.template write<GameTime>()->tick += position.x;
        ++calls;
    });

    REQUIRE(calls == 2);
    REQUIRE(registry.get<Health>(target)->value == 15);
    REQUIRE(registry.get<GameTime>()->tick == 5);
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
        REQUIRE(active_view.template get<Position>(current)->y == 3);
        REQUIRE_FALSE(registry.is_dirty<Position>(current));
        ++read_calls;
    });

    REQUIRE(read_calls == 1);
    REQUIRE_FALSE(registry.is_dirty<Position>(entity));

    int write_calls = 0;
    view.each([&](auto& active_view, ecs::Entity current, const Position& position) {
        Position* writable = active_view.template write<Position>(current);
        REQUIRE(writable != nullptr);
        writable->x = position.x + 5;
        ++write_calls;
    });

    REQUIRE(write_calls == 1);
    REQUIRE(registry.get<Position>(entity)->x == 7);
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
        REQUIRE(active_view.template write<Health>(current) == nullptr);
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
    REQUIRE(registry.get<Position>(first) != nullptr);
    REQUIRE(registry.get<Position>(second) != nullptr);
    REQUIRE(registry.get<Velocity>(first) == nullptr);
    REQUIRE(registry.get<Velocity>(second) == nullptr);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Velocity>(later)->dx == 2.0f);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Position>(later) != nullptr);
    REQUIRE(registry.get<Velocity>(later) == nullptr);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Position>(later)->x == 2);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
        visited.push_back(entity);
        REQUIRE(registry.remove<Position>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ecs::Entity entity : visited) {
        REQUIRE(registry.get<Position>(entity) == nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
    }

    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ecs::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Position>(later) == nullptr);
    REQUIRE(registry.get<Velocity>(later) != nullptr);
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

TEST_CASE("jobs are persistent and use access views") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access<Velocity>().each(
        [&](auto& active_view, ecs::Entity current, const Position& position) {
            Velocity* velocity = active_view.template write<Velocity>(current);
            REQUIRE(velocity != nullptr);
            velocity->dx += static_cast<float>(position.x);
        });

    registry.run_jobs();
    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity)->dx == 5.0f);
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
    REQUIRE(registry.view<Position, Velocity>().get<Position>(both_a)->x == 2);
    REQUIRE(registry.view<Position, Velocity>().write<Velocity>(both_b)->dx == 30.0f);
    REQUIRE(registry.is_dirty<Position>(both_a));
    REQUIRE(registry.is_dirty<Velocity>(both_a));
    REQUIRE(registry.get<Velocity>(both_a)->dy == 2.0f);
    REQUIRE(registry.get<Velocity>(both_b)->dy == 3.0f);

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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Velocity>(later)->dx == 2.0f);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
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
    REQUIRE(registry.get<Position>(later) != nullptr);
    REQUIRE(registry.get<Velocity>(later) == nullptr);
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
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
        visited.push_back(entity);
        REQUIRE(registry.remove<Velocity>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ecs::Entity entity : visited) {
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) == nullptr);
    }

    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ecs::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
    }
}

TEST_CASE("nested owned groups tolerate later entities joining the most specific view") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    registry.declare_owned_group<Position>();
    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Position, Velocity, Health>();
    registry.declare_owned_group<Velocity, Position>();

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(first, Health{10}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
        REQUIRE(registry.get<Health>(entity) != nullptr);
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Health>(later, Health{20}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> full_group;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        full_group.push_back(entity);
    });
    REQUIRE(full_group.size() == 2);
    REQUIRE(std::find(full_group.begin(), full_group.end(), first) != full_group.end());
    REQUIRE(std::find(full_group.begin(), full_group.end(), later) != full_group.end());
    REQUIRE(registry.get<Health>(later)->value == 20);
}

TEST_CASE("nested owned groups tolerate later entities leaving the most specific view") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    registry.declare_owned_group<Position>();
    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Position, Velocity, Health>();
    registry.declare_owned_group<Velocity, Position>();

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity loses_health = registry.create();
    const ecs::Entity loses_velocity = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(first, Health{10}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(second, Health{20}) != nullptr);
    REQUIRE(registry.add<Position>(loses_health, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(loses_health, Velocity{3.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(loses_health, Health{30}) != nullptr);
    REQUIRE(registry.add<Position>(loses_velocity, Position{4, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(loses_velocity, Velocity{4.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(loses_velocity, Health{40}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
        REQUIRE(registry.get<Health>(entity) != nullptr);
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Health>(loses_health));
            REQUIRE(registry.remove<Velocity>(loses_velocity));
        }
    });

    REQUIRE(visited.size() <= 4);
    std::vector<ecs::Entity> positions;
    registry.view<Position>().each([&](ecs::Entity entity, Position&) {
        positions.push_back(entity);
    });
    REQUIRE(positions.size() == 4);
    REQUIRE(std::find(positions.begin(), positions.end(), first) != positions.end());
    REQUIRE(std::find(positions.begin(), positions.end(), second) != positions.end());
    REQUIRE(std::find(positions.begin(), positions.end(), loses_health) != positions.end());
    REQUIRE(std::find(positions.begin(), positions.end(), loses_velocity) != positions.end());

    std::vector<ecs::Entity> lower_group;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        lower_group.push_back(entity);
    });
    REQUIRE(lower_group.size() == 3);
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), first) != lower_group.end());
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), second) != lower_group.end());
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), loses_health) != lower_group.end());
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), loses_velocity) == lower_group.end());

    std::vector<ecs::Entity> full_group;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        full_group.push_back(entity);
    });
    REQUIRE(full_group.size() == 2);
    REQUIRE(std::find(full_group.begin(), full_group.end(), first) != full_group.end());
    REQUIRE(std::find(full_group.begin(), full_group.end(), second) != full_group.end());
    REQUIRE(std::find(full_group.begin(), full_group.end(), loses_health) == full_group.end());
    REQUIRE(std::find(full_group.begin(), full_group.end(), loses_velocity) == full_group.end());
}

TEST_CASE("nested owned groups keep lower group membership after current entity leaves full view") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    registry.declare_owned_group<Position>();
    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Position, Velocity, Health>();
    registry.declare_owned_group<Velocity, Position>();

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(first, Health{10}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(second, Health{20}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.get<Position>(entity) != nullptr);
        REQUIRE(registry.get<Velocity>(entity) != nullptr);
        REQUIRE(registry.get<Health>(entity) != nullptr);
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Health>(entity));
        }
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);

    std::vector<ecs::Entity> lower_group;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        lower_group.push_back(entity);
    });
    REQUIRE(lower_group.size() == 2);
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), first) != lower_group.end());
    REQUIRE(std::find(lower_group.begin(), lower_group.end(), second) != lower_group.end());

    std::vector<ecs::Entity> full_group;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity entity, Position&, Velocity&, Health&) {
        full_group.push_back(entity);
    });
    REQUIRE(full_group == std::vector<ecs::Entity>{second});
}

TEST_CASE("owned group declarations allow identical and nested groups but reject partial overlap") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{10}) != nullptr);

    registry.declare_owned_group<Position>();
    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Velocity, Position, Health>();
    registry.declare_owned_group<Velocity, Position>();

    int calls = 0;
    registry.view<Position, Velocity, Health>().each([&](ecs::Entity, Position&, Velocity&, Health&) {
        ++calls;
    });
    REQUIRE(calls == 1);

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
    REQUIRE(registry.get<Tracker>(first)->value == 1);
    REQUIRE(registry.get<Tracker>(second)->value == 2);

    REQUIRE(registry.remove<Tracker>(first));
    REQUIRE(registry.get<Tracker>(first) == nullptr);
    REQUIRE(registry.get<Tracker>(second)->value == 2);

    REQUIRE(registry.destroy(second));
    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial component replacement destroys the old value") {
    ecs::Registry registry;
    registry.register_component<Tracker>("Tracker");
    TrackerCounts counts;

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Tracker>(entity, counts, 1) != nullptr);
    REQUIRE(registry.get<Tracker>(entity)->value == 1);

    Tracker* replacement = registry.add<Tracker>(entity, counts, 2);
    REQUIRE(replacement != nullptr);
    REQUIRE(replacement == registry.get<Tracker>(entity));
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
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)])->value == i);
        }

        REQUIRE(registry.remove<Tracker>(entities[4]));
        REQUIRE(registry.get<Tracker>(entities[4]) == nullptr);
        for (int i = 0; i < 10; ++i) {
            if (i == 4) {
                continue;
            }
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)])->value == i);
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
    REQUIRE(registry.get<Position>(entity) == nullptr);
    REQUIRE(registry.write<std::unique_ptr<int>>(entity) == nullptr);
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
    REQUIRE(source.write<Position>(entity) != nullptr);
    REQUIRE(source.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), source.primitive_type(ecs::PrimitiveType::I32), 1}}));
    source.write<GameTime>()->tick = 12;

    ecs::Registry moved(std::move(source));

    REQUIRE(moved.alive(entity));
    REQUIRE(moved.component<Position>() == position_component);
    REQUIRE(moved.component<GameTime>() == game_time_component);
    REQUIRE(moved.get<Position>(entity)->x == 5);
    REQUIRE(moved.get<Position>(entity)->y == 6);
    REQUIRE(moved.is_dirty<Position>(entity));
    REQUIRE(moved.get<GameTime>()->tick == 12);
    REQUIRE(moved.is_dirty<GameTime>());

    const std::vector<ecs::ComponentField>* fields = moved.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");
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
    REQUIRE(registry.get<Position>(invalid) == nullptr);
    REQUIRE(registry.write<Position>(invalid) == nullptr);
    REQUIRE_FALSE(registry.clear_dirty<Position>(invalid));
    REQUIRE_FALSE(registry.remove<Position>(invalid));
    REQUIRE_FALSE(registry.destroy(invalid));

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.destroy(entity));
    REQUIRE(registry.add<Position>(entity, Position{1, 1}) == nullptr);
    REQUIRE(registry.get<Position>(entity) == nullptr);
    REQUIRE(registry.write<Position>(entity) == nullptr);
    REQUIRE_FALSE(registry.clear_dirty<Position>(entity));
    REQUIRE_FALSE(registry.remove<Position>(entity));
    REQUIRE_FALSE(registry.destroy(entity));
}
