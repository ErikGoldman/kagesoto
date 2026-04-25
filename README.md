# ECS

Kagesoto is a C++17 ECS based on sparse lists with a few unusual features:
* First-class support for singletons
* Can rewind the system to previous states (for e.g. debugging)
* Direct transactional writes with configurable trace history
* Indexed fields for selective queries
* Group support to optimize multi-component views

## Quick Start

Include the aggregate header, define component structs, create entities, and write through a transaction.

```cpp
#include "ecs/ecs.hpp"

struct Position {
    float x;
    float y;
};

struct Velocity {
    float dx;
    float dy;
};

int main() {
    ecs::Registry registry;
    const ecs::Entity player = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(player, Position{10.0f, 20.0f});
        tx.write<Velocity>(player, Velocity{1.0f, 0.0f});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    if (const Velocity* velocity = tx.try_get<Velocity>(player)) {
        Position* position = tx.write<Position>(player);
        position->x += velocity->dx;
        position->y += velocity->dy;
    }
    tx.commit();
}
```

Transactions declare the component types they can access. A non-const declaration can write that component; a const declaration is read-only.

```cpp
auto read_only = registry.transaction<const Position>();
const Position& position = read_only.get<Position>(player);

auto writer = registry.transaction<Position>();
writer.write<Position>(player, Position{5.0f, 6.0f});
writer.commit();
```

Iterate a single component storage directly:

```cpp
auto tx = registry.transaction<Position>();

tx.storage<Position>().each([](ecs::Entity entity, const Position& position) {
    // entity has Position in this transaction's visible state.
});
```

Iterate entities that have multiple components:

```cpp
auto tx = registry.transaction<Position, Velocity>();

tx.view<const Position, const Velocity>().forEach(
    [](ecs::Entity entity, const Position& position, const Velocity& velocity) {
        // entity has both Position and Velocity.
    });
```

Views pick an execution plan from the available sources: component storage, owning groups, and usable component indexes. Use `explain()` or `explain_text()` to inspect the chosen plan.

## Core API

- `ecs::Registry` owns entities, component storage, snapshots, transactions, indexes, trace history, and groups.
- `ecs::Entity` is a 64-bit handle. Use `registry.alive(entity)` when accepting handles from outside the current flow.
- `registry.transaction<T...>()` opens a typed transaction. The component list is the transaction's access declaration.
- `tx.write<T>(entity, value)` creates or updates a component in the transaction-visible state.
- `tx.commit()` closes the transaction. Component writes are direct and remain visible even if a transaction is destroyed without an explicit commit.
- `registry.snapshot()` opens a read-only view over committed component state.
- `registry.group<A, B>()` creates an owning group for entities that have all grouped components.

Components must be trivially copyable. Singleton components are also supported by specializing `ecs::ComponentSingletonTraits<T>`; they exist once per registry and are accessed without an entity.

```cpp
struct GameClock {
    std::uint64_t tick = 0;
};

namespace ecs {

template <>
struct ComponentSingletonTraits<GameClock> {
    static constexpr bool value = true;
};

}  // namespace ecs

auto tx = registry.transaction<GameClock>();
GameClock* clock = tx.write<GameClock>();
clock->tick += 1;
tx.commit();
```

## Tracing

All component storage is direct-write storage. The registry enforces per-component access rules: one writer is exclusive, and readers cannot overlap an active writer for the same component.

```cpp
auto tx = registry.transaction<Position>();
tx.write<Position>(entity, Position{10.0f, 20.0f});
tx.commit();
```

Trace history is globally enabled or disabled. Enabling tracing allocates trace structures for existing storages and captures a baseline at the current trace timestamp. Disabling tracing keeps old history but records no new writes, removals, or frames.

```cpp
ecs::Registry registry;
registry.set_trace_max_history(128);
registry.set_current_trace_time(1);
registry.set_tracing_enabled(true);
```

By default, tracing stores copy-on-write records for committed writes and removals. Use `ComponentTraceStorageTraits` when a component should use preallocated frame snapshots instead.

```cpp
struct Transform {
    float x;
    float y;
};

namespace ecs {

template <>
struct ComponentTraceStorageTraits<Transform> {
    static constexpr ComponentTraceStorage value = ComponentTraceStorage::preallocated;
};

}  // namespace ecs
```

## Indices

Declare indexes by specializing `ecs::ComponentIndices<T>`. Use an index when you frequently find entities by component fields or when a view has selective predicates such as "x between 10 and 50". Do not index fields that are rarely queried or updated constantly unless benchmarks show the maintenance cost is worth it.

```cpp
#include <cstdint>
#include <tuple>
#include <vector>

#include "ecs/ecs.hpp"

struct IndexedPosition {
    std::int32_t x;
    std::int32_t y;
};

using XIndex = ecs::Index<&IndexedPosition::x>;
using XYUniqueIndex = ecs::UniqueIndex<&IndexedPosition::x, &IndexedPosition::y>;

namespace ecs {

template <>
struct ComponentIndices<IndexedPosition> {
    using type = std::tuple<XIndex, XYUniqueIndex>;
};

}  // namespace ecs
```

Use `ecs::Index` for non-unique lookup keys and `ecs::UniqueIndex` when the key must identify at most one entity. Unique indexes are enforced on insert, replacement, and commit; violating a unique key throws `std::invalid_argument`.

```cpp
ecs::Registry registry;
const ecs::Entity first = registry.create();
const ecs::Entity second = registry.create();

{
    auto tx = registry.transaction<IndexedPosition>();
    tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
    tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
    tx.commit();
}

auto tx = registry.transaction<IndexedPosition>();
auto positions = tx.storage<IndexedPosition>();

std::vector<ecs::Entity> by_x = positions.find_all<&IndexedPosition::x>(10);
ecs::Entity exact = positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 2);
```

Use single-field indexes for equality, inequality, or range predicates on one member. Use compound indexes when your lookup naturally depends on all key fields in order, such as `(x, y)`.

```cpp
auto tx = registry.transaction<IndexedPosition, Velocity>();

tx.view<const IndexedPosition, const Velocity>()
    .where_gte<&IndexedPosition::x>(10)
    .where_lt<&IndexedPosition::x>(50)
    .forEach([](ecs::Entity entity,
                const IndexedPosition& position,
                const Velocity& velocity) {
        // The planner can seed the view from the x index if it is selective.
    });
```

Available predicate helpers are `where_eq`, `where_ne`, `where_gt`, `where_gte`, `where_lt`, and `where_lte`, plus `or_where_*` variants. Indexes are used when the planner can use them profitably; otherwise the query falls back to scanning. For example, an indexed predicate that matches every row may still scan because the index does not reduce the candidate set.

Choose the index backend at the project or index level:

- `ecs::Index` and `ecs::UniqueIndex` use the build's default backend.
- `ecs::FlatIndex` and `ecs::FlatUniqueIndex` force the flat sorted backend for that index.
- `ecs::OptimizedIndex` and `ecs::OptimizedUniqueIndex` force the optimized B+ tree backend for that index.
- CMake `ECS_INDEX_BACKEND=flat_sorted|optimized_bplus` sets the default backend used by `Index` and `UniqueIndex`.

```cpp
using FlatX = ecs::FlatIndex<&IndexedPosition::x>;
using OptimizedXY = ecs::OptimizedUniqueIndex<&IndexedPosition::x, &IndexedPosition::y>;
```

Important limitation: if a component belongs to an owning group, storage lookups and predicate views for that component still return correct results, but index-seek execution is disabled for that grouped component and the planner scans the visible grouped/storage rows instead.

## Trace History

Trace history is for user-visible rewind, rollback-to-time, replay inspection, and time-travel debugging. It records component state against the registry's current trace timestamp, which you advance with `set_current_trace_time()`. Tracing records data only while `set_tracing_enabled(true)` is active.

Copy-on-write trace storage tracks committed changes. This is the default and is usually the right strategy for health, inventory, gameplay flags, sparse events, and editor state where changes are meaningful but not every component changes every tick.

```cpp
ecs::Registry registry;
registry.set_trace_max_history(16);

const ecs::Entity entity = registry.create();
registry.set_current_trace_time(1);
registry.set_tracing_enabled(true);

{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{1.0f, 2.0f});
    tx.commit();
}

registry.set_current_trace_time(3);
{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{7.0f, 8.0f});
    tx.commit();
}

registry.each_trace_change<Position>(
    entity,
    [](ecs::TraceChangeInfo info, const Position* value) {
        if (info.tombstone) {
            return;
        }

        // value is the Position committed at info.timestamp.
    });

registry.set_current_trace_time(4);
registry.rollback_to_timestamp<Position>(entity, 1);
```

Use preallocated trace storage when most rows are expected to change every frame and you want bounded frame snapshots. The previous dense state is captured when `set_current_trace_time()` advances, so call it at the boundary between simulated times.

```cpp
ecs::Registry registry;
registry.set_trace_max_history(4);

const ecs::Entity entity = registry.create();

registry.set_current_trace_time(10);
registry.set_tracing_enabled(true);
{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{1.0f, 2.0f});
    tx.commit();
}

registry.set_current_trace_time(11); // stores the timestamp 10 frame
{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{3.0f, 4.0f});
    tx.commit();
}

registry.set_current_trace_time(12); // stores the timestamp 11 frame
registry.rollback_to_timestamp<Position>(entity, 10);
```

Trace timestamps are monotonic. `set_trace_max_history()` bounds retained history; older history is compacted when trace time advances. Trace operations that mutate history, including changing trace time and rollback-to-timestamp, require open readers and transactions to be closed first.

Removals are part of trace history. In copy-on-write trace storage, removing a component records a tombstone at the current trace timestamp. Rolling back to a timestamp before a tombstone can restore the component.

Singleton components can also use trace storage. Call `each_trace_change<T>(callback)` and `rollback_to_timestamp<T>(timestamp)` without an entity argument.

## Groups

Owning groups store entities that have the same set of grouped components in group-owned storage. Use a group when a hot view repeatedly reads the same component combination and sparse lookups for the secondary components show up in profiling.

```cpp
ecs::Registry registry;
registry.group<Position, Velocity>();

auto tx = registry.transaction<Position, Velocity>();
tx.view<const Position, const Velocity>().forEach(
    [](ecs::Entity entity, const Position& position, const Velocity& velocity) {
        // The planner can use the owning group as the view source.
    });
```

Groups are best for stable, frequently co-iterated component sets such as `Position` plus `Velocity`. Avoid grouping components that churn in and out constantly unless profiling shows the grouped layout still wins.

A component type can belong to only one owning group. Entities are promoted into the group when they have every grouped component and demoted when a committed removal breaks membership. Existing storage and view APIs continue to work.

## Build

Build a static library:

```bash
cmake -S . -B build/static -DBUILD_SHARED_LIBS=OFF
cmake --build build/static
```

Build a shared library:

```bash
cmake -S . -B build/shared -DBUILD_SHARED_LIBS=ON
cmake --build build/shared
```

Build and run tests:

```bash
cmake -S . -B build/test -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build build/test --target tests
ctest --test-dir build/test --output-on-failure
```

## Benchmarks

Use the benchmark scripts for performance work. They configure benchmark builds consistently, write JSON/log artifacts, and support both index backends. Use `RelWithDebInfo` for benchmark numbers.

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend flat_sorted \
  --min-time 0.2s
```

Run backends serially when collecting numbers:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend optimized_bplus \
  --min-time 0.2s
```

For confirmation, prefer an exact benchmark filter:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend flat_sorted \
  --filter '^BM_ComplexSceneVisibilityQuery/16384$' \
  --min-time 0.2s
```

Useful scripts:

- `scripts/bench/build.sh`: configure and build benchmark targets.
- `scripts/bench/run.sh`: run one benchmark target with JSON/log output.
- `scripts/bench/compare.sh`: run the grouped comparison benchmark family.
- `scripts/bench/profile.sh`: build with Tracy instrumentation and run a profiling-oriented benchmark set.
- `scripts/bench/gprof.sh`: collect `gprof` hotspot direction.

Treat Tracy and `gprof` as hotspot tools, not authoritative timing sources. Record the artifact paths used in any performance summary.

## Layout

- `include/ecs/`: public aggregate/export headers.
- `src/`: library headers, implementation, and unit tests.
- `benchmarks/`: Google Benchmark targets.
- `scripts/bench/`: benchmark build, run, compare, Tracy, and `gprof` helpers.

## Notes

- The same `CMakeLists.txt` is intended to work across macOS, Windows, Linux, and WSL.
- `BUILD_SHARED_LIBS` controls whether `ecs` is created as static or shared.
- `BUILD_TESTING=ON` enables Catch2-based unit tests and creates the `ecs_tests` and `tests` build targets.
- The export macro in `include/ecs/export.hpp` handles symbol visibility for shared builds.
