#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "view_traits.hpp"

namespace ecs {

template <typename T>
class TransactionStorageView;

template <typename... Components>
class TransactionView;

class Snapshot {
public:
    explicit Snapshot(Registry& registry)
        : registry_(&registry),
          max_visible_tsn_(registry.next_tsn_ - 1),
          active_at_open_(registry.active_transactions_snapshot()) {
        registry_->register_reader();
    }

    ~Snapshot() {
        close();
    }

    Snapshot(Snapshot&& other) noexcept
        : registry_(other.registry_),
          max_visible_tsn_(other.max_visible_tsn_),
          active_at_open_(std::move(other.active_at_open_)) {
        other.registry_ = nullptr;
        other.max_visible_tsn_ = 0;
    }

    Snapshot& operator=(Snapshot&& other) noexcept {
        if (this != &other) {
            close();
            registry_ = other.registry_;
            max_visible_tsn_ = other.max_visible_tsn_;
            active_at_open_ = std::move(other.active_at_open_);
            other.registry_ = nullptr;
            other.max_visible_tsn_ = 0;
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

    const std::vector<Entity>& entities() const {
        require_open();
        return registry_->entities();
    }

protected:
    void require_open() const {
        if (registry_ == nullptr) {
            throw std::logic_error("snapshot is no longer open");
        }
    }

    void close() {
        if (registry_ != nullptr) {
            registry_->unregister_reader();
            registry_ = nullptr;
            max_visible_tsn_ = 0;
            active_at_open_.clear();
        }
    }

    Registry* registry_ = nullptr;
    std::uint64_t max_visible_tsn_ = 0;
    std::vector<std::uint64_t> active_at_open_;
};

class Transaction : public Snapshot {
public:
    explicit Transaction(Registry& registry)
        : Snapshot(registry),
          tsn_(registry.acquire_tsn()) {
        max_visible_tsn_ = tsn_ - 1;
        active_at_open_ = registry.active_transactions_snapshot();
        registry_->register_transaction(tsn_);
    }

    ~Transaction() {
        rollback();
    }

    Transaction(Transaction&& other) noexcept
        : Snapshot(std::move(other)),
          tsn_(other.tsn_),
          writes_(std::move(other.writes_)) {
        other.tsn_ = 0;
    }

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            rollback();
            Snapshot::operator=(std::move(other));
            tsn_ = other.tsn_;
            writes_ = std::move(other.writes_);
            other.tsn_ = 0;
        }
        return *this;
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

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
        return raw == nullptr ? nullptr : raw->try_get_visible_raw(entity, max_visible_tsn_, active_at_open_, tsn_);
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

    template <typename T>
    TransactionStorageView<T> storage() {
        return TransactionStorageView<T>(*this);
    }

    template <typename... Components>
    TransactionView<Components...> view() {
        return TransactionView<Components...>(*this);
    }

    template <typename T>
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

    template <typename T, typename... Args>
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

        for (const auto& write : writes_) {
            registry_->require_alive(write->entity);
        }

        for (auto& write : writes_) {
            write->commit(tsn_);
        }

        writes_.clear();
        registry_->unregister_transaction(tsn_);
        tsn_ = 0;
        close();
    }

    void rollback() {
        if (registry_ == nullptr) {
            return;
        }

        for (auto& write : writes_) {
            write->rollback(tsn_);
        }

        writes_.clear();
        if (tsn_ != 0) {
            registry_->unregister_transaction(tsn_);
        }
        tsn_ = 0;
        close();
    }

private:
    struct PendingWriteBase {
        PendingWriteBase(Entity pending_entity, ComponentId pending_component)
            : entity(pending_entity),
              component(pending_component) {}

        virtual ~PendingWriteBase() = default;
        virtual void commit(std::uint64_t tsn) = 0;
        virtual void rollback(std::uint64_t tsn) = 0;

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

        void commit(std::uint64_t tsn) override {
            storage->commit_staged(this->entity, pending, tsn);
        }

        void rollback(std::uint64_t tsn) override {
            storage->rollback_staged(this->entity, pending, tsn);
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

    std::uint64_t tsn_ = 0;
    std::vector<std::unique_ptr<PendingWriteBase>> writes_;
};

template <typename T>
class TransactionStorageView {
public:
    explicit TransactionStorageView(Transaction& transaction)
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

    std::size_t size() const {
        std::size_t count = 0;
        each([&count](Entity, const T&) {
            ++count;
        });
        return count;
    }

    template <typename IndexSpec, typename... KeyParts>
    std::vector<Entity> find(KeyParts&&... key_parts) const {
        const auto key = detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...);
        std::vector<Entity> matches;
        each([&](Entity entity, const T& component) {
            if (IndexSpec::key(component) == key) {
                matches.push_back(entity);
            }
        });
        return matches;
    }

    template <typename IndexSpec, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        const auto key = detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...);
        Entity match = null_entity;
        each([&](Entity entity, const T& component) {
            if (match == null_entity && IndexSpec::key(component) == key) {
                match = entity;
            }
        });
        return match;
    }

    template <typename Func>
    void each(Func&& func) const {
        for (const Entity entity : transaction_->entities()) {
            if (const T* component = transaction_->template try_get<T>(entity)) {
                func(entity, *component);
            }
        }
    }

private:
    Transaction* transaction_;
};

template <typename... Components>
class TransactionView {
    static_assert(sizeof...(Components) > 0, "views require at least one component type");
    static_assert(detail::unique_component_types<Components...>::value,
                  "views cannot contain duplicate component types");

public:
    explicit TransactionView(Transaction& transaction)
        : transaction_(&transaction) {}

    template <typename Func>
    void forEach(Func&& func) const {
        auto&& callback = func;
        for (const Entity entity : transaction_->entities()) {
            auto components = std::tuple<detail::component_pointer_t<Transaction, Components>...>{
                fetch<Components>(entity)...};

            if (all_present(components, std::index_sequence_for<Components...>{})) {
                invoke(callback, entity, components, std::index_sequence_for<Components...>{});
            }
        }
    }

private:
    template <typename Component>
    detail::component_pointer_t<Transaction, Component> fetch(Entity entity) const {
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

    Transaction* transaction_;
};

}  // namespace ecs
