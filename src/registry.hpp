#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
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
struct ComponentSingletonTraits {
    static constexpr bool value = false;
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

namespace detail {

inline constexpr Entity singleton_entity = make_entity(0, entity_version_mask);

template <typename T>
inline constexpr bool is_singleton_component_v = ComponentSingletonTraits<component_base_t<T>>::value;

template <typename T>
inline constexpr bool is_entity_component_v = !is_singleton_component_v<T>;

}  // namespace detail

class Registry;
class Snapshot;
template <typename... DeclaredComponents>
class Transaction;
class OwningGroupBase;

template <typename... Components>
class OwningGroupStorage;

class OwningGroupBase {
public:
    struct ComponentInfo {
        ComponentId component = null_component;
        RawPagedSparseArray* storage = nullptr;
    };

    virtual ~OwningGroupBase() = default;

    virtual bool owns(ComponentId component) const = 0;
    virtual bool contains(Entity entity) const = 0;
    virtual const void* try_get(ComponentId component, Entity entity) const = 0;
    virtual RawPagedSparseArray* storage(ComponentId component) = 0;
    virtual const RawPagedSparseArray* storage(ComponentId component) const = 0;
    virtual const std::vector<Entity>& entities() const = 0;
    virtual void build_from_registry(Registry& registry) = 0;
    virtual bool promote_if_complete(Registry& registry, Entity entity) = 0;
    virtual bool remove_component(Registry& registry, Entity entity, ComponentId component, TraceCommitContext trace_context) = 0;
    virtual bool erase_entity(Registry& registry, Entity entity, TraceCommitContext trace_context) = 0;
    virtual bool rollback_component_to_timestamp(
        Registry& registry,
        Entity entity,
        ComponentId component,
        Timestamp timestamp,
        TraceCommitContext trace_context) = 0;
    virtual const std::vector<ComponentInfo>& component_infos() const = 0;
    virtual std::size_t component_count() const = 0;
};

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

    template <typename... Components>
    void group();

    template <typename T>
    void set_storage_mode(ComponentStorageMode mode) {
        const ComponentId id = component_id<T>();
        ensure_component_slot(id);
        ComponentSlot& slot = components_[id];
        if (slot.storage != nullptr && slot.mode != mode) {
            throw std::logic_error("cannot change component storage mode after storage has been created");
        }
        if (slot.storage != nullptr && slot.singleton != detail::is_singleton_component_v<T>) {
            throw std::logic_error("cannot change component storage kind after storage has been created");
        }
        if (slot.owner != nullptr && slot.mode != mode) {
            throw std::logic_error("cannot change component storage mode after a group has been created");
        }
        slot.mode = mode;
        slot.mode_configured = true;
        slot.singleton = detail::is_singleton_component_v<T>;
        recompute_classic_mode_flag();
    }

    void set_trace_max_history(Timestamp max_history) {
        if (max_history == 0) {
            throw std::out_of_range("trace max history must be greater than zero");
        }
        trace_max_history_ = max_history;
        trace_max_history_configured_ = true;
    }

    Timestamp trace_max_history() const {
        return trace_max_history_;
    }

    void set_current_trace_time(Timestamp timestamp) {
        if (timestamp < trace_commit_context_.timestamp) {
            throw std::logic_error("trace time must be monotonic");
        }

        require_no_readers();
        if (timestamp > trace_commit_context_.timestamp) {
            capture_preallocated_trace_frames();
        }
        trace_commit_context_.timestamp = timestamp;
        compact_trace_history();
    }

    Timestamp current_trace_time() const {
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
    std::enable_if_t<!detail::is_singleton_component_v<T>, bool> remove(Entity entity) {
        return remove(entity, component_id<T>());
    }

    template <typename T, typename Func>
    std::enable_if_t<!detail::is_singleton_component_v<T>, void> each_trace_change(Entity entity, Func&& func) const {
        const ComponentId id = component_id<T>();
        if (const auto* owner = id < components_.size() ? components_[id].owner : nullptr;
            owner != nullptr && owner->contains(entity)) {
            if (const auto* raw = static_cast<const ComponentStorage<T>*>(owner->storage(id))) {
                raw->for_each_trace_change(entity, std::forward<Func>(func));
            }
        } else if (const auto* raw = static_cast<const ComponentStorage<T>*>(storage(id))) {
            raw->for_each_trace_change(entity, std::forward<Func>(func));
        }
    }

    template <typename T, typename Func>
    std::enable_if_t<detail::is_singleton_component_v<T>, void> each_trace_change(Func&& func) const {
        const ComponentId id = component_id<T>();
        if (const auto* raw = static_cast<const ComponentStorage<T>*>(storage(id))) {
            raw->for_each_trace_change(detail::singleton_entity, std::forward<Func>(func));
        }
    }

    template <typename T>
    std::enable_if_t<!detail::is_singleton_component_v<T>, bool> rollback_to_timestamp(Entity entity, Timestamp timestamp) {
        require_no_readers();
        const ComponentId id = component_id<T>();
        if (!is_trace_storage_mode(storage_mode<T>())) {
            throw std::logic_error("trace rollback requires trace component storage mode");
        }
        if (id >= components_.size()) {
            return false;
        }
        if (OwningGroupBase* owner = components_[id].owner; owner != nullptr && owner->contains(entity)) {
            return owner->rollback_component_to_timestamp(*this, entity, id, timestamp, trace_commit_context_);
        }
        if (components_[id].storage == nullptr) {
            return false;
        }
        return static_cast<ComponentStorage<T>*>(components_[id].storage)->rollback_to_trace_timestamp(entity, timestamp, trace_commit_context_);
    }

    template <typename T>
    std::enable_if_t<detail::is_singleton_component_v<T>, bool> rollback_to_timestamp(Timestamp timestamp) {
        require_no_readers();
        const ComponentId id = component_id<T>();
        if (!is_trace_storage_mode(storage_mode<T>())) {
            throw std::logic_error("trace rollback requires trace component storage mode");
        }
        auto& raw = assure_singleton_storage<T>();
        return raw.rollback_to_trace_timestamp(detail::singleton_entity, timestamp, trace_commit_context_);
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
        bool singleton = false;
        OwningGroupBase* owner = nullptr;
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
    const void* try_get_singleton(ComponentId component) const;
    const void* try_get_standalone(Entity entity, ComponentId component) const;
    RawPagedSparseArray* storage(ComponentId component);
    const RawPagedSparseArray* storage(ComponentId component) const;
    RawPagedSparseArray* storage_for_entity(Entity entity, ComponentId component);
    const RawPagedSparseArray* storage_for_entity(Entity entity, ComponentId component) const;
    bool component_belongs_to_group(ComponentId component) const;
    void refresh_groups_for_entities(const std::vector<Entity>& entities);
    void require_no_readers() const;
    void require_alive(Entity entity) const;
    void ensure_entity_slot(Entity index);
    void ensure_component_slot(ComponentId componentId);
    ComponentStorageMode resolved_storage_mode(ComponentId componentId) const;
    void recompute_classic_mode_flag();
    void compact_trace_history();
    void capture_preallocated_trace_frames();
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
            if (!is_direct_write_storage_mode(storage_mode<Base>())) {
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
                    throw std::logic_error("direct-write component storage mode requires exclusive writer access per component");
                }
                ++state.writers;
            } else {
                if (state.writers != 0) {
                    throw std::logic_error("direct-write component storage mode does not allow readers while a writer is active");
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
        static_assert(!detail::is_singleton_component_v<T>, "singleton components use no-entity singleton storage");
        const ComponentId id = component_id<T>();
        ensure_component_slot(id);

        ComponentSlot& slot = components_[id];
        if (!slot.mode_configured) {
            slot.mode = ComponentStorageModeTraits<T>::value;
            slot.mode_configured = true;
            slot.singleton = false;
            recompute_classic_mode_flag();
        }

        if (slot.mode == ComponentStorageMode::trace_preallocate && !trace_max_history_configured_) {
            throw std::logic_error("trace_preallocate storage requires set_trace_max_history before storage creation");
        }

        RawPagedSparseArray* component = slot.storage;
        if (component == nullptr) {
            component = new ComponentStorage<T>(slot.mode, page_size_, trace_max_history_);
            slot.storage = component;
        }

        return *static_cast<ComponentStorage<T>*>(component);
    }

    template <typename T>
    ComponentStorage<T>& assure_singleton_storage() {
        static_assert(detail::is_singleton_component_v<T>, "non-singleton components use entity storage");
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        static_assert(std::is_default_constructible_v<T>, "singleton components must be default constructible");
        static_assert(std::tuple_size_v<typename ComponentIndices<T>::type> == 0,
                      "singleton components cannot declare component indexes");

        const ComponentId id = component_id<T>();
        ensure_component_slot(id);

        ComponentSlot& slot = components_[id];
        if (!slot.mode_configured) {
            slot.mode = ComponentStorageModeTraits<T>::value;
            slot.mode_configured = true;
            slot.singleton = true;
            recompute_classic_mode_flag();
        }

        if (!slot.singleton) {
            throw std::logic_error("component storage was already created as entity storage");
        }

        if (slot.mode == ComponentStorageMode::trace_preallocate && !trace_max_history_configured_) {
            throw std::logic_error("trace_preallocate storage requires set_trace_max_history before storage creation");
        }

        RawPagedSparseArray* component = slot.storage;
        if (component == nullptr) {
            component = new ComponentStorage<T>(slot.mode, page_size_, trace_max_history_);
            slot.storage = component;

            auto& typed = *static_cast<ComponentStorage<T>*>(component);
            T value{};
            auto pending = typed.stage_value(detail::singleton_entity, 0, value);
            typed.commit_staged(detail::singleton_entity, pending, 0, trace_commit_context_);
        }

        return *static_cast<ComponentStorage<T>*>(component);
    }

    template <typename T>
    ComponentStorage<T>& assure_storage_for_entity(Entity entity) {
        const ComponentId id = component_id<T>();
        ensure_component_slot(id);
        if (OwningGroupBase* owner = components_[id].owner; owner != nullptr && owner->contains(entity)) {
            return *static_cast<ComponentStorage<T>*>(owner->storage(id));
        }
        return assure_storage<T>();
    }

    friend class Snapshot;
    template <typename...>
    friend class Transaction;
    template <typename...>
    friend class OwningGroupStorage;

    std::size_t page_size_;
    Entity next_entity_index_ = 0;
    std::vector<EntityVersion> current_versions_;
    std::vector<std::size_t> entity_dense_indices_;
    std::vector<Entity> alive_entities_;
    std::vector<Entity> free_entities_;
    std::vector<ComponentSlot> components_;
    std::vector<std::unique_ptr<OwningGroupBase>> groups_;
    Timestamp next_tsn_ = 1;
    std::vector<Timestamp> active_transactions_;
    std::size_t active_readers_ = 0;
    bool has_classic_storage_mode_ = false;
    std::vector<ClassicAccessState> classic_access_;
    std::size_t snapshot_classic_readers_ = 0;
    TraceCommitContext trace_commit_context_{};
    Timestamp trace_max_history_ = RawPagedSparseArray::max_trace_time();
    bool trace_max_history_configured_ = false;
};

template <typename... Components>
class OwningGroupStorage final : public OwningGroupBase {
public:
    OwningGroupStorage(ComponentStorageMode mode, std::size_t page_size, std::size_t trace_max_history)
        : storages_(ComponentStorage<Components>(mode, page_size, trace_max_history)...),
          component_infos_(make_component_infos(std::index_sequence_for<Components...>{})) {}

    bool owns(ComponentId component) const override {
        return ((component == component_id<Components>()) || ...);
    }

    bool contains(Entity entity) const override {
        return std::get<0>(storages_).contains(entity);
    }

    const void* try_get(ComponentId component, Entity entity) const override {
        return try_get_impl(component, entity, std::index_sequence_for<Components...>{});
    }

    RawPagedSparseArray* storage(ComponentId component) override {
        return storage_impl(component, std::index_sequence_for<Components...>{});
    }

    const RawPagedSparseArray* storage(ComponentId component) const override {
        return storage_impl(component, std::index_sequence_for<Components...>{});
    }

    const std::vector<Entity>& entities() const override {
        return std::get<0>(storages_).entities();
    }

    void build_from_registry(Registry& registry) override {
        for (const Entity entity : registry.entities()) {
            (void)promote_if_complete(registry, entity);
        }
    }

    bool promote_if_complete(Registry& registry, Entity entity) override {
        if (contains(entity) || !all_standalone_present(registry, entity)) {
            return false;
        }

        promote_entity(registry, entity, std::index_sequence_for<Components...>{});
        return true;
    }

    bool remove_component(Registry& registry, Entity entity, ComponentId component, TraceCommitContext trace_context) override {
        if (!contains(entity) || !owns(component)) {
            return false;
        }

        migrate_except_component(registry, entity, component, trace_context, std::index_sequence_for<Components...>{});
        erase_all(entity, trace_context, std::index_sequence_for<Components...>{});
        return true;
    }

    bool erase_entity(Registry& registry, Entity entity, TraceCommitContext trace_context) override {
        (void)registry;
        if (!contains(entity)) {
            return false;
        }
        erase_all(entity, trace_context, std::index_sequence_for<Components...>{});
        return true;
    }

    bool rollback_component_to_timestamp(
        Registry& registry,
        Entity entity,
        ComponentId component,
        Timestamp timestamp,
        TraceCommitContext trace_context) override {

        if (!contains(entity) || !owns(component)) {
            return false;
        }

        const bool changed = rollback_component_impl(
            registry,
            entity,
            component,
            timestamp,
            trace_context,
            std::index_sequence_for<Components...>{});

        if (!contains_all(entity, std::index_sequence_for<Components...>{})) {
            migrate_all_present_to_standalone(registry, entity, trace_context, std::index_sequence_for<Components...>{});
            erase_all(entity, trace_context, std::index_sequence_for<Components...>{});
        }

        return changed;
    }

    const std::vector<ComponentInfo>& component_infos() const override {
        return component_infos_;
    }

    std::size_t component_count() const override {
        return component_infos_.size();
    }

private:
    template <std::size_t... Indices>
    std::vector<ComponentInfo> make_component_infos(std::index_sequence<Indices...>) {
        std::vector<ComponentInfo> infos;
        infos.reserve(sizeof...(Components));
        (infos.push_back(ComponentInfo{
             component_id<std::tuple_element_t<Indices, std::tuple<Components...>>>(),
             const_cast<ComponentStorage<std::tuple_element_t<Indices, std::tuple<Components...>>>*>(
                 &std::get<Indices>(storages_))}),
         ...);
        return infos;
    }

    template <std::size_t... Indices>
    const void* try_get_impl(ComponentId component, Entity entity, std::index_sequence<Indices...>) const {
        const void* result = nullptr;
        (try_get_one<Indices>(component, entity, result), ...);
        return result;
    }

    template <std::size_t Index>
    void try_get_one(ComponentId component, Entity entity, const void*& result) const {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        if (result == nullptr && component == component_id<Component>()) {
            result = std::get<Index>(storages_).try_get(entity);
        }
    }

    template <std::size_t... Indices>
    RawPagedSparseArray* storage_impl(ComponentId component, std::index_sequence<Indices...>) const {
        RawPagedSparseArray* result = nullptr;
        (storage_one<Indices>(component, result), ...);
        return result;
    }

    template <std::size_t Index>
    void storage_one(ComponentId component, RawPagedSparseArray*& result) const {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        if (result == nullptr && component == component_id<Component>()) {
            result = const_cast<ComponentStorage<Component>*>(&std::get<Index>(storages_));
        }
    }

    bool all_standalone_present(Registry& registry, Entity entity) const {
        return ((registry.try_get_standalone(entity, component_id<Components>()) != nullptr) && ...);
    }

    template <std::size_t... Indices>
    void promote_entity(Registry& registry, Entity entity, std::index_sequence<Indices...>) {
        (promote_one<Indices>(registry, entity), ...);
    }

    template <std::size_t Index>
    void promote_one(Registry& registry, Entity entity) {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        const Component* value = static_cast<const Component*>(registry.try_get_standalone(entity, component_id<Component>()));
        auto& destination = std::get<Index>(storages_);
        auto pending = destination.stage_value(entity, 0, *value);
        destination.commit_staged(entity, pending, 0, registry.trace_commit_context_);
        registry.assure_storage<Component>().erase(entity, registry.trace_commit_context_);
    }

    template <std::size_t... Indices>
    void migrate_except_component(
        Registry& registry,
        Entity entity,
        ComponentId removed_component,
        TraceCommitContext trace_context,
        std::index_sequence<Indices...>) {
        (migrate_except_one<Indices>(registry, entity, removed_component, trace_context), ...);
    }

    template <std::size_t Index>
    void migrate_except_one(
        Registry& registry,
        Entity entity,
        ComponentId removed_component,
        TraceCommitContext trace_context) {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        if (component_id<Component>() == removed_component) {
            return;
        }
        if (const Component* value = std::get<Index>(storages_).try_get(entity)) {
            auto& destination = registry.assure_storage<Component>();
            auto pending = destination.stage_value(entity, 0, *value);
            destination.commit_staged(entity, pending, 0, trace_context);
        }
    }

    template <std::size_t... Indices>
    void migrate_all_present_to_standalone(
        Registry& registry,
        Entity entity,
        TraceCommitContext trace_context,
        std::index_sequence<Indices...>) {
        (migrate_all_present_one<Indices>(registry, entity, trace_context), ...);
    }

    template <std::size_t Index>
    void migrate_all_present_one(Registry& registry, Entity entity, TraceCommitContext trace_context) {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        if (const Component* value = std::get<Index>(storages_).try_get(entity)) {
            auto& destination = registry.assure_storage<Component>();
            auto pending = destination.stage_value(entity, 0, *value);
            destination.commit_staged(entity, pending, 0, trace_context);
        }
    }

    template <std::size_t... Indices>
    void erase_all(Entity entity, TraceCommitContext trace_context, std::index_sequence<Indices...>) {
        (std::get<Indices>(storages_).erase(entity, trace_context), ...);
    }

    template <std::size_t... Indices>
    bool contains_all(Entity entity, std::index_sequence<Indices...>) const {
        return ((std::get<Indices>(storages_).try_get(entity) != nullptr) && ...);
    }

    template <std::size_t... Indices>
    bool rollback_component_impl(
        Registry& registry,
        Entity entity,
        ComponentId component,
        Timestamp timestamp,
        TraceCommitContext trace_context,
        std::index_sequence<Indices...>) {
        bool changed = false;
        (rollback_component_one<Indices>(registry, entity, component, timestamp, trace_context, changed), ...);
        return changed;
    }

    template <std::size_t Index>
    void rollback_component_one(
        Registry& registry,
        Entity entity,
        ComponentId component,
        Timestamp timestamp,
        TraceCommitContext trace_context,
        bool& changed) {
        using Component = std::tuple_element_t<Index, std::tuple<Components...>>;
        (void)registry;
        if (component == component_id<Component>()) {
            changed = std::get<Index>(storages_).rollback_to_trace_timestamp(entity, timestamp, trace_context);
        }
    }

    std::tuple<ComponentStorage<Components>...> storages_;
    std::vector<ComponentInfo> component_infos_;
};

template <typename... Components>
inline void Registry::group() {
    static_assert(sizeof...(Components) >= 2, "groups require at least two components");
    static_assert(detail::unique_component_types<Components...>::value,
                  "groups cannot contain duplicate component types");

    std::array<ComponentId, sizeof...(Components)> ids{component_id<Components>()...};
    ComponentStorageMode mode = ComponentStorageModeTraits<std::tuple_element_t<0, std::tuple<Components...>>>::value;

    std::size_t index = 0;
    auto resolve_mode = [&](auto component_tag) {
        using Component = typename decltype(component_tag)::type;
        const ComponentId id = ids[index++];
        ensure_component_slot(id);
        ComponentSlot& slot = components_[id];
        if (slot.owner != nullptr) {
            throw std::logic_error("component already belongs to an owning group");
        }

        const ComponentStorageMode resolved = slot.mode_configured ? slot.mode : ComponentStorageModeTraits<Component>::value;
        if (id == ids.front()) {
            mode = resolved;
        } else if (resolved != mode) {
            throw std::logic_error("grouped components must share the same storage mode");
        }
    };
    (resolve_mode(detail::type_tag<Components>{}), ...);

    auto group_storage = std::make_unique<OwningGroupStorage<Components...>>(mode, page_size_, trace_max_history_);
    OwningGroupBase* raw = group_storage.get();
    for (const ComponentId id : ids) {
        ComponentSlot& slot = components_[id];
        slot.mode = mode;
        slot.mode_configured = true;
        slot.owner = raw;
    }
    groups_.push_back(std::move(group_storage));
    raw->build_from_registry(*this);
    recompute_classic_mode_flag();
}

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
