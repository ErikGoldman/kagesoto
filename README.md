# ECS

ECS is a C++17 entity component system library built around 64-bit entity IDs, paged sparse arrays, typed transactions, snapshots, query views, and optional component indexes.

Entity IDs use 59 low bits for the sparse-array index and 5 high bits for a recycled-version counter. That keeps handles compact while still detecting stale handles after an entity index is reused.

## Optimized For

- Fast component lookups from sparse entity IDs without allocating one huge sparse array.
- Dense component storage for cache-friendly iteration.
- Stale entity detection through versioned 64-bit entity handles.
- Transactional reads and writes with snapshot visibility in MVCC mode.
- Selective lookups through optional B+ tree-backed component indexes.
- Configurable storage behavior per component when rollback, raw write speed, or timestamped history matter more.

## Quick Start

Include the aggregate header, define trivially copyable component types, create entities, and write components through a typed transaction.

```cpp
#include "ecs/ecs.hpp"

struct Position {
    float x;
    float y;
};

int main() {
    ecs::Registry registry;

    const ecs::Entity player = registry.create();

    auto tx = registry.transaction<Position>();
    tx.write<Position>(player, Position{10.0f, 20.0f});
    tx.commit();

    auto read_tx = registry.transaction<Position>();
    const Position& position = read_tx.get<Position>(player);

    return position.x > 0.0f ? 0 : 1;
}
```

Transactions must declare the components they can access. A non-const declaration allows `tx.write<T>()`; a const declaration is read-only. View callbacks receive const component references, so mutate components by calling `tx.write<T>()` inside the transaction.

## Core Concepts

- `ecs::Registry` owns entities, component storage, transactions, snapshots, and storage-mode settings.
- `ecs::Entity` is a 64-bit handle. Use `registry.alive(entity)` when accepting handles from outside the current flow.
- Components are plain trivially copyable C++ structs.
- `registry.transaction<T...>()` opens a snapshot-visible read/write transaction for declared component types.
- `tx.write<T>(entity, value)` stages or applies a component value, depending on the component storage mode.
- `tx.commit()` publishes staged writes. In MVCC and trace modes, closing a transaction without committing rolls staged writes back.
- `registry.snapshot()` opens a read-only snapshot over committed component state.

```cpp
struct Position {
    int x;
    int y;
};

struct Velocity {
    int dx;
    int dy;
};

ecs::Registry registry;
const ecs::Entity mover = registry.create();

{
    auto tx = registry.transaction<Position, Velocity>();
    tx.write<Position>(mover, Position{10, 20});
    tx.write<Velocity>(mover, Velocity{3, 4});
    tx.commit();
}

auto tx = registry.transaction<Position, Velocity>();

if (const Velocity* velocity = tx.try_get<Velocity>(mover)) {
    Position* position = tx.write<Position>(mover);
    position->x += velocity->dx;
    position->y += velocity->dy;
}

tx.commit();
```

Iterate one component storage directly:

```cpp
auto tx = registry.transaction<Position>();

tx.storage<Position>().each([](ecs::Entity entity, const Position& position) {
    // entity has Position in this transaction's visible state.
});
```

Iterate entities that have multiple components:

```cpp
auto tx = registry.transaction<Position, Velocity>();

tx.view<Position, const Velocity>().forEach(
    [](ecs::Entity entity, const Position& position, const Velocity& velocity) {
        // entity has both Position and Velocity.
    });
```

Views choose the smallest visible component storage as the anchor scan and use sparse lookups for the remaining components. Call `explain()` or `explain_text()` when you want to inspect that plan.

## Modes and Settings

Components default to `ecs::ComponentStorageMode::classic`, but storage mode can be configured per component before that component's storage is created.

```cpp
ecs::Registry registry(1024); // page size defaults to 1024

registry.set_storage_mode<Position>(ecs::ComponentStorageMode::mvcc);
registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::trace);
registry.set_trace_max_history(128);
```

Storage modes:

- `classic`: default in-place component storage for lower overhead. Writes are immediately visible through that transaction, explicit rollback is not supported, and the registry enforces per-component reader/writer access constraints.
- `mvcc`: copy-on-write component storage. Transactions see stable snapshots, staged writes can be committed or rolled back, and uncommitted writes are isolated.
- `trace`: MVCC-style storage with timestamped history. Use it when you need per-entity component change history or rollback to earlier trace timestamps.

Set a component type's default mode by specializing `ComponentStorageModeTraits`:

```cpp
struct Transform {
    float x;
    float y;
};

namespace ecs {

template <>
struct ComponentStorageModeTraits<Transform> {
    static constexpr ComponentStorageMode value = ComponentStorageMode::classic;
};

}  // namespace ecs
```

Once storage exists for a component type, changing that component's storage mode throws `std::logic_error`.

## Query Views and Indexes

Declare indexes by specializing `ecs::ComponentIndices<T>`. Use `ecs::Index` for non-unique keys and `ecs::UniqueIndex` for unique keys. Indexes can use one member or a compound key made from multiple members.

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

Indexed storage lookups work through transaction storage views:

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

Predicate views can use component indexes for selective equality, inequality, and range filters:

```cpp
auto tx = registry.transaction<IndexedPosition, Velocity>();

tx.view<const IndexedPosition, const Velocity>()
    .where_gte<&IndexedPosition::x>(10)
    .where_lt<&IndexedPosition::x>(50)
    .forEach([](ecs::Entity entity,
                const IndexedPosition& position,
                const Velocity& velocity) {
        // Only indexed positions in the selected x range are considered.
    });
```

Available predicate operators are `eq`, `ne`, `gt`, `gte`, `lt`, and `lte`, exposed through helpers such as `where_eq`, `where_ne`, `where_gt`, and `where_lte`. Predicates use the query planner, so an indexed predicate can seed execution when it reduces the candidate set and otherwise falls back to a scan.

## Trace History

Trace mode records committed component changes with the registry's current trace timestamp. Timestamps are monotonic and use the 22-bit timestamp range exposed by `ecs::RawPagedSparseArray::max_trace_time()`.

```cpp
ecs::Registry registry;
registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
registry.set_trace_max_history(16);

const ecs::Entity entity = registry.create();

registry.set_current_trace_time(1);
{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{1, 2});
    tx.commit();
}

registry.set_current_trace_time(3);
{
    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{7, 8});
    tx.commit();
}

registry.set_current_trace_time(5);
registry.remove<Position>(entity);

registry.each_trace_change<Position>(
    entity,
    [](ecs::TraceChangeInfo info, const Position* value) {
        if (info.tombstone) {
            return;
        }

        // value points at the Position committed at info.timestamp.
    });

registry.set_current_trace_time(6);
registry.rollback_to_timestamp<Position>(entity, 3);
```

Trace operations that mutate history require open readers and transactions to be closed first.

## Layout

- `include/ecs/`: public headers
- `src/`: library implementation
- `examples/`: small consumer executable
- `tests/`: Catch2 unit tests

## Build a Static Library

```bash
cmake -S . -B build/static -DBUILD_SHARED_LIBS=OFF
cmake --build build/static
```

## Build a Shared Library

```bash
cmake -S . -B build/shared -DBUILD_SHARED_LIBS=ON
cmake --build build/shared
```

## Build and Run Tests

```bash
cmake -S . -B build/test -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build build/test --target tests
ctest --test-dir build/test --output-on-failure
```

## Build and Run Benchmarks

The benchmark suite mirrors the data setup and benchmark shapes used in `abeimler/ecs_benchmark` so the results are directly comparable.

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DECS_BUILD_BENCHMARKS=ON
cmake --build build/bench --target ecs_benchmark_entities ecs_benchmark_basic ecs_benchmark_extended
```

Run the executables directly or emit Google Benchmark JSON:

```bash
./build/bench/ecs_benchmark_entities --benchmark_out=entities.json --benchmark_out_format=json
./build/bench/ecs_benchmark_basic --benchmark_out=basic.json --benchmark_out_format=json
./build/bench/ecs_benchmark_extended --benchmark_out=extended.json --benchmark_out_format=json
```

Suite mapping:

- `ecs_benchmark_entities`: entity creation, destruction, component unpack, and remove/add benchmarks
- `ecs_benchmark_basic`: the 2-system update benchmarks (`MovementSystem` and `DataSystem`)
- `ecs_benchmark_extended`: the 7-system update benchmarks plus iteration benchmarks

## Notes

- The same `CMakeLists.txt` works across macOS, Windows, and Linux.
- `BUILD_SHARED_LIBS` controls whether `ecs` is created as static or shared.
- The export macro in `include/ecs/export.hpp` handles symbol visibility for shared builds.
- `BUILD_TESTING=ON` enables Catch2-based unit tests and creates the `ecs_tests` and `tests` build targets.
