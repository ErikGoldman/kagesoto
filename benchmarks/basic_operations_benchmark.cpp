#include "ecs/ecs.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace {

struct C0 {
    std::int64_t value = 0;
};

struct C1 {
    std::int64_t value = 0;
};

struct C2 {
    std::int64_t value = 0;
};

struct C3 {
    std::int64_t value = 0;
};

struct C4 {
    std::int64_t value = 0;
};

struct C5 {
    std::int64_t value = 0;
};

struct C6 {
    std::int64_t value = 0;
};

struct C7 {
    std::int64_t value = 0;
};

struct C8 {
    std::int64_t value = 0;
};

struct C9 {
    std::int64_t value = 0;
};

struct C10 {
    std::int64_t value = 0;
};

struct C11 {
    std::int64_t value = 0;
};

struct C12 {
    std::int64_t value = 0;
};

struct C13 {
    std::int64_t value = 0;
};

struct C14 {
    std::int64_t value = 0;
};

struct C15 {
    std::int64_t value = 0;
};

template <typename T>
void register_component(ecs::Registry& registry, const char* name) {
    registry.register_component<T>(name);
}

void register_first_n_components(ecs::Registry& registry, int count) {
    register_component<C0>(registry, "C0");
    if (count <= 1) {
        return;
    }

    register_component<C1>(registry, "C1");
    if (count <= 2) {
        return;
    }

    register_component<C2>(registry, "C2");
    register_component<C3>(registry, "C3");
    register_component<C4>(registry, "C4");
    register_component<C5>(registry, "C5");
    register_component<C6>(registry, "C6");
    register_component<C7>(registry, "C7");
    if (count <= 8) {
        return;
    }

    register_component<C8>(registry, "C8");
    register_component<C9>(registry, "C9");
    register_component<C10>(registry, "C10");
    register_component<C11>(registry, "C11");
    register_component<C12>(registry, "C12");
    register_component<C13>(registry, "C13");
    register_component<C14>(registry, "C14");
    register_component<C15>(registry, "C15");
}

std::vector<ecs::Entity> create_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        entities.push_back(registry.create());
    }

    return entities;
}

void add_first_n_components(ecs::Registry& registry, const std::vector<ecs::Entity>& entities, int count) {
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto value = static_cast<std::int64_t>(i);
        registry.add<C0>(entities[i], C0{value});
        if (count <= 1) {
            continue;
        }

        registry.add<C1>(entities[i], C1{value + 1});
        if (count <= 2) {
            continue;
        }

        registry.add<C2>(entities[i], C2{value + 2});
        registry.add<C3>(entities[i], C3{value + 3});
        registry.add<C4>(entities[i], C4{value + 4});
        registry.add<C5>(entities[i], C5{value + 5});
        registry.add<C6>(entities[i], C6{value + 6});
        registry.add<C7>(entities[i], C7{value + 7});
        if (count <= 8) {
            continue;
        }

        registry.add<C8>(entities[i], C8{value + 8});
        registry.add<C9>(entities[i], C9{value + 9});
        registry.add<C10>(entities[i], C10{value + 10});
        registry.add<C11>(entities[i], C11{value + 11});
        registry.add<C12>(entities[i], C12{value + 12});
        registry.add<C13>(entities[i], C13{value + 13});
        registry.add<C14>(entities[i], C14{value + 14});
        registry.add<C15>(entities[i], C15{value + 15});
    }
}

void set_entity_items_processed(benchmark::State& state, int entity_count) {
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count));
}

void BM_AddRemoveComponents(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_component<C0>(registry, "C0");
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);

    for (auto _ : state) {
        for (std::size_t i = 0; i < entities.size(); ++i) {
            benchmark::DoNotOptimize(registry.add<C0>(entities[i], C0{static_cast<std::int64_t>(i)}));
        }

        for (ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(registry.remove<C0>(entity));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 2);
}

void BM_IterateSingleComponent(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, 1);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, 1);

    for (auto _ : state) {
        std::int64_t total = 0;
        for (ecs::Entity entity : entities) {
            total += registry.get<C0>(entity)->value;
        }
        benchmark::DoNotOptimize(total);
    }

    set_entity_items_processed(state, entity_count);
}

template <int ComponentCount>
void BM_IterateComponents(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, ComponentCount);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, ComponentCount);

    for (auto _ : state) {
        std::int64_t total = 0;
        for (ecs::Entity entity : entities) {
            total += registry.get<C0>(entity)->value;
            total += registry.get<C1>(entity)->value;
            if constexpr (ComponentCount >= 8) {
                total += registry.get<C2>(entity)->value;
                total += registry.get<C3>(entity)->value;
                total += registry.get<C4>(entity)->value;
                total += registry.get<C5>(entity)->value;
                total += registry.get<C6>(entity)->value;
                total += registry.get<C7>(entity)->value;
            }
            if constexpr (ComponentCount >= 16) {
                total += registry.get<C8>(entity)->value;
                total += registry.get<C9>(entity)->value;
                total += registry.get<C10>(entity)->value;
                total += registry.get<C11>(entity)->value;
                total += registry.get<C12>(entity)->value;
                total += registry.get<C13>(entity)->value;
                total += registry.get<C14>(entity)->value;
                total += registry.get<C15>(entity)->value;
            }
        }
        benchmark::DoNotOptimize(total);
    }

    set_entity_items_processed(state, entity_count);
}

template <int ComponentCount>
void BM_IterateOwnedGroupViewComponents(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, ComponentCount);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, ComponentCount);

    if constexpr (ComponentCount == 2) {
        registry.declare_owned_group<C0, C1>();
    } else if constexpr (ComponentCount == 8) {
        registry.declare_owned_group<C0, C1, C2, C3, C4, C5, C6, C7>();
    } else if constexpr (ComponentCount == 16) {
        registry.declare_owned_group<C0, C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11, C12, C13, C14, C15>();
    }

    for (auto _ : state) {
        std::int64_t total = 0;
        if constexpr (ComponentCount == 2) {
            registry.view<const C0, const C1>().each([&](ecs::Entity, const C0& c0, const C1& c1) {
                total += c0.value;
                total += c1.value;
            });
        } else if constexpr (ComponentCount == 8) {
            registry.view<const C0, const C1, const C2, const C3, const C4, const C5, const C6, const C7>()
                .each([&](
                          ecs::Entity,
                          const C0& c0,
                          const C1& c1,
                          const C2& c2,
                          const C3& c3,
                          const C4& c4,
                          const C5& c5,
                          const C6& c6,
                          const C7& c7) {
                    total += c0.value;
                    total += c1.value;
                    total += c2.value;
                    total += c3.value;
                    total += c4.value;
                    total += c5.value;
                    total += c6.value;
                    total += c7.value;
                });
        } else if constexpr (ComponentCount == 16) {
            registry
                .view<
                    const C0,
                    const C1,
                    const C2,
                    const C3,
                    const C4,
                    const C5,
                    const C6,
                    const C7,
                    const C8,
                    const C9,
                    const C10,
                    const C11,
                    const C12,
                    const C13,
                    const C14,
                    const C15>()
                .each([&](
                          ecs::Entity,
                          const C0& c0,
                          const C1& c1,
                          const C2& c2,
                          const C3& c3,
                          const C4& c4,
                          const C5& c5,
                          const C6& c6,
                          const C7& c7,
                          const C8& c8,
                          const C9& c9,
                          const C10& c10,
                          const C11& c11,
                          const C12& c12,
                          const C13& c13,
                          const C14& c14,
                          const C15& c15) {
                    total += c0.value;
                    total += c1.value;
                    total += c2.value;
                    total += c3.value;
                    total += c4.value;
                    total += c5.value;
                    total += c6.value;
                    total += c7.value;
                    total += c8.value;
                    total += c9.value;
                    total += c10.value;
                    total += c11.value;
                    total += c12.value;
                    total += c13.value;
                    total += c14.value;
                    total += c15.value;
                });
        }
        benchmark::DoNotOptimize(total);
    }

    set_entity_items_processed(state, entity_count);
}

void BM_IterateWriteSameComponent(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, 1);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, 1);

    for (auto _ : state) {
        for (ecs::Entity entity : entities) {
            C0* component = registry.write<C0>(entity);
            component->value += 1;
            benchmark::DoNotOptimize(component);
        }
        benchmark::ClobberMemory();
    }

    set_entity_items_processed(state, entity_count);
}

void BM_IterateWriteEightComponents(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, 8);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, 8);

    for (auto _ : state) {
        for (ecs::Entity entity : entities) {
            registry.write<C0>(entity)->value += 1;
            registry.write<C1>(entity)->value += 1;
            registry.write<C2>(entity)->value += 1;
            registry.write<C3>(entity)->value += 1;
            registry.write<C4>(entity)->value += 1;
            registry.write<C5>(entity)->value += 1;
            registry.write<C6>(entity)->value += 1;
            registry.write<C7>(entity)->value += 1;
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 8);
}

void BM_IterateWriteOwnedGroupViewEightComponents(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    register_first_n_components(registry, 8);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    add_first_n_components(registry, entities, 8);
    registry.declare_owned_group<C0, C1, C2, C3, C4, C5, C6, C7>();

    for (auto _ : state) {
        registry.view<C0, C1, C2, C3, C4, C5, C6, C7>().each([&](
            ecs::Entity,
            C0& c0,
            C1& c1,
            C2& c2,
            C3& c3,
            C4& c4,
            C5& c5,
            C6& c6,
            C7& c7) {
            c0.value += 1;
            c1.value += 1;
            c2.value += 1;
            c3.value += 1;
            c4.value += 1;
            c5.value += 1;
            c6.value += 1;
            c7.value += 1;
        });
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 8);
}

void BM_AddRemoveEntities(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(entity_count));

    for (auto _ : state) {
        entities.clear();

        for (int i = 0; i < entity_count; ++i) {
            entities.push_back(registry.create());
        }

        for (ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(registry.destroy(entity));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 2);
}

void BasicOperationArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Arg(1024)->Arg(16384)->Arg(65536);
}

BENCHMARK(BM_AddRemoveComponents)->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateSingleComponent)->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateComponents<2>)->Name("BM_IterateTwoComponents")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateComponents<8>)->Name("BM_IterateEightComponents")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateComponents<16>)->Name("BM_IterateSixteenComponents")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateOwnedGroupViewComponents<2>)->Name("BM_IterateTwoComponentsOwnedGroupView")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateOwnedGroupViewComponents<8>)->Name("BM_IterateEightComponentsOwnedGroupView")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateOwnedGroupViewComponents<16>)->Name("BM_IterateSixteenComponentsOwnedGroupView")->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateWriteSameComponent)->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateWriteEightComponents)->Apply(BasicOperationArgs);
BENCHMARK(BM_IterateWriteOwnedGroupViewEightComponents)->Apply(BasicOperationArgs);
BENCHMARK(BM_AddRemoveEntities)->Apply(BasicOperationArgs);

}  // namespace
