#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "view_traits.hpp"

namespace ecs {

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
    sparse_lookup,
};

struct QueryAccessStep {
    ComponentId component = null_component;
    std::size_t component_index = 0;
    std::size_t visible_rows = 0;
    QueryAccessKind access = QueryAccessKind::sparse_lookup;
    bool uses_component_index = false;
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

    template <typename T>
    bool has(Entity entity) const {
        return try_get<T>(entity) != nullptr;
    }

    const void* try_get(Entity entity, ComponentId component) const {
        require_open();
        if (!registry_->alive(entity)) {
            return nullptr;
        }

        const RawPagedSparseArray* raw = registry_->storage(component);
        return raw == nullptr ? nullptr : raw->try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, 0);
    }

    template <typename T>
    const T* try_get(Entity entity) const {
        return static_cast<const T*>(try_get(entity, component_id<T>()));
    }

    template <typename T>
    const T& get(Entity entity) const {
        const T* component = try_get<T>(entity);
        if (component == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return *component;
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
    std::uint64_t max_visible_tsn_ = 0;
    std::vector<std::uint64_t> active_at_open_;
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
          writes_(std::move(other.writes_)),
          classic_accesses_(std::move(other.classic_accesses_)) {
        other.tsn_ = 0;
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
            writes_ = std::move(other.writes_);
            classic_accesses_ = std::move(other.classic_accesses_);
            other.tsn_ = 0;
        }
        return *this;
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool has(Entity entity, ComponentId component) const {
        return try_get(entity, component) != nullptr;
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...>>>
    bool has(Entity entity) const {
        return try_get<T>(entity) != nullptr;
    }

    const void* try_get(Entity entity, ComponentId component) const {
        require_open();
        if (!registry_->alive(entity)) {
            return nullptr;
        }

        const RawPagedSparseArray* raw = registry_->storage(component);
        return raw == nullptr ? nullptr : raw->try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, tsn_);
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...>>>
    const T* try_get(Entity entity) const {
        return static_cast<const T*>(try_get(entity, component_id<T>()));
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...>>>
    const T& get(Entity entity) const {
        const T* component = try_get<T>(entity);
        if (component == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return *component;
    }

    template <typename T, typename = std::enable_if_t<detail::readable_component_v<T, DeclaredComponents...>>>
    TransactionStorageView<T, Transaction<DeclaredComponents...>> storage() {
        return TransactionStorageView<T, Transaction<DeclaredComponents...>>(*this);
    }

    template <typename... Components,
              typename = std::enable_if_t<((detail::readable_component_v<detail::component_base_t<Components>, DeclaredComponents...>) && ...)>>
    TransactionView<Transaction<DeclaredComponents...>, Components...> view() {
        return TransactionView<Transaction<DeclaredComponents...>, Components...>(*this);
    }

    template <typename T, typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...>>>
    T* write(Entity entity) {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");
        require_open();
        registry_->require_alive(entity);

        if (auto* pending = find_pending<T>(entity)) {
            return pending->storage->staged_ptr(pending->pending, tsn_);
        }

        auto& storage = registry_->template assure_storage<T>();
        auto pending = storage.stage_write(entity, tsn_, active_at_open_, max_visible_tsn_);
        T* staged = storage.staged_ptr(pending, tsn_);
        writes_.push_back(std::make_unique<PendingWrite<T>>(entity, &storage, pending));
        return staged;
    }

    template <typename T,
              typename... Args,
              typename = std::enable_if_t<detail::writable_component_v<T, DeclaredComponents...>>>
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

        auto& storage = registry_->template assure_storage<T>();
        auto pending = storage.stage_value(entity, tsn_, value);
        T* staged = storage.staged_ptr(pending, tsn_);
        writes_.push_back(std::make_unique<PendingWrite<T>>(entity, &storage, pending));
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
            throw std::logic_error("classic component storage does not support transaction rollback");
        }

        finalize_writes(false);
    }

private:
    struct PendingWriteBase {
        PendingWriteBase(Entity pending_entity, ComponentId pending_component)
            : entity(pending_entity),
              component(pending_component) {}

        virtual ~PendingWriteBase() = default;
        virtual void commit(std::uint64_t tsn, TraceCommitContext trace_context) = 0;
        virtual void rollback(std::uint64_t tsn) = 0;
        virtual bool rollback_supported() const = 0;

        Entity entity;
        ComponentId component;
    };

    template <typename T>
    struct PendingWrite final : PendingWriteBase {
        PendingWrite(Entity entity,
                     ComponentStorage<T>* pending_storage,
                     typename ComponentStorage<T>::PendingWrite pending_write)
            : PendingWriteBase(entity, component_id<T>()),
              storage(pending_storage),
              pending(pending_write) {}

        void commit(std::uint64_t tsn, TraceCommitContext trace_context) override {
            storage->commit_staged(this->entity, pending, tsn, trace_context);
        }

        void rollback(std::uint64_t tsn) override {
            storage->rollback_staged(this->entity, pending, tsn);
        }

        bool rollback_supported() const override {
            return storage->storage_mode() != ComponentStorageMode::classic;
        }

        ComponentStorage<T>* storage;
        typename ComponentStorage<T>::PendingWrite pending;
    };

    template <typename T>
    PendingWrite<T>* find_pending(Entity entity) {
        const ComponentId component = component_id<T>();
        for (const auto& write : writes_) {
            if (write->entity == entity && write->component == component) {
                return static_cast<PendingWrite<T>*>(write.get());
            }
        }
        return nullptr;
    }

    bool rollback_supported() const {
        for (const auto& write : writes_) {
            if (!write->rollback_supported()) {
                return false;
            }
        }
        return true;
    }

    void finalize_writes(bool commit_writes) {
        require_open();

        for (const auto& write : writes_) {
            registry_->require_alive(write->entity);
        }

        for (auto& write : writes_) {
            if (commit_writes) {
                write->commit(tsn_, registry_->trace_commit_context_);
            } else {
                write->rollback(tsn_);
            }
        }

        writes_.clear();
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

    template <typename T, typename Func>
    void each_pending_write(Func&& func) const {
        for (const auto& write : writes_) {
            if (write->component != component_id<T>()) {
                continue;
            }

            const auto* pending = static_cast<const PendingWrite<T>*>(write.get());
            const T* staged = pending->storage->staged_ptr(pending->pending, tsn_);
            func(write->entity, staged);
        }
    }

    std::uint64_t tsn_ = 0;
    std::vector<std::unique_ptr<PendingWriteBase>> writes_;
    std::vector<typename Registry::ClassicAccessRegistration> classic_accesses_;
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
        QueryExplain plan{};
        plan.empty = size() == 0;
        plan.anchor_component = component_id<T>();
        plan.anchor_component_name = detail::type_name<T>();
        plan.anchor_component_index = 0;
        plan.candidate_rows = size();
        plan.estimated_entity_lookups = 0;
        plan.steps.push_back(QueryAccessStep{
            component_id<T>(),
            0,
            size(),
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
        const bool use_index = !std::is_void_v<index_spec> && matches < visible;
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
            if (op != PredicateOperator::ne && matches.size() < size()) {
                plan.uses_component_indexes = true;
                plan.steps[0].access = QueryAccessKind::index_seek;
                plan.steps[0].uses_component_index = true;
            }
        }

        return plan;
    }

    std::size_t size() const {
        std::size_t count = 0;
        each([&count](Entity, const T&) {
            ++count;
        });
        return count;
    }

    template <auto... Members, typename... KeyParts>
    std::vector<Entity> find_all(KeyParts&&... key_parts) const {
        using query_spec = detail::ComponentIndexSpec<false, Members...>;
        using index_spec = typename detail::tuple_member_pack_index_spec<typename ComponentIndices<T>::type, Members...>::type;
        static_assert(std::is_same_v<typename query_spec::component_type, T>, "find field must belong to the storage component");

        const auto key = detail::make_index_key<query_spec>(std::forward<KeyParts>(key_parts)...);
        std::vector<Entity> matches;

        if constexpr (!std::is_void_v<index_spec>) {
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
            if (op != PredicateOperator::ne) {
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

    template <auto Member, typename Value>
    auto where(PredicateOperator op, Value&& value) const {
        using component_type = typename detail::member_pointer_traits<decltype(Member)>::component_type;
        static_assert(detail::contains_component_v<component_type, Components...>,
                      "predicate member must belong to a component in the view");
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
        if (planned.anchor_storage == nullptr) {
            return;
        }

        iterate_scan(callback, planned.anchor_storage);
    }

private:
    struct PlannedView {
        const RawPagedSparseArray* anchor_storage = nullptr;
        std::size_t anchor_index = 0;
        QueryExplain explain;
    };

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch(Entity entity) const {
        using Base = detail::component_base_t<Component>;
        return transaction_->template try_get<Base>(entity);
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
    void iterate_scan(Func& func, const RawPagedSparseArray* anchor_storage) const {
        const auto& dense_entities = anchor_storage->entities();
        for (const Entity entity : dense_entities) {
            if (entity == null_entity) {
                continue;
            }

            auto components = std::tuple<detail::component_pointer_t<TransactionType, Components>...>{
                fetch<Components>(entity)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(func, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

    template <typename Component>
    std::size_t visible_size() const {
        using Base = detail::component_base_t<Component>;
        const RawPagedSparseArray* storage = transaction_->raw_storage(component_id<Base>());
        if (storage == nullptr) {
            return 0;
        }

        std::size_t count = 0;
        for (const Entity entity : storage->entities()) {
            if (entity != null_entity && transaction_->template try_get<Base>(entity) != nullptr) {
                ++count;
            }
        }
        return count;
    }

    template <std::size_t... Indices>
    PlannedView build_scan_plan_impl(std::index_sequence<Indices...>) const {
        PlannedView planned{};
        planned.explain.steps.reserve(sizeof...(Components));

        bool initialized = false;
        bool empty = false;

        auto consider = [&](auto index_constant, auto component_constant) {
            constexpr std::size_t index = decltype(index_constant)::value;
            using Component = typename decltype(component_constant)::type;
            using Base = detail::component_base_t<Component>;

            const ComponentId component = component_id<Base>();
            const RawPagedSparseArray* storage = transaction_->raw_storage(component);
            const std::size_t rows = visible_size<Component>();

            planned.explain.steps.push_back(QueryAccessStep{
                component,
                index,
                rows,
                QueryAccessKind::sparse_lookup,
                false,
                detail::type_name<Base>(),
            });

            if (storage == nullptr || rows == 0) {
                empty = true;
            }

            if (!initialized || rows < planned.explain.candidate_rows) {
                initialized = true;
                planned.anchor_storage = storage;
                planned.anchor_index = index;
                planned.explain.anchor_component = component;
                planned.explain.anchor_component_name = detail::type_name<Base>();
                planned.explain.anchor_component_index = index;
                planned.explain.candidate_rows = rows;
            }
        };

        (consider(std::integral_constant<std::size_t, Indices>{}, detail::type_tag<Components>{}), ...);

        if (!initialized || empty || planned.anchor_storage == nullptr) {
            planned.anchor_storage = nullptr;
            planned.explain.empty = true;
            planned.explain.anchor_component = null_component;
            planned.explain.anchor_component_name = {};
            planned.explain.anchor_component_index = 0;
            planned.explain.candidate_rows = 0;
            planned.explain.estimated_entity_lookups = 0;
            return planned;
        }

        planned.explain.empty = false;
        planned.explain.estimated_entity_lookups = planned.explain.candidate_rows * (sizeof...(Components) - 1);
        planned.explain.steps[planned.anchor_index].access = QueryAccessKind::anchor_scan;
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

        text += "\n  Anchor: component[" + std::to_string(plan.anchor_component_index) + "] ";
        text += std::string(plan.anchor_component_name);
        text += plan.uses_component_indexes ? " via index seek" : " via anchor scan";
        text += "\n  Candidates: " + std::to_string(plan.candidate_rows);
        text += "\n  Entity probes: " + std::to_string(plan.estimated_entity_lookups);
        text += "\n  Component value indexes: ";
        text += plan.uses_component_indexes ? "used" : "not used by view execution";
        append_steps(text, plan);
        return text;
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

    template <auto Member, typename Value>
    auto where(PredicateOperator op, Value&& value) const {
        using component_type = typename detail::member_pointer_traits<decltype(Member)>::component_type;
        static_assert(detail::contains_component_v<component_type, Components...>,
                      "predicate member must belong to a component in the view");
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
    template <auto Member, typename Value>
    auto or_where(PredicateOperator op, Value&& value) const {
        using component_type = typename detail::member_pointer_traits<decltype(Member)>::component_type;
        static_assert(detail::contains_component_v<component_type, Components...>,
                      "predicate member must belong to a component in the view");
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

        const auto& dense_entities = planned.anchor_storage->entities();
        for (const Entity entity : dense_entities) {
            if (entity != null_entity) {
                visit_entity(callback, entity);
            }
        }
    }

private:
    using expression_type = Expression;

    struct PlannedView {
        const RawPagedSparseArray* anchor_storage = nullptr;
        std::size_t anchor_index = 0;
        bool use_index = false;
        std::vector<Entity> candidates;
        QueryExplain explain;
    };

    template <typename Component>
    detail::component_pointer_t<TransactionType, Component> fetch(Entity entity) const {
        using Base = detail::component_base_t<Component>;
        return transaction_->template try_get<Base>(entity);
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
        const RawPagedSparseArray* storage = transaction_->raw_storage(component_id<Base>());
        if (storage == nullptr) {
            return 0;
        }

        std::size_t count = 0;
        for (const Entity entity : storage->entities()) {
            if (entity != null_entity && transaction_->template try_get<Base>(entity) != nullptr) {
                ++count;
            }
        }
        return count;
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
            const Component* component = transaction_->template try_get<Component>(entity);
            return component != nullptr && expression.matches(*component);
        }
    }

    PlannedView build_plan() const {
        PlannedView planned{};
        planned.explain = TransactionView<TransactionType, Components...>(*transaction_).explain();
        planned.anchor_storage = transaction_->raw_storage(planned.explain.anchor_component);
        planned.anchor_index = planned.explain.anchor_component_index;

        if (auto seed = build_seed_plan(expression_)) {
            if (seed->candidates.size() < planned.explain.candidate_rows) {
                planned.use_index = true;
                planned.candidates = std::move(seed->candidates);
                planned.anchor_storage = nullptr;
                planned.explain.uses_component_indexes = true;
                mark_index_steps(planned.explain, seed->indexed_component_indices);
            }
        }

        if (planned.use_index) {
            planned.explain.empty = planned.candidates.empty();
            planned.explain.candidate_rows = planned.candidates.size();
            planned.explain.estimated_entity_lookups = planned.candidates.size() * (sizeof...(Components) - 1);
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
        for (QueryAccessStep& step : explain.steps) {
            const bool indexed =
                std::find(indices.begin(), indices.end(), step.component_index) != indices.end();
            if (indexed) {
                if (step.component_index == explain.anchor_component_index) {
                    explain.anchor_component = step.component;
                    explain.anchor_component_name = step.component_name;
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
