#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("field metadata supports simple debug printing") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = registry.register_component<Position>("Position");

    REQUIRE(registry.set_component_fields(
        position_component,
        {
            ashiato::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ashiato::PrimitiveType::I32), 1},
            ashiato::ComponentField{"y", offsetof(Position, y), registry.primitive_type(ashiato::PrimitiveType::I32), 1},
        }));

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{11, 12}) != nullptr);

    REQUIRE(registry.debug_print(entity, position_component) == "Position{x=11, y=12}");
}

TEST_CASE("field metadata accessors append replace and reject invalid components") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = registry.register_component<Position>("Position");
    const ashiato::Entity invalid{};

    REQUIRE(registry.component_fields(invalid) == nullptr);
    REQUIRE_FALSE(registry.set_component_fields(invalid, {}));
    REQUIRE_FALSE(registry.add_component_field(invalid, ashiato::ComponentField{}));

    REQUIRE(registry.component_fields(position_component)->empty());

    REQUIRE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ashiato::PrimitiveType::I32), 1}));
    REQUIRE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"y", offsetof(Position, y), registry.primitive_type(ashiato::PrimitiveType::I32), 1}));

    const std::vector<ashiato::ComponentField>* appended = registry.component_fields(position_component);
    REQUIRE(appended != nullptr);
    REQUIRE(appended->size() == 2);
    REQUIRE((*appended)[0].name == "x");
    REQUIRE((*appended)[1].name == "y");

    REQUIRE(registry.set_component_fields(
        position_component,
        {ashiato::ComponentField{"only_y", offsetof(Position, y), registry.primitive_type(ashiato::PrimitiveType::I32), 1}}));

    const std::vector<ashiato::ComponentField>* replaced = registry.component_fields(position_component);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->size() == 1);
    REQUIRE((*replaced)[0].name == "only_y");

    REQUIRE(registry.destroy(position_component));
    REQUIRE(registry.component_fields(position_component) == nullptr);
    REQUIRE_FALSE(registry.set_component_fields(position_component, {}));
    REQUIRE_FALSE(registry.add_component_field(position_component, ashiato::ComponentField{}));
}

TEST_CASE("field metadata rejects invalid ranges alignment counts and types") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = registry.register_component<Position>("Position");
    const ashiato::Entity i32 = registry.primitive_type(ashiato::PrimitiveType::I32);

    REQUIRE(registry.set_component_fields(
        position_component,
        {ashiato::ComponentField{"x", offsetof(Position, x), i32, 1}}));

    const std::vector<ashiato::ComponentField>* fields = registry.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");

    REQUIRE_FALSE(registry.set_component_fields(
        position_component,
        {ashiato::ComponentField{"past_end", sizeof(Position), i32, 1}}));
    REQUIRE(registry.component_fields(position_component)->size() == 1);
    REQUIRE((*registry.component_fields(position_component))[0].name == "x");

    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"misaligned", 1, i32, 1}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"too_many", offsetof(Position, y), i32, 2}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"zero_count", offsetof(Position, y), i32, 0}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{
            "overflow_count",
            offsetof(Position, y),
            i32,
            std::numeric_limits<std::size_t>::max()}));
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"invalid_type", offsetof(Position, y), ashiato::Entity{}, 1}));

    const ashiato::Entity active_tag = registry.register_tag("ActiveTag");
    REQUIRE_FALSE(registry.add_component_field(
        position_component,
        ashiato::ComponentField{"tag_type", offsetof(Position, y), active_tag, 1}));

    REQUIRE(registry.component_fields(position_component)->size() == 1);
    REQUIRE((*registry.component_fields(position_component))[0].name == "x");
}

TEST_CASE("component registration rejects invalid field metadata") {
    ashiato::Registry registry;

    ashiato::ComponentDesc desc;
    desc.name = "BadFields";
    desc.size = sizeof(Position);
    desc.alignment = alignof(Position);
    desc.fields = {ashiato::ComponentField{
        "past_end",
        sizeof(Position),
        registry.primitive_type(ashiato::PrimitiveType::I32),
        1}};

    REQUIRE_THROWS_AS(registry.register_component(std::move(desc)), std::invalid_argument);
}

TEST_CASE("debug printing supports primitive scalars and unprintable fields") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    const ashiato::Entity scalars_component = registry.register_component<DebugScalars>("DebugScalars");

    REQUIRE(registry.set_component_fields(
        scalars_component,
        {
            ashiato::ComponentField{
                "enabled",
                offsetof(DebugScalars, enabled),
                registry.primitive_type(ashiato::PrimitiveType::Bool),
                1},
            ashiato::ComponentField{"u32", offsetof(DebugScalars, u32), registry.primitive_type(ashiato::PrimitiveType::U32), 1},
            ashiato::ComponentField{"i64", offsetof(DebugScalars, i64), registry.primitive_type(ashiato::PrimitiveType::I64), 1},
            ashiato::ComponentField{"u64", offsetof(DebugScalars, u64), registry.primitive_type(ashiato::PrimitiveType::U64), 1},
            ashiato::ComponentField{"f32", offsetof(DebugScalars, f32), registry.primitive_type(ashiato::PrimitiveType::F32), 1},
            ashiato::ComponentField{"f64", offsetof(DebugScalars, f64), registry.primitive_type(ashiato::PrimitiveType::F64), 1},
        }));

    const ashiato::Entity entity = registry.create();
    REQUIRE(
        registry.add<DebugScalars>(
            entity,
            DebugScalars{true, 23U, -45, 67U, 1.5f, 2.25}) != nullptr);

    REQUIRE(
        registry.debug_print(entity, scalars_component) ==
        "DebugScalars{enabled=true, u32=23, i64=-45, u64=67, f32=1.5, f64=2.25}");

    const ashiato::Entity missing = registry.create();
    REQUIRE(registry.debug_print(missing, scalars_component) == "<missing>");

    const ashiato::Entity nested_component = registry.register_component<DebugNested>("DebugNested");
    REQUIRE(registry.set_component_fields(
        nested_component,
        {
            ashiato::ComponentField{"position", offsetof(DebugNested, position), registry.component<Position>(), 1},
            ashiato::ComponentField{"values", offsetof(DebugNested, values), registry.primitive_type(ashiato::PrimitiveType::I32), 2},
        }));

    REQUIRE(registry.add<DebugNested>(entity, DebugNested{Position{1, 2}, {3, 4}}) != nullptr);
    REQUIRE(registry.debug_print(entity, nested_component) == "DebugNested{position=<unprintable>, values=<unprintable>}");
}
