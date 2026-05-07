# ECS

[![Tests](https://img.shields.io/endpoint?url=https%3A%2F%2Ferikgoldman.github.io%2Fkagesoto%2Fci.json)](https://github.com/ErikGoldman/kagesoto/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Ferikgoldman.github.io%2Fkagesoto%2Fcoverage.json)](https://github.com/ErikGoldman/kagesoto/actions/workflows/ci.yml)

A small C++17 sparse-set ECS.

## Entity Handles

`ecs::Entity` is a 64-bit generational id:

- low 32 bits: entity index
- high 32 bits: entity version

Deleted entity indices are recycled through an implicit free list stored in the entity slot list. A free slot stores the next free index in the low bits and the bumped version in the high bits, which makes stale handles fail `Registry::alive`.

Entity indices are opaque. Components are also entities: registering a component creates a normal entity and stores its size, alignment, lifecycle, dirty state, and optional field metadata in the registry's component-info table. Jobs and internal bookkeeping also use entity handles. Use `Registry::entity_kind()` or `Registry::is_user_entity()` when code needs to distinguish user-created entities from component descriptors, jobs, or system bookkeeping.

## Typed Components

Compile-time component APIs require explicit registration. Using `add<T>`, `contains<T>`, `try_get<T>`, `get<T>`, `write<T>`, `remove<T>`, or dirty helpers before `register_component<T>()` throws `std::logic_error`.

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

    if (registry.contains<Position>(entity)) {
        registry.write<Position>(entity).x += 3;
    }

    registry.clear_dirty<Position>(entity);
}
```

Typed methods resolve `T` through a per-registry cached component id instead of hashing `std::type_index` on each call.

## Tags

Empty non-singleton typed components are tags. A tag has no stored value, so it cannot be read or written, but it can be added to or removed from entities and used as a view filter.

```cpp
struct Active {};
struct Disabled {};

registry.register_component<Active>("Active");
registry.register_component<Disabled>("Disabled");

registry.add<Active>(entity);

registry.view<Position>()
    .with_tags<const Active>()
    .without_tags<const Disabled>()
    .each([](ecs::Entity entity, Position& position) {
        position.x += 1;
    });
```

`with_tags<T>()` requires the tag and `without_tags<T>()` excludes it. Constness controls whether the view can mutate that tag: `const Active` is filter-only, while `Active` enables `view.add_tag<Active>(entity)` and `view.remove_tag<Active>(entity)`.

Runtime tags are registered separately:

```cpp
ecs::Entity selected = registry.register_tag("Selected");
registry.add_tag(entity, selected);

auto view = registry.view<Position>().with_tags({selected});
view.each([&](auto& active_view, ecs::Entity entity, Position&) {
    if (active_view.has_tag(entity, selected)) {
        // Runtime tag filters are read-only view filters.
    }
});
```

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
registry.write<GameTime>().tick += 1;
```

No-entity `get<T>()`, `write<T>()`, `clear_dirty<T>()`, and `is_dirty<T>()` overloads are available only for singleton component types.

## Views

Typed views iterate entities that have every listed component. `const T` means read-only for that component, while `T` allows writes through the view.

```cpp
auto view = registry.view<const Position, Velocity>();

view.each([](ecs::Entity entity, const Position& position, Velocity& velocity) {
    velocity.dx += static_cast<float>(position.x);
});

const Position& position = view.get<Position>(entity);
Velocity& velocity = view.write<Velocity>(entity);
```

`view.get<T>()` and `view.write<T>()` are required-access APIs and are only available for components listed in the view. Use `contains<T>(entity)` before optional access. `view.write<T>()` is additionally available only when `T` was listed without `const`.

Singleton components listed in a view are passed to callbacks but do not filter iteration. A singleton-only view calls its callback once with an invalid `ecs::Entity`.

Mutable listed components are marked dirty before they are passed to a view callback. To read a component during iteration but mark it dirty only when user code explicitly writes it, list it as `const T` in the view and add mutable access for the same component:

```cpp
auto view = registry.view<const Velocity>().access<Velocity>();

view.each([](auto& active_view, ecs::Entity entity, const Velocity& velocity) {
    if (velocity.dx != 0.0f) {
        if (active_view.template contains<Velocity>(entity)) {
            active_view.template write<Velocity>(entity).dy += velocity.dx;
        }
    }
});
```

Views can also carry access-only components. Access-only components do not filter iteration and are not passed to callbacks, but they can be read or written on any entity through the view. If a callback accepts the active view as its first argument, `each()` passes it before the entity.

```cpp
auto view = registry.view<Position>().access<const Target, Health>();

view.each([&](auto& active_view, ecs::Entity entity, Position& position) {
    const Target* target = active_view.template try_get<Target>(entity);
    if (target != nullptr) {
        if (active_view.template contains<Health>(target->entity)) {
            active_view.template write<Health>(target->entity).value += position.x;
        }
    }
});
```

## Jobs

Jobs are persistent view callbacks with an integer order. Register jobs with `Registry::job<Components...>(order)`, then call `.each()` on the returned job view. `each()` returns an entity that identifies the registered job. Job entities are identity-only for now: they are useful in schedule output, but destroying them is not a supported job-removal API. Jobs run only when `run_jobs()` is called. Lower order values run first, and jobs with the same order run in the order they were added.

```cpp
ecs::Entity move_job = registry.job<const Position, Velocity>(10).each(
    [](ecs::Entity entity, const Position& position, Velocity& velocity) {
        velocity.dx += position.x;
        velocity.dy += position.y;
    });

registry.run_jobs();
```

Job views can declare optional access for components on the entity currently being iterated. Optional
components do not filter iteration:

```cpp
registry.job<const Position>(20)
    .optional<Velocity>()
    .each([](auto& active_view, ecs::Entity entity, const Position& position) {
        if (active_view.template contains<Velocity>(entity)) {
            active_view.template write<Velocity>(entity).dx += position.x;
        }
    });
```

Use `.access_other_entities<T...>()` only when the job needs to read or write those components on entities
other than the one currently being iterated. Jobs with other-entity access are always single-threaded:

```cpp
registry.job<const Target>(30)
    .access_other_entities<Health>()
    .each([](auto& active_view, ecs::Entity, const Target& target) {
        if (active_view.template contains<Health>(target.entity)) {
            --active_view.template write<Health>(target.entity).value;
        }
    });
```

Jobs are single-threaded by default. Use `.max_threads(count)` to let a job split its matching entities into chunks, `.min_entities_per_thread(count)` to control chunk size, and `.single_thread()` to force one chunk:

```cpp
registry.job<Position>(0)
    .max_threads(4)
    .min_entities_per_thread(256)
    .each([](ecs::Entity, Position& position) {
        position.x += 1;
    });
```

`run_jobs()` uses the orchestrator to run independent jobs in the same stage. Install an executor to hand stage tasks to a task system. The executor must run every task and return only after all tasks finish. If no executor is installed, tasks run inline.

```cpp
registry.set_job_thread_executor([](const std::vector<ecs::JobThreadTask>& tasks) {
    for (const ecs::JobThreadTask& task : tasks) {
        task.run();
    }
});

registry.run_jobs();
registry.run_jobs(ecs::RunJobsOptions{true}); // force serial execution
```

Structural changes from a job must be declared explicitly. Declaring structural access keeps that job single-threaded and isolates it in the orchestrator schedule because add/remove operations mutate registry bookkeeping.

```cpp
registry.job<const Position>(0)
    .structural<Disabled>()
    .each([](auto& job, ecs::Entity entity, const Position&) {
        job.template add<Disabled>(entity);
    });
```

`Orchestrator` can inspect registered jobs and return the order in which they can be processed. `run_jobs()` uses this schedule unless forced into single-threaded mode. Each stage contains jobs that can run in parallel, and stages must be processed in order.

```cpp
ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();
for (const ecs::JobScheduleStage& stage : schedule.stages) {
    for (ecs::Entity job : stage.jobs) {
        // job identifies a registered job in this parallel stage.
    }
}
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

Multiple owned groups can share components only when the owned sets are identical. For example, declaring both `declare_owned_group<Position, Velocity>()` and `declare_owned_group<Velocity, Position>()` is valid, but `declare_owned_group<Position>()`, `declare_owned_group<Position, Velocity>()`, and `declare_owned_group<Position, Health>()` cannot coexist with one another because they all try to own `Position`.

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
- `EntityKind Registry::entity_kind(Entity entity) const`
- `bool Registry::is_user_entity(Entity entity) const`
- `Entity Registry::register_component<T>(std::string name = {})`
- `Entity Registry::register_component(ComponentDesc desc)`
- `Entity Registry::register_tag(std::string name = {})`
- `Entity Registry::component<T>() const`
- `const ComponentInfo* Registry::component_info(Entity component) const`
- `const std::vector<ComponentField>* Registry::component_fields(Entity component) const`
- `bool Registry::set_component_fields(Entity component, std::vector<ComponentField> fields)`
- `bool Registry::add_component_field(Entity component, ComponentField field)`
- `T* Registry::add<T>(Entity entity, Args&&... args)`
- `bool Registry::add<T>(Entity entity)` (tag components only)
- `void* Registry::add(Entity entity, Entity component, const void* value = nullptr)`
- `bool Registry::add_tag(Entity entity, Entity tag)`
- `void* Registry::ensure(Entity entity, Entity component)`
- `bool Registry::remove<T>(Entity entity)`
- `bool Registry::remove(Entity entity, Entity component)`
- `bool Registry::remove_tag(Entity entity, Entity tag)`
- `bool Registry::has<T>(Entity entity) const` (tag components only)
- `bool Registry::has(Entity entity, Entity tag) const`
- `bool Registry::contains<T>(Entity entity) const` (non-singleton components only)
- `const T* Registry::try_get<T>(Entity entity) const` (non-singleton components only)
- `const T& Registry::get<T>(Entity entity) const`
- `const T& Registry::get<T>() const` (singleton components only)
- `const void* Registry::get(Entity entity, Entity component) const`
- `T& Registry::write<T>(Entity entity)`
- `T& Registry::write<T>()` (singleton components only)
- `void* Registry::write(Entity entity, Entity component)`
- `bool Registry::clear_dirty<T>(Entity entity)`
- `bool Registry::clear_dirty<T>()` (singleton components only)
- `bool Registry::clear_dirty(Entity entity, Entity component)`
- `bool Registry::is_dirty<T>(Entity entity) const`
- `bool Registry::is_dirty<T>() const` (singleton components only)
- `bool Registry::is_dirty(Entity entity, Entity component) const`
- `void Registry::clear_all_dirty<T>()`
- `void Registry::clear_all_dirty(Entity component)`
- `Entity Registry::system_tag() const`
- `Registry::Snapshot Registry::create_snapshot() const`
- `Registry::DeltaSnapshot Registry::create_delta_snapshot(const Registry::Snapshot& baseline) const`
- `Registry::DeltaSnapshot Registry::create_delta_snapshot(const Registry::DeltaSnapshot& baseline) const`
- `void Registry::restore_snapshot(const Registry::Snapshot& snapshot)`
- `void Registry::restore_delta_snapshot(const Registry::DeltaSnapshot& snapshot)`
- `void Registry::Snapshot::write_native(std::ostream& out, const SnapshotIoOptions& options = {}) const`
- `Registry::Snapshot Registry::Snapshot::read_native(std::istream& in)`
- `void Registry::DeltaSnapshot::write_native(std::ostream& out, const SnapshotIoOptions& options = {}) const`
- `Registry::DeltaSnapshot Registry::DeltaSnapshot::read_native(std::istream& in)`
- `ecs::ComponentSerializationRegistry::register_component<T, Traits>(Registry& registry, std::string name = {})`
- `void ecs::write_persistent_snapshot(std::ostream& out, const Registry::Snapshot& snapshot, const ComponentSerializationRegistry& serialization, const SnapshotIoOptions& options = {})`
- `Registry::Snapshot ecs::read_persistent_snapshot(std::istream& in, const Registry& schema, const ComponentSerializationRegistry& serialization)`
- `void ecs::write_persistent_delta_snapshot(std::ostream& out, const Registry::DeltaSnapshot& snapshot, const Registry::Snapshot& baseline, const ComponentSerializationRegistry& serialization, const SnapshotIoOptions& options = {})`
- `Registry::DeltaSnapshot ecs::read_persistent_delta_snapshot(std::istream& in, const Registry& schema, const Registry::Snapshot& baseline, const ComponentSerializationRegistry& serialization)`
- `Registry::View<Components...> Registry::view<Components...>()`
- `Registry::JobView<Components...> Registry::job<Components...>(int order)`
- `void Registry::set_job_thread_executor(JobThreadExecutor executor)`
- `void Registry::run_jobs(RunJobsOptions options = {})`
- `Registry::JobView<Components...>& Registry::JobView<Components...>::max_threads(std::size_t count)`
- `Registry::JobView<Components...>& Registry::JobView<Components...>::single_thread()`
- `Registry::JobView<Components...>& Registry::JobView<Components...>::min_entities_per_thread(std::size_t count)`
- `Registry::JobStructuralView<...> Registry::JobView<Components...>::structural<StructuralComponents...>()`
- `Registry::JobAccessView<...>& Registry::JobAccessView<...>::max_threads(std::size_t count)`
- `Registry::JobStructuralAccessView<...> Registry::JobAccessView<...>::structural<StructuralComponents...>()`
- `ecs::Orchestrator::Orchestrator(const Registry& registry)`
- `ecs::JobSchedule ecs::Orchestrator::schedule() const`
- `std::vector<ecs::JobScheduleStage> ecs::JobSchedule::stages`
- `std::vector<ecs::Entity> ecs::JobScheduleStage::jobs`
- `void Registry::declare_owned_group<Owned...>()`
- `void View<Components...>::each(Fn&& callback)`
- `auto View<Components...>::access<AccessComponents...>() const`
- `auto View<Components...>::with_tags<Tags...>() const`
- `auto View<Components...>::without_tags<Tags...>() const`
- `auto View<Components...>::with_tags(std::initializer_list<Entity> tags) const`
- `auto View<Components...>::without_tags(std::initializer_list<Entity> tags) const`
- `bool filtered_view.has_tag<T>(Entity entity) const`
- `bool filtered_view.add_tag<T>(Entity entity)` (non-const tag filters only)
- `bool filtered_view.remove_tag<T>(Entity entity)` (non-const tag filters only)
- `bool filtered_view.has_tag(Entity entity, Entity tag) const`
- `bool View<Components...>::contains<T>(Entity entity) const` (non-singleton components only)
- `const T* View<Components...>::try_get<T>(Entity entity) const` (non-singleton components only)
- `const T& View<Components...>::get<T>(Entity entity) const`
- `const T& View<Components...>::get<T>() const` (singleton components only)
- `T& View<Components...>::write<T>(Entity entity)`
- `T& View<Components...>::write<T>()` (singleton components only)
- `std::string Registry::debug_print(Entity entity, Entity component) const`

Optional access uses `contains<T>(entity)`, `try_get<T>(entity)`, or runtime pointer APIs. Typed `get<T>()` and `write<T>()` are required-access APIs; using them for missing components or invalid entities is programmer error. Unregistered component types or invalid component entities throw `std::logic_error`.

## Component Storage

Each component entity has one sparse set:

- sparse index: entity index to dense component slot
- dense entity index list
- dense dirty bit array
- dense component storage, except tags which store presence only

Trivially copyable components are relocated with `memcpy`. Non-trivially copyable typed components use placement construction, move construction, and explicit destruction.

Component pointers remain valid until that component storage is removed or mutated in a way that reallocates or moves dense storage.

`add<T>()`, runtime `add()`, `ensure()`, and `write()` mark the component dirty because they return writable storage. Tag add/remove marks tag membership dirty. `get()`, `try_get()`, `contains()`, and `has()` are read-only and do not modify dirty state.

In-memory snapshots are created with `Registry::create_snapshot()` / `create_delta_snapshot()` and applied with `restore_snapshot()` / `restore_delta_snapshot()`. Delta snapshots validate against a baseline snapshot token, copy current entity slot state, and save only dirty component values plus dirty component-removal tombstones. In-memory snapshots are optimized for creation and restore speed: trivially copyable component storage is byte-copied, copy-constructible typed components are cloned through their lifecycle hook, and move-only non-trivial component storage rejects snapshot creation when captured.

Reading and writing snapshots means serializing an existing snapshot object to or from a stream/bit buffer. `Snapshot::write_native()` / `read_native()` and `DeltaSnapshot::write_native()` / `read_native()` serialize the native in-memory snapshot binary format; `write()` / `read()` remain compatibility wrappers. That native format is useful for local transient I/O but is not a durable or portable disk format. On full native snapshot restore, typed C++ component bindings are rebuilt by matching the current process's registered component type names to restored component names; serialized typed cache slots are not trusted as stable type identities.

Persistent snapshots are separate disk frames written with `write_persistent_snapshot()` and `write_persistent_delta_snapshot()`. Persistent frames identify components by unique non-empty component names, dedupe those names in a per-frame name table, store the frame length in bits before the frame body for skipping, and use `ComponentSerializationTraits<T>`/`ComponentSerializationRegistry` to quantize and serialize present component values through `ecs::BitBuffer`. Component removals and destroyed-entity tombstones are encoded by the snapshot system, not by component serialization. Persistent reads require a schema registry with matching component names and registered serialization, then return normal snapshot objects for `Registry::restore_snapshot()` or `Registry::restore_delta_snapshot()`.

Registered job callbacks are not part of snapshots. Internal bookkeeping entities are tagged with `Registry::system_tag()` and excluded from snapshot payloads.

View callbacks mark non-const listed components dirty before passing mutable references. Structurally mutating viewed component storage during `each()` is unsupported.

Owned groups store matching entities in shared dense prefixes across their owned component pools. Creating a group may reorder owned component storage, and maintaining membership can reorder owned storage on later structural changes.

Destroying a component entity unregisters that component, clears its typed cache entry, and removes its storage from all entities.

## Build

```bash
cmake -S . -B build -DECS_BUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Generate coverage stats with GCC or Clang:

```bash
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DECS_BUILD_EXAMPLE=OFF \
  -DECS_BUILD_TESTING=ON \
  -DECS_ENABLE_COVERAGE=ON
cmake --build build-coverage --target coverage
```

The coverage target runs the test suite, writes generated `.gcov` files under
`build-coverage/coverage`, and prints line, branch, and function coverage for
project sources.

Build benchmarks explicitly when collecting performance data:

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DECS_BUILD_TESTING=ON \
  -DECS_BUILD_BENCHMARKS=ON
cmake --build build-bench --target basic_operations_benchmark
```
