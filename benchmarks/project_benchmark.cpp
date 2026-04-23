#include "benchmark_common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace {

using ecs::benchmarks::RandomXoshiro128;
using ecs::benchmarks::observe_component;

struct IndexedScenePosition {
    std::int32_t x{0};
    std::int32_t y{0};
};

struct SceneVelocity {
    std::int32_t dx{0};
    std::int32_t dy{0};
};

struct SceneSprite {
    char glyph{' '};
};

struct SceneCombatStats {
    std::int32_t hp{0};
    std::int32_t max_hp{0};
    std::int32_t atk{0};
    std::int32_t def{0};
};

struct SceneAiState {
    std::uint32_t seed{0};
    RandomXoshiro128 rng{seed};
    std::uint32_t phase{0};
};

struct TransientEffect {
    std::uint32_t ttl{0};
};

using SceneXIndex = ecs::Index<&IndexedScenePosition::x>;
using SceneXYUniqueIndex = ecs::UniqueIndex<&IndexedScenePosition::x, &IndexedScenePosition::y>;

constexpr std::int64_t kProjectMaxEntitiesRange = 262'144;
constexpr std::int32_t kWorldWidth = 2048;
constexpr std::int32_t kWorldHeight = 1024;
constexpr std::int32_t kCameraMinX = 384;
constexpr std::int32_t kCameraMaxX = 640;
constexpr std::int32_t kCameraMinY = 128;
constexpr std::int32_t kCameraMaxY = 384;
constexpr std::int32_t kSingleKeyTarget = 32;
constexpr std::int32_t kRangeMinX = 16;
constexpr std::int32_t kRangeMaxX = 76;

enum class SceneArchetype : std::uint8_t {
    HomogeneousMover,
    Combatant,
    StaticRenderable,
    Special,
};

struct SceneCounters {
    std::size_t homogeneous_entities{0};
    std::size_t heterogeneous_entities{0};
    std::size_t static_entities{0};
    std::size_t transient_entities{0};
};

void ApplyProjectArguments(benchmark::internal::Benchmark* benchmark_target) {
    ecs::benchmarks::ApplyArguments(benchmark_target, kProjectMaxEntitiesRange);
}

std::uint32_t make_seed(std::size_t index) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(index) * 2'654'435'761ull) ^ 0x85EBCA6Bu);
}

SceneArchetype classify_scene_archetype(std::size_t index) {
    const std::size_t bucket = index % 20u;
    if (bucket < 12u) {
        return SceneArchetype::HomogeneousMover;
    }
    if (bucket < 17u) {
        return SceneArchetype::Combatant;
    }
    if (bucket < 19u) {
        return SceneArchetype::StaticRenderable;
    }
    return SceneArchetype::Special;
}

void set_common_position(IndexedScenePosition& position, std::size_t index) {
    position.x = static_cast<std::int32_t>((index * 17u) % static_cast<std::size_t>(kWorldWidth));
    position.y = static_cast<std::int32_t>(index);
}

void set_common_velocity(SceneVelocity& velocity, std::size_t index) {
    velocity.dx = static_cast<std::int32_t>((index % 7u) + 1u);
    velocity.dy = static_cast<std::int32_t>((index % 5u) - 2u);
}

void seed_mixed_component_entities(ecs::Registry& registry,
                                   std::size_t entity_count,
                                   std::vector<ecs::Entity>& entities) {
    entities.clear();
    entities.reserve(entity_count);
    for (std::size_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = entities[i];
        IndexedScenePosition* position = tx.write<IndexedScenePosition>(entity);
        set_common_position(*position, i);

        if ((i % 5u) != 0u) {
            SceneVelocity* velocity = tx.write<SceneVelocity>(entity);
            set_common_velocity(*velocity, i);
        }

        if ((i % 4u) == 0u) {
            SceneCombatStats* combat = tx.write<SceneCombatStats>(entity);
            combat->max_hp = 100 + static_cast<std::int32_t>(i % 50u);
            combat->hp = combat->max_hp;
            combat->atk = 8 + static_cast<std::int32_t>(i % 7u);
            combat->def = 2 + static_cast<std::int32_t>(i % 5u);
        }

        if ((i % 6u) == 0u) {
            SceneAiState* ai = tx.write<SceneAiState>(entity);
            ai->seed = make_seed(i);
            ai->rng.initialize(ai->seed);
            ai->phase = ai->rng.range(0u, 15u);
        }

        if ((i % 3u) == 0u) {
            SceneSprite* sprite = tx.write<SceneSprite>(entity);
            sprite->glyph = static_cast<char>('A' + static_cast<char>(i % 26u));
        }
    }
    tx.commit();
}

void seed_entities_with_transient_effect(ecs::Registry& registry,
                                         std::size_t entity_count,
                                         std::vector<ecs::Entity>& entities,
                                         bool sparse_only) {
    entities.clear();
    entities.reserve(entity_count);
    for (std::size_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = entities[i];
        IndexedScenePosition* position = tx.write<IndexedScenePosition>(entity);
        set_common_position(*position, i);

        const bool add_effect = !sparse_only || ((i % 16u) == 0u);
        if (add_effect) {
            TransientEffect* effect = tx.write<TransientEffect>(entity);
            effect->ttl = 3u + static_cast<std::uint32_t>(i % 5u);
        }
    }
    tx.commit();
}

void seed_index_entities(ecs::Registry& registry,
                         std::size_t entity_count,
                         std::vector<ecs::Entity>& entities,
                         bool with_velocity) {
    entities.clear();
    entities.reserve(entity_count);
    for (std::size_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = entities[i];
        IndexedScenePosition* position = tx.write<IndexedScenePosition>(entity);
        position->x = static_cast<std::int32_t>(i % 128u);
        position->y = static_cast<std::int32_t>(i);

        if (with_velocity && ((i % 3u) != 1u)) {
            SceneVelocity* velocity = tx.write<SceneVelocity>(entity);
            set_common_velocity(*velocity, i);
        }
    }
    tx.commit();
}

SceneCounters seed_complex_scene(ecs::Registry& registry,
                                 std::size_t entity_count,
                                 std::vector<ecs::Entity>& entities,
                                 std::vector<ecs::Entity>& transient_entities) {
    ECS_PROFILE_ZONE("seed_complex_scene");
    SceneCounters counters;
    entities.clear();
    entities.reserve(entity_count);
    transient_entities.clear();
    transient_entities.reserve(entity_count / 20u + 1u);
    for (std::size_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = entities[i];
        IndexedScenePosition* position = tx.write<IndexedScenePosition>(entity);
        set_common_position(*position, i);

        SceneSprite* sprite = tx.write<SceneSprite>(entity);
        sprite->glyph = '.';

        switch (classify_scene_archetype(i)) {
        case SceneArchetype::HomogeneousMover: {
            SceneVelocity* velocity = tx.write<SceneVelocity>(entity);
            set_common_velocity(*velocity, i);
            sprite->glyph = 'm';
            ++counters.homogeneous_entities;
            break;
        }
        case SceneArchetype::Combatant: {
            SceneVelocity* velocity = tx.write<SceneVelocity>(entity);
            set_common_velocity(*velocity, i);
            SceneCombatStats* combat = tx.write<SceneCombatStats>(entity);
            combat->max_hp = 120 + static_cast<std::int32_t>(i % 60u);
            combat->hp = combat->max_hp;
            combat->atk = 10 + static_cast<std::int32_t>(i % 8u);
            combat->def = 3 + static_cast<std::int32_t>(i % 6u);
            SceneAiState* ai = tx.write<SceneAiState>(entity);
            ai->seed = make_seed(i);
            ai->rng.initialize(ai->seed);
            ai->phase = ai->rng.range(0u, 31u);
            sprite->glyph = 'c';
            ++counters.heterogeneous_entities;
            break;
        }
        case SceneArchetype::StaticRenderable:
            sprite->glyph = 's';
            ++counters.static_entities;
            break;
        case SceneArchetype::Special: {
            SceneVelocity* velocity = tx.write<SceneVelocity>(entity);
            set_common_velocity(*velocity, i);
            velocity->dx *= 2;
            velocity->dy *= 2;
            SceneCombatStats* combat = tx.write<SceneCombatStats>(entity);
            combat->max_hp = 250 + static_cast<std::int32_t>(i % 100u);
            combat->hp = combat->max_hp;
            combat->atk = 20 + static_cast<std::int32_t>(i % 12u);
            combat->def = 8 + static_cast<std::int32_t>(i % 4u);
            SceneAiState* ai = tx.write<SceneAiState>(entity);
            ai->seed = make_seed(i ^ 0x9e3779b9u);
            ai->rng.initialize(ai->seed);
            ai->phase = ai->rng.range(0u, 63u);
            TransientEffect* effect = tx.write<TransientEffect>(entity);
            effect->ttl = 5u + static_cast<std::uint32_t>(i % 7u);
            transient_entities.push_back(entity);
            sprite->glyph = 'b';
            ++counters.heterogeneous_entities;
            ++counters.transient_entities;
            break;
        }
        }
    }
    tx.commit();
    return counters;
}

void set_scene_counters(benchmark::State& state, const SceneCounters& counters) {
    state.counters["homogeneous_entities"] = static_cast<double>(counters.homogeneous_entities);
    state.counters["heterogeneous_entities"] = static_cast<double>(counters.heterogeneous_entities);
    state.counters["static_entities"] = static_cast<double>(counters.static_entities);
    state.counters["transient_entities"] = static_cast<double>(counters.transient_entities);
}

void run_scene_frame(ecs::Registry& registry) {
    {
        ECS_PROFILE_ZONE("project_scene_frame_movement");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition, const SceneVelocity>().forEach([&tx](ecs::Entity entity, const IndexedScenePosition& position, const SceneVelocity& velocity) {
            IndexedScenePosition* writable = tx.write<IndexedScenePosition>(entity);
            writable->x = (position.x + velocity.dx + kWorldWidth) % kWorldWidth;
            writable->y = position.y;
        });
        tx.commit();
    }

    {
        ECS_PROFILE_ZONE("project_scene_frame_ai");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        auto ai_storage = tx.storage<SceneAiState>();
        tx.reserve_pending_writes(ai_storage.size() * 2u);
        ai_storage.each([&tx](ecs::Entity entity, const SceneAiState&) {
            SceneAiState* ai = tx.write<SceneAiState>(entity);
            ai->phase = (ai->phase + 1u) & 63u;
            if (const SceneVelocity* velocity = tx.try_get<SceneVelocity>(entity)) {
                SceneVelocity* writable_velocity = tx.write<SceneVelocity>(entity);
                const std::int32_t direction = ((ai->phase & 1u) == 0u) ? 1 : -1;
                writable_velocity->dx = std::clamp(velocity->dx + direction, -6, 6);
                writable_velocity->dy = std::clamp(velocity->dy - direction, -6, 6);
            }
        });
        tx.commit();
    }

    {
        ECS_PROFILE_ZONE("project_scene_frame_combat");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        auto combat = tx.storage<SceneCombatStats>();
        tx.reserve_pending_writes(combat.size());
        combat.each([&tx](ecs::Entity entity, const SceneCombatStats& current) {
            SceneCombatStats* writable = tx.write<SceneCombatStats>(entity);
            const std::int32_t damage = std::max(current.atk - current.def, 1);
            writable->hp -= damage;
            if (writable->hp <= 0) {
                writable->hp = writable->max_hp;
            }
        });
        tx.commit();
    }

    {
        ECS_PROFILE_ZONE("project_scene_frame_visibility");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        auto visible = tx.view<const IndexedScenePosition, const SceneSprite>()
                           .where_gte<&IndexedScenePosition::x>(kCameraMinX)
                           .where_lte<&IndexedScenePosition::x>(kCameraMaxX);
        visible.forEach([](ecs::Entity, const IndexedScenePosition& position, const SceneSprite& sprite) {
            if (position.y >= kCameraMinY && position.y <= kCameraMaxY) {
                observe_component(position);
                observe_component(sprite);
            }
        });
    }
}

void BM_HasComponentMixedEntities(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_HasComponentMixedEntities");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_mixed_component_entities(registry, entity_count, entities);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        for (const ecs::Entity entity : entities) {
            hits += tx.has<SceneVelocity>(entity) ? 1u : 0u;
        }
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["component_hits"] = static_cast<double>(hits);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(entity_count));
}
BENCHMARK(BM_HasComponentMixedEntities)->Apply(ApplyProjectArguments);

void BM_TryGetComponentMixedEntities(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_TryGetComponentMixedEntities");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_mixed_component_entities(registry, entity_count, entities);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        for (const ecs::Entity entity : entities) {
            if (const SceneCombatStats* combat = tx.try_get<SceneCombatStats>(entity)) {
                observe_component(*combat);
                ++hits;
            }
        }
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["component_hits"] = static_cast<double>(hits);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(entity_count));
}
BENCHMARK(BM_TryGetComponentMixedEntities)->Apply(ApplyProjectArguments);

void BM_RemoveComponent(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_RemoveComponent");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    std::vector<ecs::Entity> entities;

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        seed_entities_with_transient_effect(registry, entity_count, entities, false);
        state.ResumeTiming();
        ECS_PROFILE_FRAME("ecs_benchmarks_project");

        std::size_t removals = 0;
        for (const ecs::Entity entity : entities) {
            removals += registry.remove<TransientEffect>(entity) ? 1u : 0u;
        }
        benchmark::DoNotOptimize(removals);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
}
BENCHMARK(BM_RemoveComponent)->Apply(ApplyProjectArguments);

void BM_RemoveComponentSparseSubset(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_RemoveComponentSparseSubset");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    std::vector<ecs::Entity> entities;

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        seed_entities_with_transient_effect(registry, entity_count, entities, true);
        state.ResumeTiming();
        ECS_PROFILE_FRAME("ecs_benchmarks_project");

        std::size_t removals = 0;
        for (const ecs::Entity entity : entities) {
            removals += registry.remove<TransientEffect>(entity) ? 1u : 0u;
        }
        benchmark::DoNotOptimize(removals);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["sparse_subset"] = 1.0;
}
BENCHMARK(BM_RemoveComponentSparseSubset)->Apply(ApplyProjectArguments);

void BM_IndexedFindSingleKey(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedFindSingleKey");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        ECS_PROFILE_ZONE("project_index_find_single_key");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        auto positions = tx.storage<IndexedScenePosition>();
        const std::vector<ecs::Entity> matches = positions.find_all<&IndexedScenePosition::x>(kSingleKeyTarget);
        hits = matches.size();
        benchmark::DoNotOptimize(matches.data());
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
}
BENCHMARK(BM_IndexedFindSingleKey)->Apply(ApplyProjectArguments);

void BM_IndexedFindCompoundKey(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedFindCompoundKey");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    ecs::Entity match = ecs::null_entity;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        ECS_PROFILE_ZONE("project_index_find_compound_key");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        auto positions = tx.storage<IndexedScenePosition>();
        const std::int32_t target_y = entity_count == 0 ? 0 : static_cast<std::int32_t>(entity_count / 2u);
        const std::int32_t target_x = entity_count == 0 ? 0 : target_y % 128;
        match = positions.find_one<&IndexedScenePosition::x, &IndexedScenePosition::y>(target_x, target_y);
        benchmark::DoNotOptimize(match);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = match == ecs::null_entity ? 0.0 : 1.0;
}
BENCHMARK(BM_IndexedFindCompoundKey)->Apply(ApplyProjectArguments);

void BM_IndexedViewWhereEq(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedViewWhereEq");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        ECS_PROFILE_ZONE("project_index_view_where_eq");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition>()
            .where_eq<&IndexedScenePosition::x>(kSingleKeyTarget)
            .forEach([&hits](ecs::Entity, const IndexedScenePosition& position) {
                observe_component(position);
                ++hits;
            });
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(hits));
}
BENCHMARK(BM_IndexedViewWhereEq)->Apply(ApplyProjectArguments);

void BM_IndexedViewRange(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedViewRange");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        ECS_PROFILE_ZONE("project_index_view_range");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition>()
            .where_gte<&IndexedScenePosition::x>(kRangeMinX)
            .where_lte<&IndexedScenePosition::x>(kRangeMaxX)
            .forEach([&hits](ecs::Entity, const IndexedScenePosition& position) {
                observe_component(position);
                ++hits;
            });
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
}
BENCHMARK(BM_IndexedViewRange)->Apply(ApplyProjectArguments);

void BM_IndexedViewPredicateComposeAnd(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedViewPredicateComposeAnd");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        ECS_PROFILE_ZONE("project_index_view_predicate_compose_and");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition>()
            .where_gte<&IndexedScenePosition::x>(20)
            .where_lte<&IndexedScenePosition::x>(30)
            .forEach([&hits](ecs::Entity, const IndexedScenePosition& position) {
                observe_component(position);
                ++hits;
            });
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
}
BENCHMARK(BM_IndexedViewPredicateComposeAnd)->Apply(ApplyProjectArguments);

void BM_IndexedViewPredicateComposeOr(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedViewPredicateComposeOr");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, false);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        ECS_PROFILE_ZONE("project_index_view_predicate_compose_or");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition>()
            .where_eq<&IndexedScenePosition::x>(10)
            .or_where_eq<&IndexedScenePosition::x>(30)
            .forEach([&hits](ecs::Entity, const IndexedScenePosition& position) {
                observe_component(position);
                ++hits;
            });
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
}
BENCHMARK(BM_IndexedViewPredicateComposeOr)->Apply(ApplyProjectArguments);

void BM_IndexedViewWithSecondaryFetch(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_IndexedViewWithSecondaryFetch");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    seed_index_entities(registry, entity_count, entities, true);

    std::size_t hits = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        hits = 0;
        ECS_PROFILE_ZONE("project_index_view_secondary_fetch");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition, const SceneVelocity>()
            .where_eq<&IndexedScenePosition::x>(40)
            .forEach([&hits](ecs::Entity, const IndexedScenePosition& position, const SceneVelocity& velocity) {
                observe_component(position);
                observe_component(velocity);
                ++hits;
            });
        benchmark::DoNotOptimize(hits);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["indexed_hits"] = static_cast<double>(hits);
}
BENCHMARK(BM_IndexedViewWithSecondaryFetch)->Apply(ApplyProjectArguments);

void BM_ComplexSceneFrame(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_ComplexSceneFrame");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    std::vector<ecs::Entity> transient_entities;
    const SceneCounters counters = seed_complex_scene(registry, entity_count, entities, transient_entities);

    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        run_scene_frame(registry);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["layout_grouped"] = 0.0;
    set_scene_counters(state, counters);
}
BENCHMARK(BM_ComplexSceneFrame)->Apply(ApplyProjectArguments);

void BM_ComplexSceneFrameGrouped(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_ComplexSceneFrameGrouped");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    registry.group<IndexedScenePosition, SceneVelocity>();
    std::vector<ecs::Entity> entities;
    std::vector<ecs::Entity> transient_entities;
    const SceneCounters counters = seed_complex_scene(registry, entity_count, entities, transient_entities);

    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        run_scene_frame(registry);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["layout_grouped"] = 1.0;
    set_scene_counters(state, counters);
}
BENCHMARK(BM_ComplexSceneFrameGrouped)->Apply(ApplyProjectArguments);

void BM_ComplexSceneFrameWithChurn(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_ComplexSceneFrameWithChurn");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    std::vector<ecs::Entity> transient_entities;
    const SceneCounters counters = seed_complex_scene(registry, entity_count, entities, transient_entities);
    bool add_effects = false;

    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        run_scene_frame(registry);

        if (add_effects) {
            ECS_PROFILE_ZONE("project_scene_frame_churn_add");
            auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
            for (const ecs::Entity entity : transient_entities) {
                TransientEffect* effect = tx.write<TransientEffect>(entity);
                effect->ttl = 4u;
            }
            tx.commit();
        } else {
            ECS_PROFILE_ZONE("project_scene_frame_churn_remove");
            for (const ecs::Entity entity : transient_entities) {
                registry.remove<TransientEffect>(entity);
            }
        }

        add_effects = !add_effects;
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["layout_grouped"] = 0.0;
    set_scene_counters(state, counters);
}
BENCHMARK(BM_ComplexSceneFrameWithChurn)->Apply(ApplyProjectArguments);

void BM_ComplexSceneVisibilityQuery(benchmark::State& state) {
    ECS_PROFILE_ZONE("BM_ComplexSceneVisibilityQuery");
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    std::vector<ecs::Entity> transient_entities;
    const SceneCounters counters = seed_complex_scene(registry, entity_count, entities, transient_entities);

    std::size_t visible_entities = 0;
    for (auto _ : state) {
        ECS_PROFILE_FRAME("ecs_benchmarks_project");
        visible_entities = 0;
        ECS_PROFILE_ZONE("project_scene_visibility_query");
        auto tx = registry.transaction<IndexedScenePosition, SceneVelocity, SceneCombatStats, SceneAiState, SceneSprite, TransientEffect>();
        tx.view<const IndexedScenePosition, const SceneSprite>()
            .where_gte<&IndexedScenePosition::x>(kCameraMinX)
            .where_lte<&IndexedScenePosition::x>(kCameraMaxX)
            .forEach([&visible_entities](ecs::Entity, const IndexedScenePosition& position, const SceneSprite& sprite) {
                if (position.y >= kCameraMinY && position.y <= kCameraMaxY) {
                    observe_component(position);
                    observe_component(sprite);
                    ++visible_entities;
                }
            });
        benchmark::DoNotOptimize(visible_entities);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["visible_entities"] = static_cast<double>(visible_entities);
    state.counters["layout_grouped"] = 0.0;
    set_scene_counters(state, counters);
}
BENCHMARK(BM_ComplexSceneVisibilityQuery)->Apply(ApplyProjectArguments);

}  // namespace

namespace ecs {

template <>
struct ComponentIndices<IndexedScenePosition> {
    using type = std::tuple<SceneXIndex, SceneXYUniqueIndex>;
};

}  // namespace ecs

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(true);
    benchmark::AddCustomContext("benchmark.kind", "project");
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
