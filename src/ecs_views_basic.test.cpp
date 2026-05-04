#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("runtime tag filters support readonly view access") {
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

TEST_CASE("runtime tag filters refresh cached storage after registry tag add") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity selected_tag = registry.register_tag("Selected");

    const ecs::Entity selected = registry.create();
    REQUIRE(registry.add<Position>(selected, Position{1, 0}) != nullptr);

    auto selected_view = registry.view<Position>().with_tags({selected_tag});
    REQUIRE(registry.add_tag(selected, selected_tag));

    int calls = 0;
    selected_view.each([&](ecs::Entity entity, Position&) {
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
    static_assert(!HasViewGet<AccessView, Position>::value, "iterated component is not readable by entity on access view");
    static_assert(!HasViewWrite<AccessView, Position>::value, "iterated component is not writable by entity on access view");
    static_assert(HasViewCurrentGet<AccessView, Position>::value, "iterated component stays readable on active entity");
    static_assert(HasViewCurrentWrite<AccessView, Position>::value, "mutable iterated component stays writable on active entity");
    static_assert(HasViewCurrentContains<AccessView, Position>::value, "iterated component can be checked on active entity");
    static_assert(HasViewCurrentTryGet<AccessView, Position>::value, "iterated component can be read optionally on active entity");
    static_assert(HasViewGet<AccessView, Velocity>::value, "access component can be read");
    static_assert(!HasViewWrite<AccessView, Velocity>::value, "const access component cannot be written");
    static_assert(HasViewWrite<AccessView, Health>::value, "mutable access component can be written");
    static_assert(!HasViewCurrentGet<AccessView, Health>::value, "access component is not active-entity access");
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
    static_assert(HasViewNestedView<View, std::tuple<const Position>>::value, "nested views can read listed readonly components");
    static_assert(!HasViewNestedView<View, std::tuple<Position>>::value, "nested views cannot write readonly components");
    static_assert(HasViewNestedView<View, std::tuple<const Velocity>>::value, "nested views can read listed mutable components");
    static_assert(HasViewNestedView<View, std::tuple<Velocity>>::value, "nested views can write listed mutable components");
    static_assert(!HasViewNestedView<View, std::tuple<Health>>::value, "nested views cannot access undeclared components");
    static_assert(
        HasViewNestedView<AccessView, std::tuple<Position, const Velocity, Health>>::value,
        "nested access views can use iterated and access components");
    static_assert(
        !HasViewNestedView<AccessView, std::tuple<Velocity>>::value,
        "nested access views cannot make readonly access components mutable");
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
