#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "ecs/ecs.hpp"
#include "../src/profiler.hpp"

namespace ecs::benchmarks {

inline constexpr std::int64_t kMinEntitiesRange = 0;
inline constexpr std::int64_t kMaxEntitiesRange = 2'097'152;
inline constexpr std::int64_t kSmallMaxEntitiesRange = 32'768;
inline constexpr float kFakeTimeDelta = 1.0f / 60.0f;
inline constexpr const char* kFrameworkName = "kagesoko-ecs";

inline void ApplyArguments(benchmark::internal::Benchmark* benchmark_target, std::int64_t max_entities) {
    benchmark_target->Arg(0);
    for (std::int64_t count = 1; count <= max_entities; count *= 2) {
        benchmark_target->Arg(count);
    }
}

inline void ApplyDefaultArguments(benchmark::internal::Benchmark* benchmark_target) {
    ApplyArguments(benchmark_target, kMaxEntitiesRange);
}

inline void ApplySmallArguments(benchmark::internal::Benchmark* benchmark_target) {
    ApplyArguments(benchmark_target, kSmallMaxEntitiesRange);
}

inline void AddCommonContext(bool add_more_complex_systems) {
    benchmark::AddCustomContext("framework.name", kFrameworkName);
    benchmark::AddCustomContext("framework.version", ECS_BENCHMARK_VERSION);
    benchmark::AddCustomContext("options.add_more_complex_system", add_more_complex_systems ? "true" : "false");
    benchmark::AddCustomContext("index.default_backend", ECS_INDEX_BACKEND_NAME);
}

class RandomXoshiro128 {
public:
    RandomXoshiro128() noexcept {
        const auto seed = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this));
        initialize(seed);
    }

    explicit constexpr RandomXoshiro128(std::uint32_t seed) noexcept {
        initialize(seed);
    }

    constexpr void initialize(std::uint32_t seed) noexcept {
        state_[0] = seed + 3u;
        state_[1] = seed + 5u;
        state_[2] = seed + 7u;
        state_[3] = seed + 11u;
    }

    constexpr std::uint32_t operator()() noexcept {
        return next();
    }

    constexpr std::uint32_t range(std::uint32_t low, std::uint32_t high) noexcept {
        const std::uint32_t width = high - low + 1u;
        return low + (operator()() % width);
    }

private:
    static constexpr std::uint32_t rotl(std::uint32_t value, int shift) noexcept {
        return (value << shift) | (value >> (32 - shift));
    }

    constexpr std::uint32_t next() noexcept {
        const std::uint32_t result = rotl(state_[1] * 5u, 7) * 9u;
        const std::uint32_t temp = state_[1] << 9;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= temp;
        state_[3] = rotl(state_[3], 11);

        return result;
    }

    std::array<std::uint32_t, 4> state_{};
};

struct PositionComponent {
    float x{0.0f};
    float y{0.0f};
};

struct VelocityComponent {
    float x{1.0f};
    float y{1.0f};
};

struct DataComponent {
    static constexpr std::uint32_t kDefaultSeed = 340383u;

    int thingy{0};
    double dingy{0.0};
    bool mingy{false};
    std::uint32_t seed{kDefaultSeed};
    RandomXoshiro128 rng{seed};
    std::uint32_t numgy{rng()};
};

enum class PlayerType { NPC, Monster, Hero };
enum class StatusEffect { Spawn, Dead, Alive };

struct PlayerComponent {
    RandomXoshiro128 rng{};
    PlayerType type{PlayerType::NPC};
};

struct HealthComponent {
    std::int32_t hp{0};
    std::int32_t maxhp{0};
    StatusEffect status{StatusEffect::Spawn};
};

struct DamageComponent {
    std::int32_t atk{0};
    std::int32_t def{0};
};

struct SpriteComponent {
    char character{' '};
};

class FrameBuffer {
public:
    FrameBuffer(std::uint32_t width, std::uint32_t height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), ' ') {}

    void draw(int x, int y, char value) {
        if (x < 0 || y < 0) {
            return;
        }
        if (static_cast<std::uint32_t>(x) >= width_ || static_cast<std::uint32_t>(y) >= height_) {
            return;
        }

        pixels_[static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * width_] = value;
    }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<char> pixels_;
};

struct ComponentsCounter {
    std::size_t component_one_count{0};
    std::size_t component_two_count{0};
    std::size_t component_three_count{0};
    std::size_t hero_count{0};
    std::size_t monster_count{0};
};

template <typename Component, typename TransactionType>
ecs::TransactionStorageView<Component, TransactionType> try_storage(TransactionType& transaction) {
    return transaction.template storage<Component>();
}

template <typename Component, typename... Args>
void write_component(ecs::Registry& registry, ecs::Entity entity, Args&&... args) {
    auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
    tx.write<Component>(entity, std::forward<Args>(args)...);
    tx.commit();
}

template <typename T>
void observe_component(const T& value) {
    using MutableT = std::remove_const_t<T>;
    benchmark::DoNotOptimize(const_cast<MutableT&>(value));
}

template <typename Component, typename Transaction>
void reserve_component_storage(Transaction& tx,
                               std::size_t row_capacity,
                               std::size_t additional_revision_values,
                               std::size_t additional_overflow_nodes) {
    if (const auto* storage = tx.template typed_raw_storage<Component>()) {
        auto* writable_storage = const_cast<ecs::ComponentStorage<Component>*>(storage);
        writable_storage->reserve_rows(row_capacity);
        writable_storage->reserve_revision_values(additional_revision_values);
        writable_storage->reserve_revision_overflow_nodes(additional_overflow_nodes);
    }
}

inline void reserve_system_update_storage(ecs::Registry& registry) {
    auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();

    const std::size_t position_rows = tx.template visible_component_size<PositionComponent>();
    const std::size_t velocity_rows = tx.template visible_component_size<VelocityComponent>();
    const std::size_t data_rows = tx.template visible_component_size<DataComponent>();
    const std::size_t health_rows = tx.template visible_component_size<HealthComponent>();
    const std::size_t damage_rows = tx.template visible_component_size<DamageComponent>();
    const std::size_t sprite_rows = tx.template visible_component_size<SpriteComponent>();

    reserve_component_storage<PositionComponent>(tx, position_rows, position_rows, position_rows);
    reserve_component_storage<VelocityComponent>(tx, velocity_rows, data_rows, data_rows);
    reserve_component_storage<DataComponent>(tx, data_rows, data_rows * 2, data_rows * 2);
    reserve_component_storage<HealthComponent>(tx, health_rows, health_rows * 2, health_rows * 2);
    reserve_component_storage<DamageComponent>(tx, damage_rows, 0, 0);
    reserve_component_storage<SpriteComponent>(tx, sprite_rows, sprite_rows, sprite_rows);
}

class EntityFactory {
public:
    using Registry = ecs::Registry;
    using Entity = ecs::Entity;

    static Entity create_empty(Registry& registry) {
        return registry.create();
    }

    static void create_empty_bulk(Registry& registry, std::vector<Entity>& out) {
        const std::size_t count = out.size();
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(create_empty(registry));
        }
    }

    static Entity create_single(Registry& registry) {
        const Entity entity = registry.create();
        write_component<PositionComponent>(registry, entity);
        return entity;
    }

    static void create_single_bulk(Registry& registry, std::vector<Entity>& out) {
        const std::size_t count = out.size();
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(create_single(registry));
        }
    }

    static Entity create(Registry& registry) {
        const Entity entity = registry.create();
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        tx.write<PositionComponent>(entity);
        tx.write<VelocityComponent>(entity);
        tx.write<DataComponent>(entity);
        tx.commit();
        return entity;
    }

    static void create_bulk(Registry& registry, std::vector<Entity>& out) {
        const std::size_t count = out.size();
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(create(registry));
        }
    }

    static Entity create_minimal(Registry& registry) {
        const Entity entity = registry.create();
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        tx.write<PositionComponent>(entity);
        tx.write<VelocityComponent>(entity);
        tx.commit();
        return entity;
    }

    static void create_minimal_bulk(Registry& registry, std::vector<Entity>& out) {
        const std::size_t count = out.size();
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(create_minimal(registry));
        }
    }

    static void destroy(Registry& registry, Entity entity) {
        registry.destroy(entity);
    }

    static void destroy_bulk(Registry& registry, const std::vector<Entity>& entities) {
        for (const Entity entity : entities) {
            registry.destroy(entity);
        }
    }

    static PositionComponent get_component_one_const(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        return tx.get<PositionComponent>(entity);
    }

    static VelocityComponent get_component_two_const(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        return tx.get<VelocityComponent>(entity);
    }

    static PositionComponent get_component_one(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        return tx.get<PositionComponent>(entity);
    }

    static VelocityComponent get_component_two(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        return tx.get<VelocityComponent>(entity);
    }

    static std::optional<DataComponent> get_optional_component_three(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        if (const DataComponent* component = tx.try_get<DataComponent>(entity)) {
            return *component;
        }
        return std::nullopt;
    }

    static std::optional<DataComponent> get_optional_component_three_const(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        if (const DataComponent* component = tx.try_get<DataComponent>(entity)) {
            return *component;
        }
        return std::nullopt;
    }

    static void remove_component_one(Registry& registry, Entity entity) {
        registry.remove<PositionComponent>(entity);
    }

    static void remove_component_two(Registry& registry, Entity entity) {
        registry.remove<VelocityComponent>(entity);
    }

    static void remove_component_three(Registry& registry, Entity entity) {
        registry.remove<DataComponent>(entity);
    }

    static void add_component_one(Registry& registry, Entity entity) {
        write_component<PositionComponent>(registry, entity);
    }

    static void add_component_two(Registry& registry, Entity entity) {
        write_component<VelocityComponent>(registry, entity);
    }

    static void add_component_three(Registry& registry, Entity entity) {
        write_component<DataComponent>(registry, entity);
    }
};

class HeroMonsterEntityFactory {
public:
    using Registry = ecs::Registry;
    using Entity = ecs::Entity;

    static constexpr std::uint32_t kSpawnAreaMaxX = 320;
    static constexpr std::uint32_t kSpawnAreaMaxY = 240;
    static constexpr std::uint32_t kSpawnAreaMargin = 100;

    static void add_components(Registry& registry, Entity entity) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        tx.write<PlayerComponent>(entity);
        tx.write<HealthComponent>(entity);
        tx.write<DamageComponent>(entity);
        tx.write<PositionComponent>(entity);
        tx.write<SpriteComponent>(entity);
        tx.commit();
    }

    static PlayerType init_components(Registry& registry, Entity entity, std::optional<PlayerType> forced_type = std::nullopt) {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        PositionComponent* position = tx.write<PositionComponent>(entity);
        PlayerComponent* player = tx.write<PlayerComponent>(entity);
        HealthComponent* health = tx.write<HealthComponent>(entity);
        DamageComponent* damage = tx.write<DamageComponent>(entity);
        SpriteComponent* sprite = tx.write<SpriteComponent>(entity);

        const PlayerType type = set_components(*position, *player, *health, *damage, *sprite, forced_type);
        tx.commit();
        return type;
    }

    static PlayerType set_components(PositionComponent& position,
                                     PlayerComponent& player,
                                     HealthComponent& health,
                                     DamageComponent& damage,
                                     SpriteComponent& sprite,
                                     std::optional<PlayerType> forced_type = std::nullopt) {
        player.type = forced_type.value_or([&player]() {
            const std::uint32_t roll = player.rng.range(1, 100);
            if (roll <= 3) {
                return PlayerType::NPC;
            }
            if (roll <= 30) {
                return PlayerType::Hero;
            }
            return PlayerType::Monster;
        }());

        switch (player.type) {
        case PlayerType::Hero:
            health.maxhp = static_cast<std::int32_t>(player.rng.range(5, 15));
            damage.def = static_cast<std::int32_t>(player.rng.range(2, 6));
            damage.atk = static_cast<std::int32_t>(player.rng.range(4, 10));
            break;
        case PlayerType::Monster:
            health.maxhp = static_cast<std::int32_t>(player.rng.range(4, 12));
            damage.def = static_cast<std::int32_t>(player.rng.range(2, 8));
            damage.atk = static_cast<std::int32_t>(player.rng.range(3, 9));
            break;
        case PlayerType::NPC:
            health.maxhp = static_cast<std::int32_t>(player.rng.range(6, 12));
            damage.def = static_cast<std::int32_t>(player.rng.range(3, 8));
            damage.atk = 0;
            break;
        }

        sprite.character = '_';
        position.x = static_cast<float>(player.rng.range(0, kSpawnAreaMaxX + kSpawnAreaMargin)) -
                     static_cast<float>(kSpawnAreaMargin);
        position.y = static_cast<float>(player.rng.range(0, kSpawnAreaMaxY + kSpawnAreaMargin)) -
                     static_cast<float>(kSpawnAreaMargin);
        return player.type;
    }
};

class BenchmarkSystem {
public:
    virtual ~BenchmarkSystem() = default;
    virtual void init(ecs::Registry&) {}
    virtual void update(ecs::Registry& registry, float dt) = 0;
};

class MovementSystem final : public BenchmarkSystem {
public:
    static void update_position(PositionComponent& position, const VelocityComponent& direction, float dt) {
        position.x += direction.x * dt;
        position.y += direction.y * dt;
    }

    void update(ecs::Registry& registry, float dt) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto positions = try_storage<PositionComponent>(tx);
        tx.reserve_pending_writes(positions.size());
        positions.each([&tx, dt](ecs::Entity entity, const PositionComponent&) {
            const VelocityComponent* velocity = tx.try_get<VelocityComponent>(entity);
            if (velocity != nullptr) {
                PositionComponent* position = tx.write<PositionComponent>(entity);
                update_position(*position, *velocity, dt);
            }
        });
        tx.commit();
    }
};

class DataSystem final : public BenchmarkSystem {
public:
    static void update_data(DataComponent& data, float dt) {
        data.thingy = (data.thingy + 1) % 1'000'000;
        data.dingy += 0.0001 * static_cast<double>(dt);
        data.mingy = !data.mingy;
        data.numgy = data.rng();
    }

    void update(ecs::Registry& registry, float dt) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto data = try_storage<DataComponent>(tx);
        tx.reserve_pending_writes(data.size());
        data.each([&tx, dt](ecs::Entity entity, const DataComponent&) {
            DataComponent* value = tx.write<DataComponent>(entity);
            update_data(*value, dt);
        });
        tx.commit();
    }
};

class MoreComplexSystem final : public BenchmarkSystem {
public:
    static void update_components(const PositionComponent& position, VelocityComponent& direction, DataComponent& data) {
        if ((data.thingy % 10) != 0) {
            return;
        }

        if (position.x > position.y) {
            direction.x = static_cast<float>(data.rng.range(3, 19)) - 10.0f;
            direction.y = static_cast<float>(data.rng.range(0, 5));
        } else {
            direction.x = static_cast<float>(data.rng.range(0, 5));
            direction.y = static_cast<float>(data.rng.range(3, 19)) - 10.0f;
        }
    }

    void update(ecs::Registry& registry, float) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto data = try_storage<DataComponent>(tx);
        tx.reserve_pending_writes(data.size() * 2);
        data.each([&tx](ecs::Entity entity, const DataComponent&) {
            const PositionComponent* position = tx.try_get<PositionComponent>(entity);
            if (position != nullptr && tx.try_get<VelocityComponent>(entity) != nullptr) {
                DataComponent* writable_data = tx.write<DataComponent>(entity);
                VelocityComponent* writable_velocity = tx.write<VelocityComponent>(entity);
                update_components(*position, *writable_velocity, *writable_data);
            }
        });
        tx.commit();
    }
};

class HealthSystem final : public BenchmarkSystem {
public:
    static void update_health(HealthComponent& health) {
        if (health.hp <= 0 && health.status != StatusEffect::Dead) {
            health.hp = 0;
            health.status = StatusEffect::Dead;
        } else if (health.status == StatusEffect::Dead && health.hp == 0) {
            health.hp = health.maxhp;
            health.status = StatusEffect::Spawn;
        } else if (health.hp >= health.maxhp && health.status != StatusEffect::Alive) {
            health.hp = health.maxhp;
            health.status = StatusEffect::Alive;
        } else {
            health.status = StatusEffect::Alive;
        }
    }

    void update(ecs::Registry& registry, float) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto health = try_storage<HealthComponent>(tx);
        tx.reserve_pending_writes(health.size());
        health.each([&tx](ecs::Entity entity, const HealthComponent&) {
            HealthComponent* value = tx.write<HealthComponent>(entity);
            update_health(*value);
        });
        tx.commit();
    }
};

class DamageSystem final : public BenchmarkSystem {
public:
    static void update_damage(HealthComponent& health, const DamageComponent& damage) {
        const int total_damage = damage.atk - damage.def;
        if (health.hp > 0 && total_damage > 0) {
            health.hp = std::max(health.hp - total_damage, 0);
        }
    }

    void update(ecs::Registry& registry, float) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto health = try_storage<HealthComponent>(tx);
        tx.reserve_pending_writes(health.size());
        health.each([&tx](ecs::Entity entity, const HealthComponent&) {
            const DamageComponent* damage = tx.try_get<DamageComponent>(entity);
            if (damage != nullptr) {
                HealthComponent* value = tx.write<HealthComponent>(entity);
                update_damage(*value, *damage);
            }
        });
        tx.commit();
    }
};

class SpriteSystem final : public BenchmarkSystem {
public:
    static constexpr char kPlayerSprite = '@';
    static constexpr char kMonsterSprite = 'k';
    static constexpr char kNpcSprite = 'h';
    static constexpr char kGraveSprite = '|';
    static constexpr char kSpawnSprite = '_';
    static constexpr char kNoneSprite = ' ';

    static void update_sprite(SpriteComponent& sprite, const PlayerComponent& player, const HealthComponent& health) {
        switch (health.status) {
        case StatusEffect::Alive:
            switch (player.type) {
            case PlayerType::Hero:
                sprite.character = kPlayerSprite;
                break;
            case PlayerType::Monster:
                sprite.character = kMonsterSprite;
                break;
            case PlayerType::NPC:
                sprite.character = kNpcSprite;
                break;
            }
            break;
        case StatusEffect::Dead:
            sprite.character = kGraveSprite;
            break;
        case StatusEffect::Spawn:
            sprite.character = kSpawnSprite;
            break;
        default:
            sprite.character = kNoneSprite;
            break;
        }
    }

    void update(ecs::Registry& registry, float) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto sprites = try_storage<SpriteComponent>(tx);
        tx.reserve_pending_writes(sprites.size());
        sprites.each([&tx](ecs::Entity entity, const SpriteComponent&) {
            const PlayerComponent* player = tx.try_get<PlayerComponent>(entity);
            const HealthComponent* health = tx.try_get<HealthComponent>(entity);
            if (player != nullptr && health != nullptr) {
                SpriteComponent* sprite = tx.write<SpriteComponent>(entity);
                update_sprite(*sprite, *player, *health);
            }
        });
        tx.commit();
    }
};

class RenderSystem final : public BenchmarkSystem {
public:
    explicit RenderSystem(FrameBuffer& frame_buffer)
        : frame_buffer_(frame_buffer) {}

    static void render_sprite(FrameBuffer& frame_buffer, const PositionComponent& position, const SpriteComponent& sprite) {
        frame_buffer.draw(static_cast<int>(position.x), static_cast<int>(position.y), sprite.character);
    }

    void update(ecs::Registry& registry, float) override {
        auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
        auto positions = try_storage<PositionComponent>(tx);
        positions.each([this, &tx](ecs::Entity entity, const PositionComponent& position) {
            const SpriteComponent* sprite = tx.try_get<SpriteComponent>(entity);
            if (sprite != nullptr) {
                render_sprite(frame_buffer_, position, *sprite);
            }
        });
    }

private:
    FrameBuffer& frame_buffer_;
};

enum class SystemFlavor : bool {
    Basic = false,
    Complex = true,
};

class BenchmarkApplication {
public:
    static constexpr std::uint32_t kFrameBufferWidth = 320;
    static constexpr std::uint32_t kFrameBufferHeight = 240;

    explicit BenchmarkApplication(SystemFlavor flavor = SystemFlavor::Basic)
        : flavor_(flavor),
          frame_buffer_(kFrameBufferWidth, kFrameBufferHeight) {}

    ecs::Registry& entities() {
        return registry_;
    }

    void init() {
        systems_.clear();
        systems_.push_back(std::make_unique<MovementSystem>());
        systems_.push_back(std::make_unique<DataSystem>());

        if (flavor_ == SystemFlavor::Complex) {
            systems_.push_back(std::make_unique<MoreComplexSystem>());
            systems_.push_back(std::make_unique<HealthSystem>());
            systems_.push_back(std::make_unique<DamageSystem>());
            systems_.push_back(std::make_unique<SpriteSystem>());
            systems_.push_back(std::make_unique<RenderSystem>(frame_buffer_));
        }

        for (const auto& system : systems_) {
            system->init(registry_);
        }
    }

    void uninit() {
        systems_.clear();
    }

    void update(float dt) {
        for (const auto& system : systems_) {
            system->update(registry_, dt);
        }
    }

private:
    SystemFlavor flavor_;
    FrameBuffer frame_buffer_;
    ecs::Registry registry_;
    std::vector<std::unique_ptr<BenchmarkSystem>> systems_;
};

class BenchmarkHarness {
public:
    using Registry = ecs::Registry;
    using Entity = ecs::Entity;

    void set_counters(benchmark::State& state, const std::vector<Entity>& entities, const ComponentsCounter& counts) const {
        state.counters["entities"] = static_cast<double>(entities.size());
        state.counters["components_one"] = static_cast<double>(counts.component_one_count);
        state.counters["components_two"] = static_cast<double>(counts.component_two_count);
        state.counters["components_three"] = static_cast<double>(counts.component_three_count);
        state.counters["hero_count"] = static_cast<double>(counts.hero_count);
        state.counters["monster_count"] = static_cast<double>(counts.monster_count);
    }

    void set_iteration_layout_counters(benchmark::State& state,
                                       bool grouped_layout,
                                       std::size_t grouped_component_count,
                                       bool sparse_health) const {
        state.counters["layout_grouped"] = grouped_layout ? 1.0 : 0.0;
        state.counters["grouped_components"] = static_cast<double>(grouped_component_count);
        state.counters["sparse_health"] = sparse_health ? 1.0 : 0.0;
    }

    ComponentsCounter create_no_entities(Registry&, std::vector<Entity>& out) const {
        out.clear();
        return {};
    }

    ComponentsCounter create_entities(Registry& registry, std::size_t count, std::vector<Entity>& out) const {
        ComponentsCounter counts;
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(entity_factory_.create(registry));
            ++counts.component_one_count;
            ++counts.component_two_count;
            ++counts.component_three_count;
        }
        return counts;
    }

    ComponentsCounter create_entities_with_minimal_components(Registry& registry,
                                                              std::size_t count,
                                                              std::vector<Entity>& out) const {
        ComponentsCounter counts;
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(entity_factory_.create_minimal(registry));
            ++counts.component_one_count;
            ++counts.component_two_count;
        }
        return counts;
    }

    ComponentsCounter create_entities_with_single_component(Registry& registry,
                                                            std::size_t count,
                                                            std::vector<Entity>& out) const {
        ComponentsCounter counts;
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(entity_factory_.create_single(registry));
            ++counts.component_one_count;
        }
        return counts;
    }

    ComponentsCounter create_entities_with_half_components(Registry& registry,
                                                           std::size_t count,
                                                           std::vector<Entity>& out) const {
        ComponentsCounter counts;
        out.clear();
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if ((i % 2u) == 0u) {
                out.push_back(entity_factory_.create(registry));
                ++counts.component_one_count;
                ++counts.component_two_count;
                ++counts.component_three_count;
            } else {
                // This matches the upstream benchmark setup exactly, including the output vector shape.
                entity_factory_.create_minimal(registry);
                ++counts.component_one_count;
                ++counts.component_two_count;
            }
        }
        return counts;
    }

    ComponentsCounter create_entities_with_mixed_components(Registry& registry,
                                                            std::size_t count,
                                                            std::vector<Entity>& out) const {
        ComponentsCounter counts;
        out.clear();
        out.reserve(count);

        for (std::size_t i = 0, j = 0; i < count; ++i) {
            out.push_back(entity_factory_.create(registry));
            ++counts.component_one_count;
            ++counts.component_two_count;
            ++counts.component_three_count;

            if (count < 100 || (i >= (2 * count) / 4 && i <= (3 * count) / 4)) {
                if (count < 100 || (j % 10u) == 0u) {
                    if ((i % 7u) == 0u) {
                        entity_factory_.remove_component_one(registry, out.back());
                        --counts.component_one_count;
                    }
                    if ((i % 11u) == 0u) {
                        entity_factory_.remove_component_two(registry, out.back());
                        --counts.component_two_count;
                    }
                    if ((i % 13u) == 0u) {
                        entity_factory_.remove_component_three(registry, out.back());
                        --counts.component_three_count;
                    }
                }
                ++j;
            }
        }

        return counts;
    }

    ComponentsCounter create_entities_with_group_and_sparse_health(Registry& registry,
                                                                   std::size_t count,
                                                                   std::vector<Entity>& out) const {
        ComponentsCounter counts = create_entities_with_minimal_components(registry, count, out);
        for (std::size_t i = 0; i < out.size(); ++i) {
            if ((i % 8u) == 0u) {
                write_component<HealthComponent>(registry, out[i], HealthComponent{100, 100, StatusEffect::Alive});
                ++counts.hero_count;
            }
        }
        return counts;
    }

    ComponentsCounter attach_mixed_hero_monster_components(BenchmarkApplication& app, std::vector<Entity>& entities) const {
        ComponentsCounter counts;
        Registry& registry = app.entities();
        const std::size_t count = entities.size();

        for (std::size_t i = 0, j = 0; i < entities.size(); ++i) {
            const Entity entity = entities[i];
            if (((count < 100) && i == 0u) || count >= 100 || i >= count / 8u) {
                if (((count < 100) && i == 0u) || count >= 100 || (j % 2u) == 0u) {
                    if (i == 0u) {
                        hero_monster_factory_.add_components(registry, entity);
                        hero_monster_factory_.init_components(registry, entity, PlayerType::Hero);
                        ++counts.hero_count;
                    } else if ((i % 6u) == 0u) {
                        hero_monster_factory_.add_components(registry, entity);
                        switch (hero_monster_factory_.init_components(registry, entity)) {
                        case PlayerType::Hero:
                            ++counts.hero_count;
                            break;
                        case PlayerType::Monster:
                            ++counts.monster_count;
                            break;
                        case PlayerType::NPC:
                            break;
                        }
                    } else if ((i % 4u) == 0u) {
                        hero_monster_factory_.add_components(registry, entity);
                        hero_monster_factory_.init_components(registry, entity, PlayerType::Hero);
                        ++counts.hero_count;
                    } else if ((i % 2u) == 0u) {
                        hero_monster_factory_.add_components(registry, entity);
                        hero_monster_factory_.init_components(registry, entity, PlayerType::Monster);
                        ++counts.monster_count;
                    }
                }
                ++j;
            }
        }

        return counts;
    }

    void benchmark_create_no_entities(benchmark::State& state) const {
        constexpr std::size_t entity_count = 0;
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            state.ResumeTiming();
            for (std::size_t i = 0; i < entity_count; ++i) {
                benchmark::DoNotOptimize(entity_factory_.create_empty(registry));
            }
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_create_empty_entities(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            state.ResumeTiming();
            for (std::size_t i = 0; i < entity_count; ++i) {
                benchmark::DoNotOptimize(entity_factory_.create_empty(registry));
            }
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_create_empty_entities_in_bulk(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            std::vector<Entity> entities(entity_count);
            state.ResumeTiming();
            entity_factory_.create_empty_bulk(registry, entities);
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_create_entities(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            state.ResumeTiming();
            for (std::size_t i = 0; i < entity_count; ++i) {
                benchmark::DoNotOptimize(entity_factory_.create(registry));
            }
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_create_entities_in_bulk(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            std::vector<Entity> entities(entity_count);
            state.ResumeTiming();
            entity_factory_.create_bulk(registry, entities);
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_destroy_entities(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            std::vector<Entity> entities;
            entities.reserve(entity_count);
            for (std::size_t i = 0; i < entity_count; ++i) {
                entities.push_back(entity_factory_.create_minimal(registry));
            }
            state.ResumeTiming();
            for (const Entity entity : entities) {
                entity_factory_.destroy(registry, entity);
            }
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_destroy_entities_in_bulk(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        for (auto _ : state) {
            state.PauseTiming();
            Registry registry;
            std::vector<Entity> entities(entity_count);
            entity_factory_.create_minimal_bulk(registry, entities);
            state.ResumeTiming();
            entity_factory_.destroy_bulk(registry, entities);
        }
        state.counters["entities"] = static_cast<double>(entity_count);
    }

    void benchmark_unpack_no_component(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        create_no_entities(registry, entities);
        entities.reserve(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i) {
            entities.push_back(entity_factory_.create_empty(registry));
        }

        for (auto _ : state) {
            for (const Entity entity : entities) {
                benchmark::DoNotOptimize(entity_factory_.get_optional_component_three(registry, entity));
            }
        }

        set_counters(state, entities, {});
    }

    void benchmark_unpack_one_component(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            for (const Entity entity : entities) {
                benchmark::DoNotOptimize(entity_factory_.get_component_one(registry, entity));
            }
        }

        set_counters(state, entities, counts);
    }

    void benchmark_unpack_two_components(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            for (const Entity entity : entities) {
                benchmark::DoNotOptimize(entity_factory_.get_component_one(registry, entity));
                benchmark::DoNotOptimize(entity_factory_.get_component_two(registry, entity));
            }
        }

        set_counters(state, entities, counts);
    }

    void benchmark_unpack_three_components(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_half_components(registry, entity_count, entities);

        for (auto _ : state) {
            for (const Entity entity : entities) {
                benchmark::DoNotOptimize(entity_factory_.get_component_one(registry, entity));
                benchmark::DoNotOptimize(entity_factory_.get_component_two(registry, entity));
                benchmark::DoNotOptimize(entity_factory_.get_optional_component_three(registry, entity));
            }
        }

        set_counters(state, entities, counts);
    }

    void benchmark_add_component(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            for (const Entity entity : entities) {
                state.PauseTiming();
                entity_factory_.remove_component_one(registry, entity);
                state.ResumeTiming();
                entity_factory_.add_component_one(registry, entity);
            }
        }

        set_counters(state, entities, counts);
    }

    void benchmark_remove_add_component(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            for (const Entity entity : entities) {
                entity_factory_.remove_component_one(registry, entity);
                entity_factory_.add_component_one(registry, entity);
            }
        }

        set_counters(state, entities, counts);
    }

    void benchmark_systems_update(benchmark::State& state, SystemFlavor flavor, bool mixed_entities) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        BenchmarkApplication application(flavor);
        application.init();

        Registry& registry = application.entities();
        std::vector<Entity> entities;
        ComponentsCounter counts = mixed_entities
            ? create_entities_with_mixed_components(registry, entity_count, entities)
            : create_entities(registry, entity_count, entities);

        const ComponentsCounter hero_monster_counts = attach_mixed_hero_monster_components(application, entities);
        counts.hero_count = hero_monster_counts.hero_count;
        counts.monster_count = hero_monster_counts.monster_count;
        reserve_system_update_storage(registry);

        for (auto _ : state) {
            application.update(kFakeTimeDelta);
        }

        set_counters(state, entities, counts);
        application.uninit();
    }

    void benchmark_iterate_single_component(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_single_component(registry, entity_count, entities);

        for (auto _ : state) {
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            auto positions = try_storage<PositionComponent>(tx);
            positions.each([](Entity, const PositionComponent& component) {
                observe_component(component);
            });
        }

        set_counters(state, entities, counts);
    }

    void benchmark_iterate_two_components(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            auto positions = try_storage<PositionComponent>(tx);
            positions.each([&tx](Entity entity, const PositionComponent& position) {
                const VelocityComponent* velocity = tx.try_get<VelocityComponent>(entity);
                if (velocity != nullptr) {
                    observe_component(position);
                    observe_component(*velocity);
                }
            });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, false, 0, false);
    }

    void benchmark_iterate_two_components_grouped(benchmark::State& state) const {
        ECS_PROFILE_ZONE("benchmark_iterate_two_components_grouped");
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);
        registry.group<PositionComponent, VelocityComponent>();

        for (auto _ : state) {
            ECS_PROFILE_FRAME("ecs_benchmarks");
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            tx.view<const PositionComponent, const VelocityComponent>().forEach([](Entity, const PositionComponent& position, const VelocityComponent& velocity) {
                observe_component(position);
                observe_component(velocity);
            });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, true, 2, false);
    }

    void benchmark_compare_iterate_two_components_standalone(benchmark::State& state) const {
        ECS_PROFILE_ZONE("benchmark_compare_iterate_two_components_standalone");
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_minimal_components(registry, entity_count, entities);

        for (auto _ : state) {
            ECS_PROFILE_FRAME("ecs_benchmarks");
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            tx.view<const PositionComponent, const VelocityComponent>().forEach([](Entity, const PositionComponent& position, const VelocityComponent& velocity) {
                observe_component(position);
                observe_component(velocity);
            });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, false, 0, false);
    }

    void benchmark_compare_iterate_two_components_grouped(benchmark::State& state) const {
        benchmark_iterate_two_components_grouped(state);
    }

    void benchmark_iterate_three_components_with_mixed_entities(benchmark::State& state) const {
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_mixed_components(registry, entity_count, entities);

        for (auto _ : state) {
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            auto data = try_storage<DataComponent>(tx);
            data.each([&tx](Entity entity, const DataComponent& value) {
                const PositionComponent* position = tx.try_get<PositionComponent>(entity);
                const VelocityComponent* velocity = tx.try_get<VelocityComponent>(entity);
                if (position != nullptr && velocity != nullptr) {
                    observe_component(*position);
                    observe_component(*velocity);
                    observe_component(value);
                }
            });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, false, 0, false);
    }

    void benchmark_iterate_three_components_with_sparse_health(benchmark::State& state) const {
        ECS_PROFILE_ZONE("benchmark_iterate_three_components_with_sparse_health");
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_group_and_sparse_health(registry, entity_count, entities);

        for (auto _ : state) {
            ECS_PROFILE_FRAME("ecs_benchmarks");
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            tx.view<const PositionComponent, const VelocityComponent, const HealthComponent>().forEach(
                [](Entity, const PositionComponent& position, const VelocityComponent& velocity, const HealthComponent& health) {
                    observe_component(position);
                    observe_component(velocity);
                    observe_component(health);
                });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, false, 0, true);
    }

    void benchmark_iterate_three_components_grouped_with_sparse_health(benchmark::State& state) const {
        ECS_PROFILE_ZONE("benchmark_iterate_three_components_grouped_with_sparse_health");
        const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
        Registry registry;
        std::vector<Entity> entities;
        const ComponentsCounter counts = create_entities_with_group_and_sparse_health(registry, entity_count, entities);
        registry.group<PositionComponent, VelocityComponent>();

        for (auto _ : state) {
            ECS_PROFILE_FRAME("ecs_benchmarks");
            auto tx = registry.transaction<PositionComponent, VelocityComponent, DataComponent, PlayerComponent, HealthComponent, DamageComponent, SpriteComponent>();
            tx.view<const PositionComponent, const VelocityComponent, const HealthComponent>().forEach(
                [](Entity, const PositionComponent& position, const VelocityComponent& velocity, const HealthComponent& health) {
                    observe_component(position);
                    observe_component(velocity);
                    observe_component(health);
                });
        }

        set_counters(state, entities, counts);
        set_iteration_layout_counters(state, true, 2, true);
    }

    void benchmark_compare_iterate_three_components_sparse_health_standalone(benchmark::State& state) const {
        benchmark_iterate_three_components_with_sparse_health(state);
    }

    void benchmark_compare_iterate_three_components_sparse_health_grouped(benchmark::State& state) const {
        benchmark_iterate_three_components_grouped_with_sparse_health(state);
    }

private:
    EntityFactory entity_factory_;
    HeroMonsterEntityFactory hero_monster_factory_;
};

}  // namespace ecs::benchmarks
