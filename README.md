# ECS

A small C++17 sparse-set ECS.

## Entity Handles

`ecs::Entity` is a 64-bit generational id:

- low 32 bits: entity index
- high 32 bits: entity version

Deleted entity indices are recycled through an implicit free list stored in the entity slot list. A free slot stores the next free index in the low bits and the bumped version in the high bits, which makes stale handles fail `Registry::alive`.

Components are also entities. Registering a component creates a normal entity and stores its size, alignment, lifecycle, dirty state, and optional field metadata in the registry's component-info table.

## Typed Components

Compile-time component APIs require explicit registration. Using `add<T>`, `get<T>`, `write<T>`, `remove<T>`, or dirty helpers before `register_component<T>()` throws `std::logic_error`.

```cpp
#include "ecs/ecs.hpp"

struct Position {
    int x;
    int y;
};

int main() {
    ecs::Registry registry;

    ecs::Entity position_type = registry.register_component<Position>("Position");
    ecs::Entity entity = registry.create();

    registry.add<Position>(entity, Position{1, 2});

    if (Position* position = registry.write<Position>(entity)) {
        position->x += 3;
    }

    registry.clear_dirty<Position>(entity);
}
```

Typed methods resolve `T` through a per-registry cached component id instead of hashing `std::type_index` on each call.

## Singleton Components

Typed components can opt into singleton storage by specializing `ecs::is_singleton_component<T>`. Singleton components are compile-time only, must be default-constructible, and are created when the component is registered.

```cpp
struct GameTime {
    int tick = 0;
};

namespace ecs {
template <>
struct is_singleton_component<GameTime> : std::true_type {};
}

registry.register_component<GameTime>("GameTime");
registry.write<GameTime>()->tick += 1;
```

No-entity `get<T>()`, `write<T>()`, `clear_dirty<T>()`, and `is_dirty<T>()` overloads are available only for singleton component types.

## Views

Typed views iterate entities that have every listed component. `const T` means read-only for that component, while `T` allows writes through the view.

```cpp
auto view = registry.view<const Position, Velocity>();

view.each([](ecs::Entity entity, const Position& position, Velocity& velocity) {
    velocity.dx += static_cast<float>(position.x);
});

const Position* position = view.get<Position>(entity);
Velocity* velocity = view.write<Velocity>(entity);
```

`view.get<T>()` and `view.write<T>()` are only available for components listed in the view. `view.write<T>()` is additionally available only when `T` was listed without `const`.

Singleton components listed in a view are passed to callbacks but do not filter iteration. A singleton-only view calls its callback once with an invalid `ecs::Entity`.

Mutable listed components are marked dirty before they are passed to a view callback. To read a component during iteration but mark it dirty only when user code explicitly writes it, list it as `const T` in the view and add mutable access for the same component:

```cpp
auto view = registry.view<const Velocity>().access<Velocity>();

view.each([](auto& active_view, ecs::Entity entity, const Velocity& velocity) {
    if (velocity.dx != 0.0f) {
        if (Velocity* writable = active_view.template write<Velocity>(entity)) {
            writable->dy += velocity.dx;
        }
    }
});
```

Views can also carry access-only components. Access-only components do not filter iteration and are not passed to callbacks, but they can be read or written on any entity through the view. If a callback accepts the active view as its first argument, `each()` passes it before the entity.

```cpp
auto view = registry.view<Position>().access<const Target, Health>();

view.each([&](auto& active_view, ecs::Entity entity, Position& position) {
    const Target* target = active_view.template get<Target>(entity);
    if (target != nullptr) {
        if (Health* health = active_view.template write<Health>(target->entity)) {
            health->value += position.x;
        }
    }
});
```

## Jobs

Jobs are persistent view callbacks with an integer order. Register jobs with `Registry::job<Components...>(order)`, then call `.each()` on the returned job view. Jobs run only when `run_jobs()` is called. Lower order values run first, and jobs with the same order run in the order they were added.

```cpp
registry.job<const Position, Velocity>(10).each(
    [](ecs::Entity entity, const Position& position, Velocity& velocity) {
        velocity.dx += position.x;
        velocity.dy += position.y;
    });

registry.run_jobs();
```

Job views support the same access pattern as ordinary views:

```cpp
registry.job<const Position>(20)
    .access<Velocity>()
    .each([](auto& active_view, ecs::Entity entity, const Position& position) {
        if (Velocity* velocity = active_view.template write<Velocity>(entity)) {
            velocity->dx += position.x;
        }
    });
```

## Owned Groups and Sorting

Fully owned groups are declared as layout hints. After declaration, matching views automatically use the owned group prefix as their iteration driver while the normal `view`, `get`, and `write` APIs stay unchanged.

```cpp
registry.declare_owned_group<Position, Velocity>();

registry.view<Position, Velocity>().each([](ecs::Entity entity, Position& position, Velocity& velocity) {
    velocity.dx += static_cast<float>(position.x);
});
```

View callbacks mark owned components dirty before passing mutable references, just like ordinary mutable views.

Multiple owned groups can share components only when the owned sets are identical or nested. For example, `declare_owned_group<Position>()` and `declare_owned_group<Position, Velocity>()` can coexist, but `declare_owned_group<Position, Velocity>()` and `declare_owned_group<Position, Health>()` conflict.

## Runtime Components

Runtime components use component entities directly. Runtime values are byte-copyable blobs unless a typed component registered lifecycle hooks for the same component.

```cpp
struct Velocity {
    float dx;
    float dy;
};

ecs::ComponentDesc velocity_desc;
velocity_desc.name = "Velocity";
velocity_desc.size = sizeof(Velocity);
velocity_desc.alignment = alignof(Velocity);

ecs::Entity velocity_type = registry.register_component(velocity_desc);

Velocity velocity{1.0f, 0.0f};
registry.add(entity, velocity_type, &velocity);

auto* writable = static_cast<Velocity*>(registry.write(entity, velocity_type));
```

## Metadata

Field metadata supports print debugging. Field types are entities, following the same component/type model.

```cpp
registry.set_component_fields(position_type, {
    {"x", offsetof(Position, x), registry.primitive_type(ecs::PrimitiveType::I32), 1},
    {"y", offsetof(Position, y), registry.primitive_type(ecs::PrimitiveType::I32), 1},
});

std::string text = registry.debug_print(entity, position_type);
// Position{x=1, y=2}
```

## API

- `ecs::Entity Registry::create()`
- `bool Registry::destroy(Entity entity)`
- `bool Registry::alive(Entity entity) const`
- `Entity Registry::register_component<T>(std::string name = {})`
- `Entity Registry::register_component(ComponentDesc desc)`
- `Entity Registry::component<T>() const`
- `const ComponentInfo* Registry::component_info(Entity component) const`
- `const std::vector<ComponentField>* Registry::component_fields(Entity component) const`
- `bool Registry::set_component_fields(Entity component, std::vector<ComponentField> fields)`
- `bool Registry::add_component_field(Entity component, ComponentField field)`
- `T* Registry::add<T>(Entity entity, Args&&... args)`
- `void* Registry::add(Entity entity, Entity component, const void* value = nullptr)`
- `void* Registry::ensure(Entity entity, Entity component)`
- `bool Registry::remove<T>(Entity entity)`
- `bool Registry::remove(Entity entity, Entity component)`
- `const T* Registry::get<T>(Entity entity) const`
- `const T* Registry::get<T>() const` (singleton components only)
- `const void* Registry::get(Entity entity, Entity component) const`
- `T* Registry::write<T>(Entity entity)`
- `T* Registry::write<T>()` (singleton components only)
- `void* Registry::write(Entity entity, Entity component)`
- `bool Registry::clear_dirty<T>(Entity entity)`
- `bool Registry::clear_dirty<T>()` (singleton components only)
- `bool Registry::clear_dirty(Entity entity, Entity component)`
- `bool Registry::is_dirty<T>(Entity entity) const`
- `bool Registry::is_dirty<T>() const` (singleton components only)
- `bool Registry::is_dirty(Entity entity, Entity component) const`
- `void Registry::clear_all_dirty<T>()`
- `void Registry::clear_all_dirty(Entity component)`
- `Registry::View<Components...> Registry::view<Components...>()`
- `Registry::JobView<Components...> Registry::job<Components...>(int order)`
- `void Registry::run_jobs()`
- `void Registry::declare_owned_group<Owned...>()`
- `void View<Components...>::each(Fn&& callback)`
- `auto View<Components...>::access<AccessComponents...>() const`
- `const T* View<Components...>::get<T>(Entity entity) const`
- `const T* View<Components...>::get<T>() const` (singleton components only)
- `T* View<Components...>::write<T>(Entity entity)`
- `T* View<Components...>::write<T>()` (singleton components only)
- `std::string Registry::debug_print(Entity entity, Entity component) const`

Invalid, dead, stale, or missing entity values are reported with `nullptr` or `false`. Unregistered component types or invalid component entities throw `std::logic_error`.

## Component Storage

Each component entity has one sparse set:

- sparse index: entity index to dense component slot
- dense entity index list
- dense dirty bit array
- dense component storage

Trivially copyable components are relocated with `memcpy`. Non-trivially copyable typed components use placement construction, move construction, and explicit destruction.

Component pointers remain valid until that component storage is removed or mutated in a way that reallocates or moves dense storage.

`add<T>()`, runtime `add()`, `ensure()`, and `write()` mark the component dirty because they return writable storage. `get()` is read-only and does not modify dirty state.

View callbacks mark non-const listed components dirty before passing mutable references. Structurally mutating viewed component storage during `each()` is unsupported.

Owned groups store matching entities in shared dense prefixes across their owned component pools. Creating a group may reorder owned component storage, and maintaining membership can reorder owned storage on later structural changes.

Destroying a component entity unregisters that component, clears its typed cache entry, and removes its storage from all entities.

## Build

```bash
./test.sh
```
