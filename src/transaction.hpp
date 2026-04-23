#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "profiler.hpp"
#include "view_traits.hpp"

namespace ecs {

namespace detail {

template <typename... Args>
struct first_argument_is_entity : std::false_type {};

template <typename First, typename... Rest>
struct first_argument_is_entity<First, Rest...>
    : std::is_same<std::decay_t<First>, Entity> {};

template <typename... Components>
inline constexpr std::size_t entity_component_count_v =
    ((!is_singleton_component_v<Components> ? std::size_t{1} : std::size_t{0}) + ... + std::size_t{0});

}  // namespace detail

template <typename T, typename TransactionType>
class TransactionStorageView;

template <typename TransactionType, typename Expression, typename... Components>
class FilteredTransactionView;

template <typename TransactionType, typename... Components>
class TransactionView;

template <typename... DeclaredComponents>
class Transaction;

enum class QueryAccessKind {
    anchor_scan,
    index_seek,
    grouped_fetch,
    sparse_lookup,
    singleton_read,
};

enum class QuerySourceKind {
    standalone_storage,
    owning_group,
    component_index,
};

struct QueryAccessStep {
    ComponentId component = null_component;
    std::size_t component_index = 0;
    std::size_t visible_rows = 0;
    QueryAccessKind access = QueryAccessKind::sparse_lookup;
    bool uses_component_index = false;
    std::string_view component_name{};
};

struct QuerySourceCandidate {
    QuerySourceKind source = QuerySourceKind::standalone_storage;
    ComponentId component = null_component;
    std::size_t component_index = 0;
    std::size_t candidate_rows = 0;
    std::size_t grouped_component_count = 0;
    bool chosen = false;
    std::string_view component_name{};
};

struct QueryExplain {
    bool empty = true;
    ComponentId anchor_component = null_component;
    std::string_view anchor_component_name{};
    std::size_t anchor_component_index = 0;
    std::size_t candidate_rows = 0;
    std::size_t estimated_entity_lookups = 0;
    bool uses_component_indexes = false;
    std::vector<QueryAccessStep> steps;
    std::vector<QuerySourceCandidate> candidates;
};

template <typename T>
bool compare_values(const T& lhs, PredicateOperator op, const T& rhs) {
    switch (op) {
        case PredicateOperator::eq:
            return lhs == rhs;
        case PredicateOperator::ne:
            return lhs != rhs;
        case PredicateOperator::gt:
            return lhs > rhs;
        case PredicateOperator::gte:
            return lhs >= rhs;
        case PredicateOperator::lt:
            return lhs < rhs;
        case PredicateOperator::lte:
            return lhs <= rhs;
    }

    return false;
}

template <auto Member, typename Value>
struct ViewPredicate {
    using pointer_traits = detail::member_pointer_traits<decltype(Member)>;
    using component_type = typename pointer_traits::component_type;
    using member_type = typename pointer_traits::member_type;
    using value_type = Value;

    static constexpr auto member = Member;

    PredicateOperator op;
    value_type value;

    bool matches(const component_type& component) const {
        return compare_values<member_type>(component.*Member, op, value);
    }
};

struct TruePredicate {
    bool matches(Entity, const Snapshot&) const {
        return true;
    }
};

template <typename Left, typename Right>
struct AndPredicate {
    using left_type = Left;
    using right_type = Right;
    Left left;
    Right right;
};

template <typename Left, typename Right>
struct OrPredicate {
    using left_type = Left;
    using right_type = Right;
    Left left;
    Right right;
};

template <typename T>
struct is_and_predicate : std::false_type {};

template <typename Left, typename Right>
struct is_and_predicate<AndPredicate<Left, Right>> : std::true_type {};

template <typename T>
struct is_or_predicate : std::false_type {};

template <typename Left, typename Right>
struct is_or_predicate<OrPredicate<Left, Right>> : std::true_type {};

class Snapshot {
public:
    explicit Snapshot(Registry& registry)
        : Snapshot(registry, true) {}

    ~Snapshot() {
        close();
    }

    Snapshot(Snapshot&& other) noexcept
        : registry_(other.registry_),
          max_visible_tsn_(other.max_visible_tsn_),
          active_at_open_(std::move(other.active_at_open_)),
          holds_snapshot_classic_access_(other.holds_snapshot_classic_access_) {
        other.registry_ = nullptr;
        other.max_visible_tsn_ = 0;
        other.holds_snapshot_classic_access_ = false;
    }

    Snapshot& operator=(Snapshot&& other) noexcept {
        if (this != &other) {
            close();
            registry_ = other.registry_;
            max_visible_tsn_ = other.max_visible_tsn_;
            active_at_open_ = std::move(other.active_at_open_);
            holds_snapshot_classic_access_ = other.holds_snapshot_classic_access_;
            other.registry_ = nullptr;
            other.max_visible_tsn_ = 0;
            other.holds_snapshot_classic_access_ = false;
        }
        return *this;
    }

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    bool has(Entity entity, ComponentId component) const {
        return try_get(entity, component) != nullptr;
    }

    template <typename T, typename = std::enable_if_t<!detail::is_singleton_component_v<T>>>
    bool has(Entity entity) const {
        return try_get<T>(entity) != nullptr;
    }

    template <typename T, typename = std::enable_if_t<detail::is_singleton_component_v<T>>, typename = void>
    bool has() const {
        return try_get<T>() != nullptr;
    }

    const void* try_get(Entity entity, ComponentId component) const {
        ECS_PROFILE_ZONE("Transaction::try_get");
        require_open();
        if (!registry_->alive(entity)) {
            return nullptr;
        }

        const RawPagedSparseArray* raw = registry_->storage_for_entity(entity, component);
        return raw == nullptr ? nullptr : raw->try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, 0);
    }

    template <typename T, typename = std::enable_if_t<!detail::is_singleton_component_v<T>>>
    const T* try_get(Entity entity) const {
        return static_cast<const T*>(try_get(entity, component_id<T>()));
    }

    template <typename T, typename = std::enable_if_t<detail::is_singleton_component_v<T>>, typename = void>
    const T* try_get() const {
        require_open();
        auto& storage = registry_->template assure_singleton_storage<T>();
        return storage.try_get_visible(detail::singleton_entity, max_visible_tsn_, active_at_open_, 0);
    }

    template <typename T, typename = std::enable_if_t<!detail::is_singleton_component_v<T>>>
    const T& get(Entity entity) const {
        const T* component = try_get<T>(entity);
        if (component == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return *component;
    }

    template <typename T, typename = std::enable_if_t<detail::is_singleton_component_v<T>>, typename = void>
    const T& get() const {
        return *try_get<T>();
    }

    std::size_t entity_count() const {
        require_open();
        return registry_->entity_count();
    }

    const RawPagedSparseArray* raw_storage(ComponentId component) const {
        require_open();
        return registry_->storage(component);
    }

    const std::vector<Entity>& entities() const {
        require_open();
        return registry_->entities();
    }

    bool has_stable_visibility() const {
        require_open();
        return active_at_open_.empty();
    }

    bool has_pending_writes() const {
        return false;
    }

protected:
    Snapshot(Registry& registry, bool register_snapshot_classic_access)
        : registry_(&registry),
          max_visible_tsn_(registry.next_tsn_ - 1),
          active_at_open_(registry.active_transactions_snapshot()) {
        registry_->register_reader();
        try {
            if (register_snapshot_classic_access) {
                registry_->register_snapshot_classic_access();
                holds_snapshot_classic_access_ = true;
            }
        } catch (...) {
            registry_->unregister_reader();
            registry_ = nullptr;
            max_visible_tsn_ = 0;
            active_at_open_.clear();
            holds_snapshot_classic_access_ = false;
            throw;
        }
    }

    void require_open() const {
        if (registry_ == nullptr) {
            throw std::logic_error("snapshot is no longer open");
        }
    }

    void close() {
        if (registry_ != nullptr) {
            if (holds_snapshot_classic_access_) {
                registry_->unregister_snapshot_classic_access();
                holds_snapshot_classic_access_ = false;
            }
            registry_->unregister_reader();
            registry_ = nullptr;
            max_visible_tsn_ = 0;
            active_at_open_.clear();
        }
    }

    Registry* registry_ = nullptr;
    Timestamp max_visible_tsn_ = 0;
    std::vector<Timestamp> active_at_open_;
    bool holds_snapshot_classic_access_ = false;
};

template <typename... DeclaredComponents>
class Transaction : public Snapshot {
    static_assert(sizeof...(DeclaredComponents) > 0,
                  "transactions must declare at least one component access");
    static_assert(detail::unique_component_types<DeclaredComponents...>::value,
                  "transactions cannot declare duplicate component types");

public:
    explicit Transaction(Registry& registry)
        : Snapshot(registry, false),
          tsn_(registry.acquire_tsn()) {
        max_visible_tsn_ = tsn_ - 1;
        active_at_open_ = registry.active_transactions_snapshot();
        registry_->register_transaction(tsn_);
        try {
            classic_accesses_ = registry_->template register_transaction_classic_access<DeclaredComponents...>();
        } catch (...) {
            registry_->unregister_transaction(tsn_);
            tsn_ = 0;
            close();
            throw;
        }
    }

    ~Transaction() {
        if (registry_ == nullptr) {
            return;
        }

        if (rollback_supported()) {
            finalize_writes(false);
        } else {
            finalize_writes(true);
        }
    }

    Transaction(Transaction&& other) noexcept
        : Snapshot(std::move(other)),
          tsn_(other.tsn_),
          pending_write_stores_(std::move(other.pending_write_stores_)),
          classic_accesses_(std::move(other.classic_accesses_)),
          pending_write_count_(other.pending_write_count_),
          pending_write_reserve_hint_(other.pending_write_reserve_hint_) {
        other.tsn_ = 0;
        other.pending_write_count_ = 0;
        other.pending_write_reserve_hint_ = 0;
    }

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            if (registry_ != nullptr) {
                if (rollback_supported()) {
                    finalize_writes(false);
                } else {
                    finalize_writes(true);
                }
            }
            Snapshot::operator=(std::move(other));
            tsn_ = other.tsn_;
            pending_write_stores_ = std::move(other.pending_write_stores_);
            classic_accesses_ = std::move(other.classic_accesses_);
            pending_write_count_ = other.pending_write_count_;
            pending_write_reserve_hint_ = other.pending_write_reserve_hint_;
            other.tsn_ = 0;
            other.pending_write_count_ = 0;
            other.pending_write_reserve_hint_ = 0;
        }
        return *this;
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool has(Entity entity, ComponentId component) const {
        return try_get(entity, component) != nullptr;
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      !detail::is_singleton_component_v<T>>>
    bool has(Entity entity) const {
        return try_get<T>(entity) != nullptr;
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      detail::is_singleton_component_v<T>>, typename = void>
    bool has() const {
        return try_get<T>() != nullptr;
    }

    const void* try_get(Entity entity, ComponentId component) const {
        require_open();
        if (!registry_->alive(entity)) {
            return nullptr;
        }

        const RawPagedSparseArray* raw = registry_->storage_for_entity(entity, component);
        return raw == nullptr ? nullptr : raw->try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, tsn_);
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      !detail::is_singleton_component_v<T>>>
    const T* try_get(Entity entity) const {
        return static_cast<const T*>(try_get(entity, component_id<T>()));
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      detail::is_singleton_component_v<T>>, typename = void>
    const T* try_get() const {
        require_open();
        auto& storage = registry_->template assure_singleton_storage<T>();
        return storage.try_get_visible(detail::singleton_entity, max_visible_tsn_, active_at_open_, tsn_);
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      !detail::is_singleton_component_v<T>>>
    const T& get(Entity entity) const {
        const T* component = try_get<T>(entity);
        if (component == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return *component;
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      detail::is_singleton_component_v<T>>, typename = void>
    const T& get() const {
        return *try_get<T>();
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...> &&
                                                      !detail::is_singleton_component_v<T>>>
    TransactionStorageView<T, Transaction<DeclaredComponents...>> storage() {
        return TransactionStorageView<T, Transaction<DeclaredComponents...>>(*this);
    }

    template <typename... Components,
              typename = std::enable_if_t<((detail::readable_component_v<detail::component_base_t<Components>, DeclaredComponents...>) && ...)>>
    TransactionView<Transaction<DeclaredComponents...>, Components...> view() {
        return TransactionView<Transaction<DeclaredComponents...>, Components...>(*this);
    }

    template <typename T>
    std::size_t visible_component_size() const {
        using Base = detail::component_base_t<T>;
        require_open();

        if constexpr (detail::is_singleton_component_v<Base>) {
            return try_get<Base>() == nullptr ? 0 : 1;
        } else {
            if (has_stable_visibility() && !has_pending_writes()) {
                std::size_t count = 0;
                if (const RawPagedSparseArray* storage = raw_storage(component_id<Base>()); storage != nullptr) {
                    count += storage->size();
                }
                if (const OwningGroupBase* owner = owning_group(component_id<Base>()); owner != nullptr) {
                    count += owner->entities().size();
                }
                return count;
            }

            std::size_t count = 0;
            for (const Entity entity : entities()) {
                if (try_get<Base>(entity) != nullptr) {
                    ++count;
                }
            }
            return count;
        }
    }

    void reserve_pending_writes(std::size_t expected_writes) {
        require_open();
        if (expected_writes == 0) {
            return;
        }

        pending_write_reserve_hint_ = std::max(pending_write_reserve_hint_, expected_writes);
    }

    const void* try_get_visible_dense(const RawPagedSparseArray& storage, std::size_t dense_index) const {
        ECS_PROFILE_ZONE("Transaction::try_get_visible_dense");
        return storage.try_get_visible_dense_raw(dense_index, max_visible_tsn_, active_at_open_, tsn_);
    }

    const void* try_get_visible_from_storage(const RawPagedSparseArray& storage, Entity entity) const {
        ECS_PROFILE_ZONE("Transaction::try_get_from_storage");
        return storage.try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, tsn_);
    }

    template <typename T, typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...> &&
                                                      !detail::is_singleton_component_v<T>>>
    T* write(Entity entity) {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        require_open();
        registry_->require_alive(entity);

        if (auto* pending = find_pending<T>(entity)) {
            return pending->storage->staged_ptr(pending->pending, tsn_);
        }

        auto& storage = registry_->template assure_storage_for_entity<T>(entity);
        auto pending = storage.stage_write(entity, tsn_, active_at_open_, max_visible_tsn_);
        T* staged = storage.staged_ptr(pending, tsn_);
        add_pending_write(entity, &storage, pending);
        return staged;
    }

    template <typename T,
              typename... Args,
              typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...> &&
                                          !detail::is_singleton_component_v<T>>>
    T* write(Entity entity, Args&&... args) {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        require_open();
        registry_->require_alive(entity);

        T value{std::forward<Args>(args)...};
        if (auto* pending = find_pending<T>(entity)) {
            T* staged = pending->storage->staged_ptr(pending->pending, tsn_);
            std::memcpy(staged, &value, sizeof(T));
            return staged;
        }

        auto& storage = registry_->template assure_storage_for_entity<T>(entity);
        auto pending = storage.stage_value(entity, tsn_, value);
        T* staged = storage.staged_ptr(pending, tsn_);
        add_pending_write(entity, &storage, pending);
        return staged;
    }

    template <typename T, typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...> &&
                                                      detail::is_singleton_component_v<T>>>
    T* write() {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        require_open();

        if (auto* pending = find_pending_singleton<T>()) {
            return pending->storage->staged_ptr(pending->pending, tsn_);
        }

        auto& storage = registry_->template assure_singleton_storage<T>();
        auto pending = storage.stage_write(detail::singleton_entity, tsn_, active_at_open_, max_visible_tsn_);
        T* staged = storage.staged_ptr(pending, tsn_);
        add_pending_write(detail::singleton_entity, &storage, pending, false);
        return staged;
    }

    template <typename T,
              typename... Args,
              typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...> &&
                                          detail::is_singleton_component_v<T> &&
                                          (sizeof...(Args) > 0) &&
                                          !detail::first_argument_is_entity<Args...>::value>,
              typename = void>
    T* write(Args&&... args) {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        require_open();

        T value{std::forward<Args>(args)...};
        if (auto* pending = find_pending_singleton<T>()) {
            T* staged = pending->storage->staged_ptr(pending->pending, tsn_);
            std::memcpy(staged, &value, sizeof(T));
            return staged;
        }

        auto& storage = registry_->template assure_singleton_storage<T>();
        auto pending = storage.stage_value(detail::singleton_entity, tsn_, value);
        T* staged = storage.staged_ptr(pending, tsn_);
        add_pending_write(detail::singleton_entity, &storage, pending, false);
        return staged;
    }

    void commit() {
        require_open();
        finalize_writes(true);
    }

    void rollback() {
        if (registry_ == nullptr) {
            return;
        }

        if (!rollback_supported()) {
            throw std::logic_error("direct-write component storage does not support transaction rollback");
        }

        finalize_writes(false);
    }

private:
    template <typename T>
    struct PendingWriteEntry {
        Entity entity = null_entity;
        ComponentStorage<T>* storage;
        typename ComponentStorage<T>::PendingWrite pending;
        bool requires_alive = true;
    };

    template <typename T>
    struct PendingWriteStore {
        using component_type = T;

        std::vector<PendingWriteEntry<T>> writes;
        std::unordered_map<Entity, std::size_t> index_by_entity;

        void reserve(std::size_t expected_writes) {
            writes.reserve(expected_writes);
            index_by_entity.reserve(expected_writes);
        }

        void clear() {
            writes.clear();
            index_by_entity.clear();
        }
    };

    template <typename T>
    PendingWriteStore<T>& pending_store() {
        return std::get<PendingWriteStore<T>>(pending_write_stores_);
    }

    template <typename T>
    const PendingWriteStore<T>& pending_store() const {
        return std::get<PendingWriteStore<T>>(pending_write_stores_);
    }

    template <typename T>
    void ensure_pending_store_capacity() {
        auto& store = pending_store<T>();
        if (pending_write_reserve_hint_ != 0 && store.writes.empty() && store.index_by_entity.empty()) {
            store.reserve(pending_write_reserve_hint_);
        }
    }

    template <typename T>
    PendingWriteEntry<T>* find_pending(Entity entity) {
        ensure_pending_store_capacity<T>();
        auto& store = pending_store<T>();
        const auto it = store.index_by_entity.find(entity);
        return it == store.index_by_entity.end() ? nullptr : &store.writes[it->second];
    }

    template <typename T>
    void add_pending_write(Entity entity,
                           ComponentStorage<T>* storage,
                           typename ComponentStorage<T>::PendingWrite pending,
                           bool requires_alive = true) {
        ensure_pending_store_capacity<T>();
        auto& store = pending_store<T>();
        store.index_by_entity.emplace(entity, store.writes.size());
        store.writes.push_back(PendingWriteEntry<T>{entity, storage, pending, requires_alive});
        ++pending_write_count_;
    }

    template <typename Store, typename Func>
    static void for_each_store_entry(Store& store, Func& func) {
        for (auto& entry : store.writes) {
            func(entry);
        }
    }

    template <typename Func>
    void for_each_pending_entry(Func&& func) {
        auto&& callback = func;
        std::apply([&](auto&... stores) { (for_each_store_entry(stores, callback), ...); }, pending_write_stores_);
    }

    template <typename Store, typename Func>
    static void for_each_store_entry_const(const Store& store, Func& func) {
        for (const auto& entry : store.writes) {
            func(entry);
        }
    }

    template <typename Func>
    void for_each_pending_entry(Func&& func) const {
        auto&& callback = func;
        std::apply([&](const auto&... stores) { (for_each_store_entry_const(stores, callback), ...); }, pending_write_stores_);
    }

    template <typename T>
    PendingWriteEntry<T>* find_pending_singleton() {
        return find_pending<T>(detail::singleton_entity);
    }

    bool rollback_supported() const {
        bool supported = true;
        for_each_pending_entry([&](const auto& entry) {
            if (is_direct_write_storage_mode(entry.storage->storage_mode())) {
                supported = false;
            }
        });
        return supported;
    }

    void finalize_writes(bool commit_writes) {
        require_open();

        std::vector<Entity> touched_entities;
        touched_entities.reserve(pending_write_count_);

        for_each_pending_entry([&](auto& entry) {
            if (entry.requires_alive) {
                registry_->require_alive(entry.entity);
            }
        });

        for_each_pending_entry([&](auto& entry) {
            if (commit_writes) {
                entry.storage->commit_staged(entry.entity, entry.pending, tsn_, registry_->trace_commit_context_);
                if (entry.requires_alive) {
                    touched_entities.push_back(entry.entity);
                }
            } else {
                entry.storage->rollback_staged(entry.entity, entry.pending, tsn_);
            }
        });

        if (commit_writes && !touched_entities.empty()) {
            std::sort(touched_entities.begin(), touched_entities.end());
            touched_entities.erase(std::unique(touched_entities.begin(), touched_entities.end()), touched_entities.end());
            registry_->refresh_groups_for_entities(touched_entities);
        }

        std::apply([](auto&... stores) { (stores.clear(), ...); }, pending_write_stores_);
        pending_write_count_ = 0;
        registry_->unregister_classic_access(classic_accesses_);
        classic_accesses_.clear();
        if (tsn_ != 0) {
            registry_->unregister_transaction(tsn_);
        }
        tsn_ = 0;
        close();
    }

public:
    template <typename T>
    const ComponentStorage<T>* typed_raw_storage() const {
        return static_cast<const ComponentStorage<T>*>(raw_storage(component_id<T>()));
    }

    bool component_belongs_to_group(ComponentId component) const {
        return registry_ != nullptr && registry_->component_belongs_to_group(component);
    }

    const OwningGroupBase* owning_group(ComponentId component) const {
        if (registry_ == nullptr || component >= registry_->components_.size()) {
            return nullptr;
        }
        return registry_->components_[component].owner;
    }

    bool has_pending_for_group(const OwningGroupBase* group) const {
        if (group == nullptr || registry_ == nullptr) {
            return false;
        }

        bool found = false;
        std::apply([&](const auto&... stores) {
            ((found = found || (!stores.writes.empty() &&
                                owning_group(component_id<typename std::decay_t<decltype(stores)>::component_type>()) == group)),
             ...);
        }, pending_write_stores_);
        return found;
    }

    bool has_pending_writes() const {
        return pending_write_count_ != 0;
    }

    template <typename T, typename Func>
    void each_pending_write(Func&& func) const {
        const auto& store = pending_store<T>();
        for (const auto& pending : store.writes) {
            const T* staged = pending.storage->staged_ptr(pending.pending, tsn_);
            func(pending.entity, staged);
        }
    }

    Timestamp tsn_ = 0;
    std::tuple<PendingWriteStore<detail::component_base_t<DeclaredComponents>>...> pending_write_stores_;
    std::vector<typename Registry::ClassicAccessRegistration> classic_accesses_;
    std::size_t pending_write_count_ = 0;
    std::size_t pending_write_reserve_hint_ = 0;
};

template <typename T, typename TransactionType>
class TransactionStorageView {
public:
    explicit TransactionStorageView(TransactionType& transaction)
        : transaction_(&transaction) {}

    const T* try_get(Entity entity) const {
        return transaction_->template try_get<T>(entity);
    }

    const T& get(Entity entity) const {
        return transaction_->template get<T>(entity);
    }

    bool contains(Entity entity) const {
        return try_get(entity) != nullptr;
    }

    QueryExplain explain() const {
        const std::size_t visible = size();
        QueryExplain plan{};
        plan.empty = visible == 0;
        plan.anchor_component = component_id<T>();
        plan.anchor_component_name = detail::type_name<T>();
        plan.anchor_component_index = 0;
        plan.candidate_rows = visible;
        plan.estimated_entity_lookups = 0;
        plan.steps.push_back(QueryAccessStep{
            component_id<T>(),
            0,
            visible,
            QueryAccessKind::anchor_scan,
            false,
            detail::type_name<T>(),
        });
        return plan;
    }

    template <auto... Members, typename... KeyParts>
    QueryExplain explain_find(KeyParts&&... key_parts) const {
        using index_spec = typename detail::tuple_member_pack_index_spec<typename ComponentIndices<T>::type, Members...>::type;
        const std::size_t matches = find_all<Members...>(std::forward<KeyParts>(key_parts)...).size();
        const std::size_t visible = size();
        const bool use_index =
            !transaction_->component_belongs_to_group(component_id<T>()) &&
            !std::is_void_v<index_spec> &&
            matches < visible;
        QueryExplain plan{};
        plan.empty = matches == 0;
        plan.anchor_component = component_id<T>();
        plan.anchor_component_name = detail::type_name<T>();
        plan.anchor_component_index = 0;
        plan.candidate_rows = matches;
        plan.estimated_entity_lookups = 0;
        plan.uses_component_indexes = use_index;
        plan.steps.push_back(QueryAccessStep{
            component_id<T>(),
            0,
            visible,
            use_index ? QueryAccessKind::index_seek : QueryAccessKind::anchor_scan,
            use_index,
            detail::type_name<T>(),
        });
        return plan;
    }

    template <auto Member, typename Value>
    QueryExplain explain_where(PredicateOperator op, Value&& value) const {
        const auto matches = find_where<Member>(op, std::forward<Value>(value));
        QueryExplain plan{};
        plan.empty = matches.empty();
        plan.anchor_component = component_id<T>();
        plan.anchor_component_name = detail::type_name<T>();
        plan.anchor_component_index = 0;
        plan.candidate_rows = matches.size();
        plan.estimated_entity_lookups = 0;
        plan.steps.push_back(QueryAccessStep{
            component_id<T>(),
            0,
            size(),
            QueryAccessKind::anchor_scan,
            false,
            detail::type_name<T>(),
        });

        if constexpr (!std::is_void_v<typename detail::tuple_member_index_spec<Member, typename ComponentIndices<T>::type>::type>) {
            if (transaction_->component_belongs_to_group(component_id<T>())) {
                return plan;
            }
            if (matches.size() < size()) {
                plan.uses_component_indexes = true;
                plan.steps[0].access = QueryAccessKind::index_seek;
                plan.steps[0].uses_component_index = true;
            }
        }

        return plan;
    }

    std::size_t size() const {
        return transaction_->template visible_component_size<T>();
    }

    template <auto... Members, typename... KeyParts>
    std::vector<Entity> find_all(KeyParts&&... key_parts) const {
        using query_spec = detail::ComponentIndexSpec<false, Members...>;
        using index_spec = typename detail::tuple_member_pack_index_spec<typename ComponentIndices<T>::type, Members...>::type;
        static_assert(std::is_same_v<typename query_spec::component_type, T>, "find field must belong to the storage component");

        const auto key = detail::make_index_key<query_spec>(std::forward<KeyParts>(key_parts)...);
        std::vector<Entity> matches;

        if constexpr (!std::is_void_v<index_spec>) {
            if (transaction_->component_belongs_to_group(component_id<T>())) {
                each([&](Entity entity, const T& component) {
                    if (query_spec::key(component) == key) {
                        matches.push_back(entity);
                    }
                });
                return matches;
            }
            if (const auto* storage = transaction_->template typed_raw_storage<T>()) {
                const auto indexed = storage->template find_index<index_spec>(key);
                matches.reserve(indexed.size());
                for (const Entity entity : indexed) {
                    if (const T* component = transaction_->template try_get<T>(entity);
                        component != nullptr && query_spec::key(*component) == key) {
                        matches.push_back(entity);
                    }
                }
            }
        } else {
            each([&](Entity entity, const T& component) {
                if (query_spec::key(component) == key) {
                    matches.push_back(entity);
                }
            });
            return matches;
        }

        transaction_->template each_pending_write<T>([&](Entity entity, const T* staged) {
            if (staged != nullptr && query_spec::key(*staged) == key &&
                std::find(matches.begin(), matches.end(), entity) == matches.end()) {
                matches.push_back(entity);
            }
        });
        return matches;
    }

    template <auto... Members, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        const std::vector<Entity> matches = find_all<Members...>(std::forward<KeyParts>(key_parts)...);
        return matches.empty() ? null_entity : matches.front();
    }

    template <typename Func>
    void each(Func&& func) const {
        for (const Entity entity : transaction_->entities()) {
            if (const T* component = transaction_->template try_get<T>(entity)) {
                func(entity, *component);
            }
        }
    }

    template <auto Member, typename Value>
    std::vector<Entity> find_where(PredicateOperator op, Value&& raw_value) const {
        using predicate_type = ViewPredicate<Member, std::decay_t<Value>>;
        using component_type = typename predicate_type::component_type;
        using stored_type = std::decay_t<Value>;
        static_assert(std::is_same_v<component_type, T>, "predicate member must belong to the storage component");

        const stored_type value(std::forward<Value>(raw_value));
        std::vector<Entity> matches;
        bool used_index = false;

        using index_spec = typename detail::tuple_member_index_spec<Member, typename ComponentIndices<T>::type>::type;
        if constexpr (!std::is_void_v<index_spec>) {
            if (transaction_->component_belongs_to_group(component_id<T>())) {
                each([&](Entity entity, const T& component) {
                    if (compare_values(component.*Member, op, value)) {
                        matches.push_back(entity);
                    }
                });
                return matches;
            }
            used_index = true;
            if (const auto* storage = transaction_->template typed_raw_storage<T>()) {
                const auto indexed = storage->template find_compare_index<index_spec>(op, value);
                matches.reserve(indexed.size());
                for (const Entity entity : indexed) {
                    if (const T* component = transaction_->template try_get<T>(entity);
                        component != nullptr && compare_values(component->*Member, op, value)) {
                        matches.push_back(entity);
                    }
                }
            }
        }

        if (!used_index || matches.size() >= size()) {
            matches.clear();
            each([&](Entity entity, const T& component) {
                if (compare_values(component.*Member, op, value)) {
                    matches.push_back(entity);
                }
            });
            return matches;
        }

        transaction_->template each_pending_write<T>([&](Entity entity, const T* staged) {
            if (staged != nullptr && compare_values(staged->*Member, op, value) &&
                std::find(matches.begin(), matches.end(), entity) == matches.end()) {
                matches.push_back(entity);
            }
        });
        return matches;
    }

private:
    template <auto Member>
    static constexpr bool has_index() {
        return !std::is_void_v<typename detail::tuple_member_index_spec<Member, typename ComponentIndices<T>::type>::type>;
    }

    TransactionType* transaction_;
};

struct QuerySeedPlan {
    std::vector<Entity> candidates;
    std::vector<std::size_t> indexed_component_indices;
};

inline void normalize_entities(std::vector<Entity>& entities) {
    std::sort(entities.begin(), entities.end());
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
}

inline std::vector<Entity> intersect_entities(std::vector<Entity> lhs, std::vector<Entity> rhs) {
    normalize_entities(lhs);
    normalize_entities(rhs);
    std::vector<Entity> result;
    result.reserve(std::min(lhs.size(), rhs.size()));
    std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(result));
    return result;
}

inline std::vector<Entity> union_entities(std::vector<Entity> lhs, std::vector<Entity> rhs) {
    normalize_entities(lhs);
    normalize_entities(rhs);
    std::vector<Entity> result;
    result.reserve(lhs.size() + rhs.size());
    std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(result));
    return result;
}

template <typename TransactionType, typename... Components>
class TransactionView {
    static_assert(sizeof...(Components) > 0, "views require at least one component type");
    static_assert(detail::unique_component_types<Components...>::value,
                  "views cannot contain duplicate component types");

public:
    explicit TransactionView(TransactionType& transaction)
        : transaction_(&transaction) {}

    QueryExplain explain() const {
        return build_scan_plan().explain;
    }

    std::string explain_text() const {
        return format_explain(explain());
    }

    template <auto Member,
              typename Value,
              typename Component = typename detail::member_pointer_traits<decltype(Member)>::component_type,
              typename = std::enable_if_t<detail::contains_component_v<Component, Components...> &&
                                          !detail::is_singleton_component_v<Component>>>
    auto where(PredicateOperator op, Value&& value) const {
        using predicate_type = ViewPredicate<Member, std::decay_t<Value>>;
        return FilteredTransactionView<TransactionType, predicate_type, Components...>(
            *transaction_,
            predicate_type{op, std::forward<Value>(value)});
    }

    template <auto Member, typename Value>
    auto where_eq(Value&& value) const { return where<Member>(PredicateOperator::eq, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_ne(Value&& value) const { return where<Member>(PredicateOperator::ne, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_gt(Value&& value) const { return where<Member>(PredicateOperator::gt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_gte(Value&& value) const { return where<Member>(PredicateOperator::gte, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_lt(Value&& value) const { return where<Member>(PredicateOperator::lt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_lte(Value&& value) const { return where<Member>(PredicateOperator::lte, std::forward<Value>(value)); }

    template <typename Builder>
    auto and_group(Builder&& builder) const {
        return std::forward<Builder>(builder)(TransactionView<TransactionType, Components...>(*transaction_));
    }

    template <typename Builder>
    auto or_group(Builder&& builder) const {
        return std::forward<Builder>(builder)(TransactionView<TransactionType, Components...>(*transaction_));
    }

    template <typename Func>
    void forEach(Func&& func) const {
        auto&& callback = func;
        const auto planned = build_scan_plan();
        if (planned.explain.empty) {
            return;
        }

        if (planned.anchor_entities == nullptr) {
            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch<Components>(null_entity)...};
            invoke(callback, null_entity, components, std::index_sequence_for<Components...>{});
            return;
        }

        if (planned.group != nullptr) {
            iterate_group(callback, planned);
            return;
        }

        iterate_scan(callback, planned);
    }

private:
    struct ComponentDescriptor {
        ComponentId component = null_component;
        std::size_t component_index = 0;
        std::string_view component_name{};
        bool singleton = false;
        std::size_t (*visible_rows)(const TransactionView&) = nullptr;
    };

    struct PlannedView {
        const std::vector<Entity>* anchor_entities = nullptr;
        std::vector<Entity> materialized_anchor_entities;
        std::size_t anchor_index = 0;
        std::size_t grouped_component_count = 0;
        const OwningGroupBase* group = nullptr;
        std::vector<std::size_t> covered_component_indices;
        std::array<const RawPagedSparseArray*, sizeof...(Components)> grouped_storages{};
        const RawPagedSparseArray* anchor_storage = nullptr;
        bool anchor_uses_dense_iteration = false;
        QueryExplain explain;
    };

    struct SourceCandidate {
        const std::vector<Entity>* entities = nullptr;
        std::vector<Entity> materialized_entities;
        std::size_t row_count = 0;
        std::size_t anchor_index = 0;
        ComponentId anchor_component = null_component;
        std::string_view anchor_name{};
        const OwningGroupBase* group = nullptr;
        std::vector<std::size_t> covered_component_indices;
        std::array<const RawPagedSparseArray*, sizeof...(Components)> grouped_storages{};
        const RawPagedSparseArray* anchor_storage = nullptr;
        bool anchor_uses_dense_iteration = false;
    };

    static const std::array<ComponentDescriptor, sizeof...(Components)>& component_descriptors() {
        static const std::array<ComponentDescriptor, sizeof...(Components)> descriptors{{
            ComponentDescriptor{
                component_id<detail::component_base_t<Components>>(),
                detail::component_index_of<detail::component_base_t<Components>, detail::component_base_t<Components>...>::value,
                detail::type_name<detail::component_base_t<Components>>(),
                detail::is_singleton_component_v<detail::component_base_t<Components>>,
                [](const TransactionView& self) -> std::size_t {
                    return self.template visible_size<Components>();
                },
            }...
        }};
        return descriptors;
    }

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch(Entity entity) const {
        using Base = detail::component_base_t<Component>;
        if constexpr (detail::is_singleton_component_v<Base>) {
            (void)entity;
            return transaction_->template try_get<Base>();
        } else {
            return transaction_->template try_get<Base>(entity);
        }
    }

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch_known_alive(Entity entity, std::size_t component_index) const {
        using Base = detail::component_base_t<Component>;
        if constexpr (detail::is_singleton_component_v<Base>) {
            (void)entity;
            (void)component_index;
            return transaction_->template try_get<Base>();
        }

        const ComponentId component = component_descriptors()[component_index].component;
        if (const OwningGroupBase* owner = transaction_->owning_group(component); owner != nullptr && owner->contains(entity)) {
            if (const RawPagedSparseArray* raw = owner->storage(component); raw != nullptr) {
                return static_cast<detail::component_pointer_t<TransactionType, Component>>(
                    transaction_->try_get_visible_from_storage(*raw, entity));
            }
            return nullptr;
        }

        if (const RawPagedSparseArray* raw = transaction_->raw_storage(component); raw != nullptr) {
            return static_cast<detail::component_pointer_t<TransactionType, Component>>(
                transaction_->try_get_visible_from_storage(*raw, entity));
        }
        return nullptr;
    }

    template <typename Tuple, std::size_t... Indices>
    static bool all_present(const Tuple& components, std::index_sequence<Indices...>) {
        return ((std::get<Indices>(components) != nullptr) && ...);
    }

    template <typename Func, typename Tuple, std::size_t... Indices>
    static void invoke(Func& func, Entity entity, Tuple& components, std::index_sequence<Indices...>) {
        func(entity, *std::get<Indices>(components)...);
    }

    template <typename Func>
    void iterate_scan(Func& func, const PlannedView& planned) const {
        ECS_PROFILE_ZONE("TransactionView::iterate_scan");
        const std::vector<Entity>& anchor_entities = *planned.anchor_entities;
        if (planned.anchor_uses_dense_iteration && planned.anchor_storage != nullptr) {
            iterate_scan_dense(func, planned, anchor_entities);
            return;
        }

        for (const Entity entity : anchor_entities) {
            if (entity == null_entity) {
                continue;
            }
            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch_known_alive<Components>(entity, detail::component_index_of<detail::component_base_t<Components>, detail::component_base_t<Components>...>::value)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(func, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

    template <typename Func, std::size_t... Indices>
    void iterate_scan_dense_impl(
        Func& func,
        const PlannedView& planned,
        const std::vector<Entity>& anchor_entities,
        std::index_sequence<Indices...>) const {
        for (std::size_t dense_index = 0; dense_index < anchor_entities.size(); ++dense_index) {
            const Entity entity = anchor_entities[dense_index];
            if (entity == null_entity) {
                continue;
            }

            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch_anchor_dense<Components>(planned, entity, dense_index, Indices)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(func, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

    template <typename Func>
    void iterate_scan_dense(Func& func, const PlannedView& planned, const std::vector<Entity>& anchor_entities) const {
        ECS_PROFILE_ZONE("TransactionView::iterate_scan_dense");
        iterate_scan_dense_impl(func, planned, anchor_entities, std::index_sequence_for<Components...>{});
    }

    static bool plan_covers_component(const PlannedView& planned, std::size_t component_index) {
        return planned.grouped_storages[component_index] != nullptr;
    }

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch_anchor_dense(
        const PlannedView& planned,
        Entity entity,
        std::size_t dense_index,
        std::size_t component_index) const {
        if (component_index == planned.anchor_index && planned.anchor_storage != nullptr) {
            return static_cast<detail::component_pointer_t<TransactionType, Component>>(
                transaction_->try_get_visible_dense(*planned.anchor_storage, dense_index));
        }
        if (planned.group != nullptr && plan_covers_component(planned, component_index)) {
            return static_cast<detail::component_pointer_t<TransactionType, Component>>(
                transaction_->try_get_visible_dense(*planned.grouped_storages[component_index], dense_index));
        }
        return fetch_known_alive<Component>(entity, component_index);
    }

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch_grouped(
        const PlannedView& planned,
        Entity entity,
        std::size_t dense_index,
        std::size_t component_index) const {

        if (planned.group != nullptr && plan_covers_component(planned, component_index)) {
            const RawPagedSparseArray* raw = planned.grouped_storages[component_index];
            return raw == nullptr
                ? nullptr
                : static_cast<detail::component_pointer_t<TransactionType, Component>>(
                      transaction_->try_get_visible_dense(*raw, dense_index));
        }
        return fetch_known_alive<Component>(entity, component_index);
    }

    template <typename Func, std::size_t... Indices>
    void iterate_group_impl(Func& func, const PlannedView& planned, std::index_sequence<Indices...>) const {
        ECS_PROFILE_ZONE("TransactionView::iterate_group");
        const std::vector<Entity>& anchor_entities = *planned.anchor_entities;
        for (std::size_t dense_index = 0; dense_index < anchor_entities.size(); ++dense_index) {
            const Entity entity = anchor_entities[dense_index];
            if (entity == null_entity) {
                continue;
            }

            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch_grouped<Components>(planned, entity, dense_index, Indices)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(func, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

    template <typename Func>
    void iterate_group(Func& func, const PlannedView& planned) const {
        iterate_group_impl(func, planned, std::index_sequence_for<Components...>{});
    }

    bool can_use_committed_counts() const {
        return transaction_->has_stable_visibility() && !transaction_->has_pending_writes();
    }

    template <typename Component>
    std::size_t visible_size() const {
        using Base = detail::component_base_t<Component>;
        if constexpr (detail::is_singleton_component_v<Base>) {
            return transaction_->template try_get<Base>() == nullptr ? 0 : 1;
        } else {
            if (can_use_committed_counts()) {
                std::size_t count = 0;
                if (const RawPagedSparseArray* storage = transaction_->raw_storage(component_id<Base>()); storage != nullptr) {
                    count += storage->size();
                }
                if (const OwningGroupBase* owner = transaction_->owning_group(component_id<Base>()); owner != nullptr) {
                    count += owner->entities().size();
                }
                return count;
            }

            std::size_t count = 0;
            for (const Entity entity : transaction_->entities()) {
                if (transaction_->template try_get<Base>(entity) != nullptr) {
                    ++count;
                }
            }
            return count;
        }
    }

    std::size_t visible_rows_in_entities(const std::vector<Entity>& entities, ComponentId component) const {
        std::size_t count = 0;
        for (const Entity entity : entities) {
            if (entity != null_entity && transaction_->try_get(entity, component) != nullptr) {
                ++count;
            }
        }
        return count;
    }

    std::vector<Entity> component_source_entities(ComponentId component) const {
        std::vector<Entity> entities;
        if (const RawPagedSparseArray* storage = transaction_->raw_storage(component); storage != nullptr) {
            entities.insert(entities.end(), storage->entities().begin(), storage->entities().end());
        }
        if (const OwningGroupBase* owner = transaction_->owning_group(component); owner != nullptr) {
            const auto& grouped = owner->entities();
            entities.insert(entities.end(), grouped.begin(), grouped.end());
        }
        normalize_entities(entities);
        return entities;
    }

    SourceCandidate build_component_candidate(
        std::size_t component_index,
        ComponentId component,
        std::string_view name,
        std::size_t rows) const {

        SourceCandidate candidate{};
        candidate.anchor_index = component_index;
        candidate.anchor_component = component;
        candidate.anchor_name = name;
        candidate.row_count = rows;

        const RawPagedSparseArray* storage = transaction_->raw_storage(component);
        const OwningGroupBase* owner = transaction_->owning_group(component);
        const bool has_standalone = storage != nullptr && !storage->empty();
        const bool has_group = owner != nullptr && !owner->entities().empty();

        if (has_standalone && !has_group) {
            candidate.entities = &storage->entities();
            candidate.anchor_storage = storage;
            candidate.anchor_uses_dense_iteration = true;
            return candidate;
        }
        if (!has_standalone && has_group) {
            candidate.entities = &owner->entities();
            candidate.anchor_storage = owner->storage(component);
            candidate.anchor_uses_dense_iteration = candidate.anchor_storage != nullptr;
            return candidate;
        }

        candidate.materialized_entities = component_source_entities(component);
        return candidate;
    }

    std::vector<SourceCandidate> build_group_candidates(std::size_t best_non_group_rows) const {
        ECS_PROFILE_ZONE("TransactionView::build_group_candidates");
        struct GroupRequest {
            const OwningGroupBase* group = nullptr;
            std::size_t covered_count = 0;
        };

        std::vector<GroupRequest> requested_groups;
        requested_groups.reserve(sizeof...(Components));

        for (const ComponentDescriptor& descriptor : component_descriptors()) {
            const OwningGroupBase* group = transaction_->owning_group(descriptor.component);
            if (group == nullptr || group->component_count() < 2 || transaction_->has_pending_for_group(group)) {
                continue;
            }

            auto it = std::find_if(requested_groups.begin(), requested_groups.end(), [&](const GroupRequest& request) {
                return request.group == group;
            });
            if (it == requested_groups.end()) {
                requested_groups.push_back(GroupRequest{group, 1});
            } else {
                ++it->covered_count;
            }
        }

        std::vector<SourceCandidate> candidates;
        candidates.reserve(requested_groups.size());

        for (const GroupRequest& request : requested_groups) {
            if (request.covered_count < 2) {
                continue;
            }

            SourceCandidate candidate{};
            candidate.entities = &request.group->entities();
            candidate.group = request.group;
            candidate.row_count = can_use_committed_counts() ? candidate.entities->size() : 0;

            for (const ComponentDescriptor& descriptor : component_descriptors()) {
                const auto& component_infos = request.group->component_infos();
                const auto info = std::find_if(component_infos.begin(), component_infos.end(), [&](const OwningGroupBase::ComponentInfo& entry) {
                    return entry.component == descriptor.component;
                });
                if (info == component_infos.end()) {
                    continue;
                }

                if (!can_use_committed_counts() && candidate.covered_component_indices.empty()) {
                    candidate.row_count = visible_rows_in_entities(*candidate.entities, descriptor.component);
                }

                candidate.covered_component_indices.push_back(descriptor.component_index);
                candidate.grouped_storages[descriptor.component_index] = info->storage;
                if (candidate.covered_component_indices.size() == 1 || descriptor.component_index < candidate.anchor_index) {
                    candidate.anchor_index = descriptor.component_index;
                    candidate.anchor_component = descriptor.component;
                    candidate.anchor_name = descriptor.component_name;
                    candidate.anchor_storage = info->storage;
                    candidate.anchor_uses_dense_iteration = candidate.anchor_storage != nullptr;
                }
            }

            if (candidate.covered_component_indices.size() < 2 || candidate.row_count == 0) {
                continue;
            }
            if (best_non_group_rows != std::numeric_limits<std::size_t>::max() &&
                candidate.row_count > best_non_group_rows) {
                continue;
            }

            candidates.push_back(std::move(candidate));
        }
        return candidates;
    }

    void apply_candidate(PlannedView& planned, const SourceCandidate& candidate) const {
        if (candidate.entities != nullptr) {
            planned.anchor_entities = candidate.entities;
            planned.materialized_anchor_entities.clear();
        } else {
            planned.materialized_anchor_entities = candidate.materialized_entities;
            planned.anchor_entities = &planned.materialized_anchor_entities;
        }
        planned.anchor_index = candidate.anchor_index;
        planned.grouped_component_count = candidate.covered_component_indices.size();
        planned.group = candidate.group;
        planned.covered_component_indices = candidate.covered_component_indices;
        planned.grouped_storages = candidate.grouped_storages;
        planned.anchor_storage = candidate.anchor_storage;
        planned.anchor_uses_dense_iteration = candidate.anchor_uses_dense_iteration;
        planned.explain.anchor_component = candidate.anchor_component;
        planned.explain.anchor_component_name = candidate.anchor_name;
        planned.explain.anchor_component_index = candidate.anchor_index;
        planned.explain.candidate_rows = candidate.row_count;
    }

    void append_candidate_explain(QueryExplain& explain,
                                  const SourceCandidate& candidate,
                                  bool chosen) const {
        explain.candidates.push_back(QuerySourceCandidate{
            candidate.group != nullptr ? QuerySourceKind::owning_group : QuerySourceKind::standalone_storage,
            candidate.anchor_component,
            candidate.anchor_index,
            candidate.row_count,
            candidate.covered_component_indices.size(),
            chosen,
            candidate.anchor_name,
        });
    }

    static bool prefer_candidate(const SourceCandidate& candidate, const SourceCandidate* current) {
        if (current == nullptr) {
            return true;
        }
        if (candidate.row_count != current->row_count) {
            return candidate.row_count < current->row_count;
        }
        if ((candidate.group != nullptr) != (current->group != nullptr)) {
            return candidate.group != nullptr;
        }
        return candidate.anchor_index < current->anchor_index;
    }

    template <std::size_t... Indices>
    PlannedView build_scan_plan_impl(std::index_sequence<Indices...>) const {
        (void)sizeof...(Indices);
        ECS_PROFILE_ZONE("TransactionView::build_scan_plan");
        PlannedView planned{};
        planned.explain.steps.reserve(sizeof...(Components));
        planned.explain.candidates.reserve(sizeof...(Components));

        const SourceCandidate* chosen = nullptr;
        bool empty = false;
        std::vector<SourceCandidate> component_candidates;

        for (const ComponentDescriptor& descriptor : component_descriptors()) {
            const std::size_t index = descriptor.component_index;
            const ComponentId component = descriptor.component;
            const std::size_t rows = descriptor.visible_rows(*this);
            planned.explain.steps.push_back(QueryAccessStep{
                component,
                index,
                rows,
                descriptor.singleton ? QueryAccessKind::singleton_read : QueryAccessKind::sparse_lookup,
                false,
                descriptor.component_name,
            });

            if (rows == 0) {
                empty = true;
            }

            if (!descriptor.singleton &&
                (transaction_->raw_storage(component) != nullptr || transaction_->component_belongs_to_group(component))) {
                component_candidates.push_back(build_component_candidate(
                    index,
                    component,
                    descriptor.component_name,
                    rows));
            }
        }

        std::size_t best_non_group_rows = std::numeric_limits<std::size_t>::max();
        for (const SourceCandidate& candidate : component_candidates) {
            if (prefer_candidate(candidate, chosen)) {
                chosen = &candidate;
            }
            if (candidate.group == nullptr && candidate.row_count < best_non_group_rows) {
                best_non_group_rows = candidate.row_count;
            }
        }

        std::vector<SourceCandidate> group_candidates = build_group_candidates(best_non_group_rows);
        for (const SourceCandidate& candidate : group_candidates) {
            if (prefer_candidate(candidate, chosen)) {
                chosen = &candidate;
            }
        }

        if (empty || (chosen == nullptr &&
                      detail::entity_component_count_v<detail::component_base_t<Components>...> != 0)) {
            planned.anchor_entities = nullptr;
            planned.explain.empty = true;
            planned.explain.anchor_component = null_component;
            planned.explain.anchor_component_name = {};
            planned.explain.anchor_component_index = 0;
            planned.explain.candidate_rows = 0;
            planned.explain.estimated_entity_lookups = 0;
            return planned;
        }

        if (chosen == nullptr) {
            planned.explain.empty = false;
            planned.explain.anchor_component = null_component;
            planned.explain.anchor_component_name = {};
            planned.explain.anchor_component_index = 0;
            planned.explain.candidate_rows = 1;
            planned.explain.estimated_entity_lookups = 0;
            return planned;
        }

        for (const SourceCandidate& candidate : component_candidates) {
            append_candidate_explain(planned.explain, candidate, &candidate == chosen);
        }
        for (const SourceCandidate& candidate : group_candidates) {
            append_candidate_explain(planned.explain, candidate, &candidate == chosen);
        }

        apply_candidate(planned, *chosen);
        planned.explain.empty = false;
        if (chosen->group != nullptr) {
            planned.explain.estimated_entity_lookups =
                planned.explain.candidate_rows *
                (detail::entity_component_count_v<detail::component_base_t<Components>...> - planned.grouped_component_count);
            for (QueryAccessStep& step : planned.explain.steps) {
                const bool covered = std::find(
                    chosen->covered_component_indices.begin(),
                    chosen->covered_component_indices.end(),
                    step.component_index) != chosen->covered_component_indices.end();
                if (!covered) {
                    continue;
                }
                step.access = step.component_index == chosen->anchor_index
                    ? QueryAccessKind::anchor_scan
                    : QueryAccessKind::grouped_fetch;
            }
        } else {
            planned.explain.estimated_entity_lookups =
                planned.explain.candidate_rows *
                (detail::entity_component_count_v<detail::component_base_t<Components>...> - 1);
            planned.explain.steps[planned.anchor_index].access = QueryAccessKind::anchor_scan;
        }
        return planned;
    }

    PlannedView build_scan_plan() const {
        return build_scan_plan_impl(std::index_sequence_for<Components...>{});
    }

    static std::string format_explain(const QueryExplain& plan) {
        std::string text = "EXPLAIN VIEW";

        if (plan.empty) {
            text += "\n  Result: empty";
            text += "\n  Reason: at least one required component has no visible rows in this transaction";
            text += "\n  Component value indexes: not used by view execution";
            append_steps(text, plan);
            return text;
        }

        if (plan.anchor_component == null_component) {
            text += "\n  Anchor: singleton-only";
        } else {
            text += "\n  Anchor: component[" + std::to_string(plan.anchor_component_index) + "] ";
            text += std::string(plan.anchor_component_name);
            text += plan.uses_component_indexes ? " via index seek" : " via anchor scan";
        }
        text += "\n  Candidates: " + std::to_string(plan.candidate_rows);
        text += "\n  Entity probes: " + std::to_string(plan.estimated_entity_lookups);
        text += "\n  Component value indexes: ";
        text += plan.uses_component_indexes ? "used" : "not used by view execution";
        append_candidates(text, plan);
        append_steps(text, plan);
        return text;
    }

    static void append_candidates(std::string& text, const QueryExplain& plan) {
        if (plan.candidates.empty()) {
            return;
        }

        text += "\n  Source candidates:";
        for (const QuerySourceCandidate& candidate : plan.candidates) {
            text += "\n  - ";
            if (candidate.source == QuerySourceKind::owning_group) {
                text += "group";
            } else if (candidate.source == QuerySourceKind::component_index) {
                text += "index";
            } else {
                text += "storage";
            }
            text += " component[" + std::to_string(candidate.component_index) + "] ";
            text += std::string(candidate.component_name);
            text += ", rows=" + std::to_string(candidate.candidate_rows);
            if (candidate.source == QuerySourceKind::owning_group) {
                text += ", covers=" + std::to_string(candidate.grouped_component_count);
            }
            text += candidate.chosen ? " (chosen)" : " (not chosen)";
        }
    }

    static void append_steps(std::string& text, const QueryExplain& plan) {
        for (const QueryAccessStep& step : plan.steps) {
            text += "\n  - component[" + std::to_string(step.component_index) + "] ";
            text += std::string(step.component_name);
            text += ": ";
            if (step.access == QueryAccessKind::anchor_scan) {
                text += "anchor scan";
            } else if (step.access == QueryAccessKind::index_seek) {
                text += "index seek";
            } else if (step.access == QueryAccessKind::singleton_read) {
                text += "singleton read";
            } else if (step.access == QueryAccessKind::grouped_fetch) {
                text += "group row fetch";
            } else if (step.access == QueryAccessKind::singleton_read) {
                text += "singleton read";
            } else {
                text += "sparse entity probe";
            }
            text += ", visible rows=" + std::to_string(step.visible_rows);
            text += ", component index=";
            text += step.uses_component_index ? "used" : "not used";
        }
    }

    TransactionType* transaction_;

    template <typename, typename, typename...>
    friend class FilteredTransactionView;
};

template <typename TransactionType, typename Expression, typename... Components>
class FilteredTransactionView {
    static_assert(sizeof...(Components) > 0, "views require at least one component type");
    static_assert(detail::unique_component_types<Components...>::value,
                  "views cannot contain duplicate component types");

public:
    explicit FilteredTransactionView(TransactionType& transaction, Expression expression)
        : transaction_(&transaction),
          expression_(std::move(expression)) {}

    QueryExplain explain() const {
        return build_plan().explain;
    }

    std::string explain_text() const {
        return TransactionView<TransactionType, Components...>::format_explain(explain());
    }

    template <auto Member,
              typename Value,
              typename Component = typename detail::member_pointer_traits<decltype(Member)>::component_type,
              typename = std::enable_if_t<detail::contains_component_v<Component, Components...> &&
                                          !detail::is_singleton_component_v<Component>>>
    auto where(PredicateOperator op, Value&& value) const {
        using predicate_type = ViewPredicate<Member, std::decay_t<Value>>;
        return FilteredTransactionView<TransactionType, AndPredicate<Expression, predicate_type>, Components...>(
            *transaction_,
            AndPredicate<Expression, predicate_type>{expression_, predicate_type{op, std::forward<Value>(value)}});
    }

    template <auto Member, typename Value>
    auto where_eq(Value&& value) const { return where<Member>(PredicateOperator::eq, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_ne(Value&& value) const { return where<Member>(PredicateOperator::ne, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_gt(Value&& value) const { return where<Member>(PredicateOperator::gt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_gte(Value&& value) const { return where<Member>(PredicateOperator::gte, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_lt(Value&& value) const { return where<Member>(PredicateOperator::lt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto where_lte(Value&& value) const { return where<Member>(PredicateOperator::lte, std::forward<Value>(value)); }
    template <auto Member,
              typename Value,
              typename Component = typename detail::member_pointer_traits<decltype(Member)>::component_type,
              typename = std::enable_if_t<detail::contains_component_v<Component, Components...> &&
                                          !detail::is_singleton_component_v<Component>>>
    auto or_where(PredicateOperator op, Value&& value) const {
        using predicate_type = ViewPredicate<Member, std::decay_t<Value>>;
        return FilteredTransactionView<TransactionType, OrPredicate<Expression, predicate_type>, Components...>(
            *transaction_,
            OrPredicate<Expression, predicate_type>{expression_, predicate_type{op, std::forward<Value>(value)}});
    }
    template <auto Member, typename Value>
    auto or_where_eq(Value&& value) const { return or_where<Member>(PredicateOperator::eq, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto or_where_ne(Value&& value) const { return or_where<Member>(PredicateOperator::ne, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto or_where_gt(Value&& value) const { return or_where<Member>(PredicateOperator::gt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto or_where_gte(Value&& value) const { return or_where<Member>(PredicateOperator::gte, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto or_where_lt(Value&& value) const { return or_where<Member>(PredicateOperator::lt, std::forward<Value>(value)); }
    template <auto Member, typename Value>
    auto or_where_lte(Value&& value) const { return or_where<Member>(PredicateOperator::lte, std::forward<Value>(value)); }

    template <typename Builder>
    auto and_group(Builder&& builder) const {
        auto grouped = std::forward<Builder>(builder)(TransactionView<TransactionType, Components...>(*transaction_));
        return FilteredTransactionView<TransactionType, AndPredicate<Expression, typename decltype(grouped)::expression_type>, Components...>(
            *transaction_,
            AndPredicate<Expression, typename decltype(grouped)::expression_type>{expression_, grouped.expression_});
    }

    template <typename Builder>
    auto or_group(Builder&& builder) const {
        auto grouped = std::forward<Builder>(builder)(TransactionView<TransactionType, Components...>(*transaction_));
        return FilteredTransactionView<TransactionType, OrPredicate<Expression, typename decltype(grouped)::expression_type>, Components...>(
            *transaction_,
            OrPredicate<Expression, typename decltype(grouped)::expression_type>{expression_, grouped.expression_});
    }

    template <typename Func>
    void forEach(Func&& func) const {
        auto&& callback = func;
        const PlannedView planned = build_plan();
        if (planned.explain.empty) {
            return;
        }

        if (planned.use_index) {
            for (const Entity entity : planned.candidates) {
                visit_entity(callback, entity);
            }
            return;
        }

        if (planned.anchor_entities == nullptr) {
            visit_entity(callback, null_entity);
            return;
        }

        for (const Entity entity : *planned.anchor_entities) {
            if (entity != null_entity) {
                visit_entity(callback, entity);
            }
        }
    }

private:
    using expression_type = Expression;

    struct PlannedView {
        const std::vector<Entity>* anchor_entities = nullptr;
        std::vector<Entity> materialized_anchor_entities;
        std::size_t anchor_index = 0;
        bool use_index = false;
        std::vector<Entity> candidates;
        QueryExplain explain;
    };

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch(Entity entity) const {
        using Base = detail::component_base_t<Component>;
        if constexpr (detail::is_singleton_component_v<Base>) {
            (void)entity;
            return transaction_->template try_get<Base>();
        } else {
            return transaction_->template try_get<Base>(entity);
        }
    }

    template <typename Tuple, std::size_t... Indices>
    static bool all_present(const Tuple& components, std::index_sequence<Indices...>) {
        return ((std::get<Indices>(components) != nullptr) && ...);
    }

    template <typename Func, typename Tuple, std::size_t... Indices>
    static void invoke(Func& func, Entity entity, Tuple& components, std::index_sequence<Indices...>) {
        func(entity, *std::get<Indices>(components)...);
    }

    template <typename Func>
    void visit_entity(Func& func, Entity entity) const {
            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch<Components>(entity)...};

        if (all_present(components, std::index_sequence_for<Components...>{}) &&
            expression_matches(expression_, entity)) {
            invoke(func, entity, components, std::index_sequence_for<Components...>{});
        }
    }

    template <typename Component>
    std::size_t visible_size() const {
        using Base = detail::component_base_t<Component>;
        if constexpr (detail::is_singleton_component_v<Base>) {
            return transaction_->template try_get<Base>() == nullptr ? 0 : 1;
        } else {
            std::size_t count = 0;
            for (const Entity entity : transaction_->entities()) {
                if (transaction_->template try_get<Base>(entity) != nullptr) {
                    ++count;
                }
            }
            return count;
        }
    }

    template <typename Expr>
    bool expression_matches(const Expr& expression, Entity entity) const {
        if constexpr (std::is_same_v<Expr, TruePredicate>) {
            return true;
        } else if constexpr (is_and_predicate<Expr>::value) {
            return expression_matches(expression.left, entity) && expression_matches(expression.right, entity);
        } else if constexpr (is_or_predicate<Expr>::value) {
            return expression_matches(expression.left, entity) || expression_matches(expression.right, entity);
        } else {
            using Component = typename Expr::component_type;
            static_assert(!detail::is_singleton_component_v<Component>,
                          "predicates on singleton components are not supported");
            const Component* component = transaction_->template try_get<Component>(entity);
            return component != nullptr && expression.matches(*component);
        }
    }

    PlannedView build_plan() const {
        PlannedView planned{};
        const auto base_plan = TransactionView<TransactionType, Components...>(*transaction_).build_scan_plan();
        planned.explain = base_plan.explain;
        if (base_plan.anchor_entities != nullptr) {
            planned.materialized_anchor_entities = *base_plan.anchor_entities;
            planned.anchor_entities = &planned.materialized_anchor_entities;
        }
        planned.anchor_index = planned.explain.anchor_component_index;

        if (auto seed = build_seed_plan(expression_)) {
            if (seed->candidates.size() < planned.explain.candidate_rows) {
                planned.use_index = true;
                planned.candidates = std::move(seed->candidates);
                planned.anchor_entities = nullptr;
                planned.explain.uses_component_indexes = true;
                planned.explain.candidates.push_back(QuerySourceCandidate{
                    QuerySourceKind::component_index,
                    null_component,
                    seed->indexed_component_indices.front(),
                    planned.candidates.size(),
                    0,
                    true,
                    {},
                });
                mark_index_steps(planned.explain, seed->indexed_component_indices);
            }
        }

        if (planned.use_index) {
            planned.explain.empty = planned.candidates.empty();
            planned.explain.candidate_rows = planned.candidates.size();
            planned.explain.estimated_entity_lookups =
                planned.candidates.size() *
                (detail::entity_component_count_v<detail::component_base_t<Components>...> - 1);
        }

        return planned;
    }

    template <typename Expr>
    std::optional<QuerySeedPlan> build_seed_plan(const Expr& expression) const {
        if constexpr (std::is_same_v<Expr, TruePredicate>) {
            return std::nullopt;
        } else if constexpr (is_and_predicate<Expr>::value) {
            auto left = build_seed_plan(expression.left);
            auto right = build_seed_plan(expression.right);
            if (left && right) {
                QuerySeedPlan merged{};
                merged.candidates = intersect_entities(left->candidates, right->candidates);
                merged.indexed_component_indices = left->indexed_component_indices;
                merged.indexed_component_indices.insert(
                    merged.indexed_component_indices.end(),
                    right->indexed_component_indices.begin(),
                    right->indexed_component_indices.end());
                std::sort(merged.indexed_component_indices.begin(), merged.indexed_component_indices.end());
                merged.indexed_component_indices.erase(
                    std::unique(merged.indexed_component_indices.begin(), merged.indexed_component_indices.end()),
                    merged.indexed_component_indices.end());
                return merged;
            }
            return left ? left : right;
        } else if constexpr (is_or_predicate<Expr>::value) {
            auto left = build_seed_plan(expression.left);
            auto right = build_seed_plan(expression.right);
            if (!left || !right) {
                return std::nullopt;
            }
            QuerySeedPlan merged{};
            merged.candidates = union_entities(left->candidates, right->candidates);
            merged.indexed_component_indices = left->indexed_component_indices;
            merged.indexed_component_indices.insert(
                merged.indexed_component_indices.end(),
                right->indexed_component_indices.begin(),
                right->indexed_component_indices.end());
            std::sort(merged.indexed_component_indices.begin(), merged.indexed_component_indices.end());
            merged.indexed_component_indices.erase(
                std::unique(merged.indexed_component_indices.begin(), merged.indexed_component_indices.end()),
                merged.indexed_component_indices.end());
            return merged;
        } else {
            using Component = typename Expr::component_type;
            auto storage = transaction_->template storage<Component>();
            const QueryExplain predicate_plan = storage.template explain_where<Expr::member>(expression.op, expression.value);
            if (!predicate_plan.uses_component_indexes) {
                return std::nullopt;
            }
            QuerySeedPlan seed{};
            seed.candidates = storage.template find_where<Expr::member>(expression.op, expression.value);
            seed.indexed_component_indices.push_back(
                detail::component_index_of<Component, detail::component_base_t<Components>...>::value);
            return seed;
        }
    }

    static void mark_index_steps(QueryExplain& explain, const std::vector<std::size_t>& indices) {
        if (indices.empty()) {
            return;
        }

        explain.anchor_component_index = indices.front();
        QuerySourceCandidate* index_candidate = nullptr;
        for (QuerySourceCandidate& candidate : explain.candidates) {
            if (candidate.source == QuerySourceKind::component_index && candidate.chosen) {
                index_candidate = &candidate;
                break;
            }
        }
        for (QueryAccessStep& step : explain.steps) {
            const bool indexed =
                std::find(indices.begin(), indices.end(), step.component_index) != indices.end();
            if (indexed) {
                if (step.component_index == explain.anchor_component_index) {
                    explain.anchor_component = step.component;
                    explain.anchor_component_name = step.component_name;
                    if (index_candidate != nullptr) {
                        index_candidate->component = step.component;
                        index_candidate->component_name = step.component_name;
                    }
                }
                step.access = QueryAccessKind::index_seek;
                step.uses_component_index = true;
            } else if (step.access == QueryAccessKind::anchor_scan) {
                step.access = QueryAccessKind::sparse_lookup;
            }
        }
    }

    TransactionType* transaction_;
    Expression expression_;

    template <typename, typename, typename...>
    friend class FilteredTransactionView;
};

}  // namespace ecs
