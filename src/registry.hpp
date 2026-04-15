#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ecs/export.hpp"
#include "paged_sparse_array.hpp"

namespace ecs {

using ComponentId = std::uint32_t;
inline constexpr ComponentId null_component = std::numeric_limits<ComponentId>::max();

namespace detail {

ECS_API ComponentId next_component_id();

}  // namespace detail

template <typename T>
struct ComponentIdTraits {
    static ComponentId value() {
        static const ComponentId id = detail::next_component_id();
        return id;
    }
};

template <typename T>
ComponentId component_id() {
    return ComponentIdTraits<T>::value();
}

class Registry;
class Snapshot;
class Transaction;

class ECS_API Registry {
public:
    explicit Registry(std::size_t page_size = 1024);
    ~Registry();

    Registry(Registry&& other) noexcept;
    Registry& operator=(Registry&& other) noexcept;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    Entity create();
    bool destroy(Entity entity);
    void clear();

    bool alive(Entity entity) const;
    std::size_t page_size() const;
    std::size_t entity_count() const;
    const std::vector<Entity>& entities() const;
    bool remove(Entity entity, ComponentId component);

    template <typename T>
    bool remove(Entity entity) {
        return remove(entity, component_id<T>());
    }

    Snapshot snapshot();
    Transaction transaction();

private:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max() - 1;

    bool has(Entity entity, ComponentId component) const;
    const void* try_get(Entity entity, ComponentId component) const;
    RawPagedSparseArray* storage(ComponentId component);
    const RawPagedSparseArray* storage(ComponentId component) const;
    void require_no_readers() const;
    void require_alive(Entity entity) const;
    void ensure_entity_slot(Entity index);
    RawPagedSparseArray& assure_storage(ComponentId componentId, std::size_t component_size, std::size_t component_alignment);

    std::uint64_t acquire_tsn();
    std::vector<std::uint64_t> active_transactions_snapshot() const;
    void register_transaction(std::uint64_t tsn);
    void unregister_transaction(std::uint64_t tsn);
    void register_reader();
    void unregister_reader();

    template <typename T>
    ComponentStorage<T>& assure_storage() {
        const ComponentId id = component_id<T>();
        if (id >= components_.size()) {
            components_.resize(static_cast<std::size_t>(id) + 1);
        }

        RawPagedSparseArray* component = components_[id];
        if (component == nullptr) {
            component = new ComponentStorage<T>(page_size_);
            components_[id] = component;
        }

        return *static_cast<ComponentStorage<T>*>(component);
    }

    friend class Snapshot;
    friend class Transaction;

    std::size_t page_size_;
    Entity next_entity_index_ = 0;
    std::vector<EntityVersion> current_versions_;
    std::vector<std::size_t> entity_dense_indices_;
    std::vector<Entity> alive_entities_;
    std::vector<Entity> free_entities_;
    std::vector<RawPagedSparseArray*> components_;
    std::uint64_t next_tsn_ = 1;
    std::vector<std::uint64_t> active_transactions_;
    std::size_t active_readers_ = 0;
};

}  // namespace ecs

#include "transaction.hpp"

namespace ecs {

inline Snapshot Registry::snapshot() {
    return Snapshot(*this);
}

inline Transaction Registry::transaction() {
    return Transaction(*this);
}

}  // namespace ecs
