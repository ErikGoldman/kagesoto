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
#include "view_traits.hpp"

namespace ecs {

using ComponentId = std::uint32_t;
inline constexpr ComponentId null_component = std::numeric_limits<ComponentId>::max();

namespace detail {

ECS_API ComponentId next_component_id();

}  // namespace detail

template <typename T>
struct ComponentStorageModeTraits {
    static constexpr ComponentStorageMode value = ComponentStorageMode::mvcc;
};

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
template <typename... DeclaredComponents>
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
    void set_storage_mode(ComponentStorageMode mode) {
        const ComponentId id = component_id<T>();
        ensure_component_slot(id);
        ComponentSlot& slot = components_[id];
        if (slot.storage != nullptr && slot.mode != mode) {
            throw std::logic_error("cannot change component storage mode after storage has been created");
        }
        slot.mode = mode;
        slot.mode_configured = true;
        recompute_classic_mode_flag();
    }

    void set_trace_max_history(std::uint32_t max_history) {
        if (max_history > RawPagedSparseArray::max_trace_time()) {
            throw std::out_of_range("trace max history exceeds 22-bit timestamp range");
        }
        trace_max_history_ = max_history;
    }

    std::uint32_t trace_max_history() const {
        return trace_max_history_;
    }

    void set_current_trace_time(std::uint32_t timestamp) {
        if (timestamp > RawPagedSparseArray::max_trace_time()) {
            throw std::out_of_range("trace timestamp exceeds 22-bit range");
        }
        if (timestamp < trace_commit_context_.timestamp) {
            throw std::logic_error("trace time must be monotonic");
        }

        require_no_readers();
        trace_commit_context_.timestamp = timestamp;
        compact_trace_history();
    }

    std::uint32_t current_trace_time() const {
        return trace_commit_context_.timestamp;
    }

    template <typename T>
    ComponentStorageMode storage_mode() const {
        const ComponentId id = component_id<T>();
        if (id >= components_.size() || !components_[id].mode_configured) {
            return ComponentStorageModeTraits<T>::value;
        }
        return components_[id].mode;
    }

    template <typename T>
    bool remove(Entity entity) {
        return remove(entity, component_id<T>());
    }

    template <typename T, typename Func>
    void each_trace_change(Entity entity, Func&& func) const {
        const ComponentId id = component_id<T>();
        if (const auto* raw = static_cast<const ComponentStorage<T>*>(storage(id))) {
            raw->for_each_trace_change(entity, std::forward<Func>(func));
        }
    }

    template <typename T>
    bool rollback_to_timestamp(Entity entity, std::uint32_t timestamp) {
        require_no_readers();
        const ComponentId id = component_id<T>();
        if (storage_mode<T>() != ComponentStorageMode::trace) {
            throw std::logic_error("trace rollback requires trace component storage mode");
        }
        if (id >= components_.size() || components_[id].storage == nullptr) {
            return false;
        }
        return static_cast<ComponentStorage<T>*>(components_[id].storage)
            ->rollback_to_trace_timestamp(entity, timestamp, trace_commit_context_);
    }

    Snapshot snapshot();

    template <typename... DeclaredComponents>
    Transaction<DeclaredComponents...> transaction();

private:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max() - 1;

    struct ComponentSlot {
        RawPagedSparseArray* storage = nullptr;
        ComponentStorageMode mode = ComponentStorageMode::mvcc;
        bool mode_configured = false;
    };

    struct ClassicAccessRegistration {
        ComponentId component = null_component;
        bool writer = false;
    };

    struct ClassicAccessState {
        std::size_t readers = 0;
        std::size_t writers = 0;
    };

    bool has(Entity entity, ComponentId component) const;
    const void* try_get(Entity entity, ComponentId component) const;
    RawPagedSparseArray* storage(ComponentId component);
    const RawPagedSparseArray* storage(ComponentId component) const;
    void require_no_readers() const;
    void require_alive(Entity entity) const;
    void ensure_entity_slot(Entity index);
    void ensure_component_slot(ComponentId componentId);
    ComponentStorageMode resolved_storage_mode(ComponentId componentId) const;
    void recompute_classic_mode_flag();
    void compact_trace_history();
    RawPagedSparseArray& assure_storage(ComponentId componentId, std::size_t component_size, std::size_t component_alignment);

    Timestamp acquire_tsn();
    std::vector<Timestamp> active_transactions_snapshot() const;
    void register_transaction(Timestamp tsn);
    void unregister_transaction(Timestamp tsn);
    void register_reader();
    void unregister_reader();
    void register_snapshot_classic_access();
    void unregister_snapshot_classic_access();

    template <typename... DeclaredComponents>
    std::vector<ClassicAccessRegistration> register_transaction_classic_access() {
        std::vector<ClassicAccessRegistration> registrations;
        if constexpr (sizeof...(DeclaredComponents) == 0) {
            return registrations;
        }

        auto register_component = [&](auto component_constant) {
            using Declared = typename decltype(component_constant)::type;
            using Base = detail::component_base_t<Declared>;
            const ComponentId component = component_id<Base>();
            if (storage_mode<Base>() != ComponentStorageMode::classic) {
                return;
            }

            ensure_component_slot(component);
            if (component >= classic_access_.size()) {
                classic_access_.resize(static_cast<std::size_t>(component) + 1);
            }

            ClassicAccessState& state = classic_access_[component];
            const bool writer = !std::is_const_v<Declared>;
            if (writer) {
                if (state.readers != 0 || state.writers != 0) {
                    throw std::logic_error("classic component storage mode requires exclusive writer access per component");
                }
                ++state.writers;
            } else {
                if (state.writers != 0) {
                    throw std::logic_error("classic component storage mode does not allow readers while a writer is active");
                }
                ++state.readers;
            }

            registrations.push_back(ClassicAccessRegistration{component, writer});
        };

        try {
            (register_component(detail::type_tag<DeclaredComponents>{}), ...);
        } catch (...) {
            unregister_classic_access(registrations);
            throw;
        }

        return registrations;
    }

    void unregister_classic_access(const std::vector<ClassicAccessRegistration>& registrations);

    template <typename T>
    ComponentStorage<T>& assure_storage() {
        const ComponentId id = component_id<T>();
        ensure_component_slot(id);

        ComponentSlot& slot = components_[id];
        if (!slot.mode_configured) {
            slot.mode = ComponentStorageModeTraits<T>::value;
            slot.mode_configured = true;
            recompute_classic_mode_flag();
        }

        RawPagedSparseArray* component = slot.storage;
        if (component == nullptr) {
            component = new ComponentStorage<T>(slot.mode, page_size_);
            slot.storage = component;
        }

        return *static_cast<ComponentStorage<T>*>(component);
    }

    friend class Snapshot;
    template <typename...>
    friend class Transaction;

    std::size_t page_size_;
    Entity next_entity_index_ = 0;
    std::vector<EntityVersion> current_versions_;
    std::vector<std::size_t> entity_dense_indices_;
    std::vector<Entity> alive_entities_;
    std::vector<Entity> free_entities_;
    std::vector<ComponentSlot> components_;
    Timestamp next_tsn_ = 1;
    std::vector<Timestamp> active_transactions_;
    std::size_t active_readers_ = 0;
    bool has_classic_storage_mode_ = false;
    std::vector<ClassicAccessState> classic_access_;
    std::size_t snapshot_classic_readers_ = 0;
    TraceCommitContext trace_commit_context_{};
    std::uint32_t trace_max_history_ = RawPagedSparseArray::max_trace_time();
};

}  // namespace ecs

#include "transaction.hpp"

namespace ecs {

inline Snapshot Registry::snapshot() {
    return Snapshot(*this);
}

template <typename... DeclaredComponents>
inline Transaction<DeclaredComponents...> Registry::transaction() {
    return Transaction<DeclaredComponents...>(*this);
}

}  // namespace ecs
