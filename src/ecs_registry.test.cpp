#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("entities are created, destroyed, and recycled with versions") {
    ecs::Registry registry;

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.alive(first));
    REQUIRE(registry.alive(second));
    REQUIRE(first != second);
    REQUIRE(registry.entity_kind(first) == ecs::EntityKind::User);
    REQUIRE(registry.entity_kind(second) == ecs::EntityKind::User);
    REQUIRE(registry.is_user_entity(first));
    REQUIRE(registry.is_user_entity(second));
    REQUIRE(ecs::Registry::entity_version(first) == 1);

    REQUIRE(registry.destroy(first));
    REQUIRE_FALSE(registry.alive(first));
    REQUIRE(registry.entity_kind(first) == ecs::EntityKind::Invalid);
    REQUIRE_FALSE(registry.is_user_entity(first));

    const ecs::Entity recycled = registry.create();
    REQUIRE(registry.alive(recycled));
    REQUIRE(registry.entity_kind(recycled) == ecs::EntityKind::User);
    REQUIRE(ecs::Registry::entity_index(recycled) == ecs::Registry::entity_index(first));
    REQUIRE(ecs::Registry::entity_version(recycled) == ecs::Registry::entity_version(first) + 1);
    REQUIRE_FALSE(registry.alive(first));
}

TEST_CASE("entity kind classifies users components jobs and system bookkeeping") {
    ecs::Registry registry;

    const ecs::Entity invalid{};
    const ecs::Entity system_tag = registry.system_tag();
    const ecs::Entity primitive = registry.primitive_type(ecs::PrimitiveType::I32);
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const ecs::Entity runtime_tag = registry.register_tag("Selected");
    const ecs::Entity user = registry.create();
    const ecs::Entity job = registry.job<const Position>(0).each([](ecs::Entity, const Position&) {});

    REQUIRE(registry.entity_kind(invalid) == ecs::EntityKind::Invalid);
    REQUIRE(registry.entity_kind(system_tag) == ecs::EntityKind::Component);
    REQUIRE(registry.entity_kind(primitive) == ecs::EntityKind::Component);
    REQUIRE(registry.entity_kind(position_component) == ecs::EntityKind::Component);
    REQUIRE(registry.entity_kind(runtime_tag) == ecs::EntityKind::Component);
    REQUIRE(registry.entity_kind(user) == ecs::EntityKind::User);
    REQUIRE(registry.entity_kind(job) == ecs::EntityKind::Job);

    REQUIRE_FALSE(registry.is_user_entity(invalid));
    REQUIRE_FALSE(registry.is_user_entity(system_tag));
    REQUIRE_FALSE(registry.is_user_entity(primitive));
    REQUIRE_FALSE(registry.is_user_entity(position_component));
    REQUIRE_FALSE(registry.is_user_entity(runtime_tag));
    REQUIRE(registry.is_user_entity(user));
    REQUIRE_FALSE(registry.is_user_entity(job));

    REQUIRE(registry.destroy(user));
    REQUIRE(registry.entity_kind(user) == ecs::EntityKind::Invalid);
    REQUIRE_FALSE(registry.is_user_entity(user));
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
