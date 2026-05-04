#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("field metadata rejects invalid ranges alignment counts and types") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const ecs::Entity i32 = registry.primitive_type(ecs::PrimitiveType::I32);

    REQUIRE(registry.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), i32, 1}}));

    const std::vector<ecs::ComponentField>* fields = registry.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");

    REQUIRE_FALSE(registry.set_component_fields(
        position_component,
        {ecs::ComponentField{"past_end", sizeof(Position), i32, 1}}));
    REQUIRE(registry.component_fields(position_component)->size() == 1);
    REQUIRE((*registry.component_fields(position_component))[0].name == "x");

    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"misaligned", 1, i32, 1}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"too_many", offsetof(Position, y), i32, 2}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"zero_count", offsetof(Position, y), i32, 0}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{
            "overflow_count",
            offsetof(Position, y),
            i32,
            std::numeric_limits<std::size_t>::max()}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"invalid_type", offsetof(Position, y), ecs::Entity{}, 1}));

    const ecs::Entity active_tag = registry.register_tag("ActiveTag");
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ecs::ComponentField{"tag_type", offsetof(Position, y), active_tag, 1}));

    REQUIRE(registry.component_fields(position_component)->size() == 1);
    REQUIRE((*registry.component_fields(position_component))[0].name == "x");
}

TEST_CASE("component registration rejects invalid field metadata") {
    ecs::Registry registry;

    ecs::ComponentDesc desc;
    desc.name = "BadFields";
    desc.size = sizeof(Position);
    desc.alignment = alignof(Position);
    desc.fields = {ecs::ComponentField{
        "past_end",
        sizeof(Position),
        registry.primitive_type(ecs::PrimitiveType::I32),
        1}};

    REQUIRE_THROWS_AS(registry.register_component(std::move(desc)), std::invalid_argument);
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
