#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs {

template <typename T>
struct is_singleton_component : std::false_type {};

namespace detail {

template <typename... Components>
struct type_list {};

template <typename Left, typename Right>
struct type_list_concat;

template <typename... Left, typename... Right>
struct type_list_concat<type_list<Left...>, type_list<Right...>> {
    using type = type_list<Left..., Right...>;
};

template <typename T>
using component_query_t = typename std::remove_cv<typename std::remove_reference<T>::type>::type;

template <typename T>
struct is_singleton_query : is_singleton_component<component_query_t<T>> {};

template <typename T>
struct is_tag_query
    : std::integral_constant<
          bool,
          std::is_empty<component_query_t<T>>::value && !is_singleton_query<T>::value> {};

template <typename... Components>
struct contains_non_singleton_component;

template <>
struct contains_non_singleton_component<> : std::false_type {};

template <typename First, typename... Rest>
struct contains_non_singleton_component<First, Rest...>
    : std::conditional<
          is_singleton_query<First>::value,
          contains_non_singleton_component<Rest...>,
          std::true_type>::type {};

template <typename T, typename... Components>
struct contains_component;

template <typename T>
struct contains_component<T> : std::false_type {};

template <typename T, typename First, typename... Rest>
struct contains_component<T, First, Rest...>
    : std::conditional<
          std::is_same<component_query_t<T>, component_query_t<First>>::value,
          std::true_type,
          contains_component<T, Rest...>>::type {};

template <typename... Components>
struct unique_components;

template <>
struct unique_components<> : std::true_type {};

template <typename First, typename... Rest>
struct unique_components<First, Rest...>
    : std::integral_constant<
          bool,
          !contains_component<First, Rest...>::value && unique_components<Rest...>::value> {};

template <typename T, typename... Components>
struct contains_mutable_component;

template <typename T>
struct contains_mutable_component<T> : std::false_type {};

template <typename T, typename First, typename... Rest>
struct contains_mutable_component<T, First, Rest...>
    : std::conditional<
          !std::is_const<typename std::remove_reference<First>::type>::value &&
              std::is_same<component_query_t<T>, component_query_t<First>>::value,
          std::true_type,
          contains_mutable_component<T, Rest...>>::type {};

template <typename... Left>
struct disjoint_from;

template <typename... Left>
struct disjoint_from<type_list<Left...>> {
    template <typename... Right>
    struct with
        : std::integral_constant<bool, (!contains_component<Right, Left...>::value && ...)> {};
};

template <typename Access, typename Iter>
struct access_overlap_allowed_pair
    : std::integral_constant<
          bool,
          !std::is_same<component_query_t<Access>, component_query_t<Iter>>::value ||
              (std::is_const<typename std::remove_reference<Iter>::type>::value &&
               !std::is_const<typename std::remove_reference<Access>::type>::value)> {};

template <typename Access, typename... IterComponents>
struct access_component_allowed
    : std::integral_constant<bool, (access_overlap_allowed_pair<Access, IterComponents>::value && ...)> {};

template <typename IterList>
struct access_components_allowed;

template <typename... IterComponents>
struct access_components_allowed<type_list<IterComponents...>> {
    template <typename... AccessComponents>
    struct with
        : std::integral_constant<bool, (access_component_allowed<AccessComponents, IterComponents...>::value && ...)> {};
};

}  // namespace detail

struct Entity {
    std::uint64_t value = 0;

    constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
};

constexpr bool operator==(Entity lhs, Entity rhs) noexcept {
    return lhs.value == rhs.value;
}

constexpr bool operator!=(Entity lhs, Entity rhs) noexcept {
    return !(lhs == rhs);
}

struct ComponentInfo {
    std::size_t size = 0;
    std::size_t alignment = 1;
    bool trivially_copyable = true;
    bool tag = false;
};

struct ComponentField {
    std::string name;
    std::size_t offset = 0;
    Entity type;
    std::size_t count = 1;
};

struct ComponentDesc {
    std::string name;
    std::size_t size = 0;
    std::size_t alignment = 1;
    std::vector<ComponentField> fields;
};

struct JobScheduleStage {
    std::vector<Entity> jobs;
};

struct JobSchedule {
    std::vector<JobScheduleStage> stages;
};

enum class PrimitiveType {
    Bool,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64
};

class Registry {
    friend class Orchestrator;

public:
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    template <typename... Components>
    class View;

    template <typename IterList, typename... AccessComponents>
    class AccessView;

    template <typename IterList, typename AccessList, typename WithList, typename WithoutList>
    class TagFilteredView;

    template <typename... Components>
    class JobView;

    template <typename IterList, typename... AccessComponents>
    class JobAccessView;

    class Snapshot;
    class DeltaSnapshot;

    Registry() {
        register_system_tag();
        register_primitive_types();
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;
    ~Registry() = default;

    Entity create() {
        if (free_head_ != invalid_index) {
            const std::uint32_t index = free_head_;
            const std::uint64_t slot = entities_[index];
            const std::uint32_t version = slot_version(slot);

            free_head_ = slot_index(slot);
            entities_[index] = pack(index, version);
            return Entity{entities_[index]};
        }

        if (entities_.size() >= invalid_index) {
            throw std::length_error("ecs entity index space exhausted");
        }

        const std::uint32_t index = static_cast<std::uint32_t>(entities_.size());
        entities_.push_back(pack(index, 1));
        return Entity{entities_.back()};
    }

    bool destroy(Entity entity) {
        if (!alive(entity)) {
            return false;
        }

        const std::uint32_t index = entity_index(entity);
        for (auto& storage : storages_) {
            if (storage.second->contains_index(index)) {
                remove_from_groups_before_component_removal(index, storage.first);
                storage.second->remove_index_for_destroy(index);
            }
        }

        unregister_component_entity(entity);

        std::uint32_t next_version = entity_version(entity) + 1;
        if (next_version == 0) {
            next_version = 1;
        }

        entities_[index] = pack(free_head_, next_version);
        free_head_ = index;
        return true;
    }

    bool alive(Entity entity) const {
        const std::uint32_t index = entity_index(entity);
        const std::uint32_t version = entity_version(entity);

        if (version == 0 || index >= entities_.size()) {
            return false;
        }

        const std::uint64_t slot = entities_[index];
        return slot_index(slot) == index && slot_version(slot) == version;
    }

    template <typename T>
    Entity register_component(std::string name = {}) {
        static_assert(
            !is_singleton_component<T>::value || std::is_default_constructible<T>::value,
            "ecs singleton components must be default constructible");

        const std::size_t id = type_id<T>();
        if (id < typed_components_.size() && typed_components_[id]) {
            return typed_components_[id];
        }

        if (name.empty()) {
            name = typeid(T).name();
        }

        ComponentDesc desc;
        desc.name = std::move(name);
        desc.size = detail::is_tag_query<T>::value ? 0 : sizeof(T);
        desc.alignment = alignof(T);

        ComponentLifecycle lifecycle;
        lifecycle.trivially_copyable = std::is_trivially_copyable<T>::value;
        if constexpr (std::is_copy_constructible<T>::value) {
            lifecycle.copy_construct = &copy_construct<T>;
        }
        lifecycle.move_construct = &move_construct<T>;
        lifecycle.destroy = &destroy_value<T>;

        const Entity component = register_component_impl(
            std::move(desc),
            lifecycle,
            std::is_trivially_copyable<T>::value,
            is_singleton_component<T>::value,
            detail::is_tag_query<T>::value,
            id);

        ensure_typed_capacity(id);
        typed_components_[id] = component;
        if constexpr (is_singleton_component<T>::value) {
            storage_for(component).template emplace_or_replace<T>(entity_index(singleton_entity()));
        }
        return component;
    }

    Entity register_component(ComponentDesc desc) {
        ComponentLifecycle lifecycle;
        lifecycle.trivially_copyable = true;
        return register_component_impl(std::move(desc), lifecycle, true, false, false, npos_type_id);
    }

    Entity register_tag(std::string name = {}) {
        ComponentDesc desc;
        desc.name = std::move(name);
        desc.size = 0;
        desc.alignment = 1;

        ComponentLifecycle lifecycle;
        lifecycle.trivially_copyable = true;
        return register_component_impl(std::move(desc), lifecycle, true, false, true, npos_type_id);
    }

    template <typename T>
    Entity component() const {
        return registered_component<T>();
    }

    Entity primitive_type(PrimitiveType type) const {
        return primitive_types_[static_cast<std::size_t>(type)];
    }

    Entity system_tag() const {
        return system_tag_;
    }

    const ComponentInfo* component_info(Entity component) const {
        const ComponentRecord* record = find_component_record(component);
        return record != nullptr ? &record->info : nullptr;
    }

    const std::vector<ComponentField>* component_fields(Entity component) const {
        const ComponentRecord* record = find_component_record(component);
        return record != nullptr ? &record->fields : nullptr;
    }

    bool set_component_fields(Entity component, std::vector<ComponentField> fields) {
        ComponentRecord* record = find_component_record(component);
        if (record == nullptr) {
            return false;
        }

        record->fields = std::move(fields);
        return true;
    }

    bool add_component_field(Entity component, ComponentField field) {
        ComponentRecord* record = find_component_record(component);
        if (record == nullptr) {
            return false;
        }

        record->fields.push_back(std::move(field));
        return true;
    }

    template <
        typename T,
        typename std::enable_if<!is_singleton_component<T>::value && !detail::is_tag_query<T>::value, int>::type = 0,
        typename... Args>
    T* add(Entity entity, Args&&... args) {
        const Entity component = registered_component<T>();
        if (!alive(entity)) {
            return nullptr;
        }

        T* value = storage_for(component).emplace_or_replace<T>(
            entity_index(entity),
            std::forward<Args>(args)...);
        refresh_groups_after_add(entity_index(entity));
        return value;
    }

    template <typename T, typename std::enable_if<detail::is_tag_query<T>::value, int>::type = 0>
    bool add(Entity entity) {
        const Entity component = registered_component<T>();
        return add_tag(entity, component);
    }

    bool add_tag(Entity entity, Entity tag) {
        const ComponentRecord& record = require_component_record(tag);
        if (!record.info.tag) {
            throw std::logic_error("ecs component entity is not a tag");
        }
        if (!alive(entity)) {
            return false;
        }

        storage_for(tag).emplace_or_replace_tag(entity_index(entity));
        refresh_groups_after_add(entity_index(entity));
        return true;
    }

    bool remove_tag(Entity entity, Entity tag) {
        return remove(entity, tag);
    }

    void* add(Entity entity, Entity component, const void* value = nullptr) {
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ecs tags cannot be added as writable components");
        }
        if (record.singleton) {
            return storage_for(component).emplace_or_replace_bytes(entity_index(singleton_entity()), value);
        }
        if (!alive(entity)) {
            return nullptr;
        }

        void* added = storage_for(component).emplace_or_replace_bytes(entity_index(entity), value);
        refresh_groups_after_add(entity_index(entity));
        return added;
    }

    void* ensure(Entity entity, Entity component) {
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ecs tags cannot be ensured as writable components");
        }
        if (record.singleton) {
            return storage_for(component).ensure(entity_index(singleton_entity()));
        }
        if (!alive(entity)) {
            return nullptr;
        }

        void* ensured = storage_for(component).ensure(entity_index(entity));
        refresh_groups_after_add(entity_index(entity));
        return ensured;
    }

    template <typename T, typename std::enable_if<!is_singleton_component<T>::value, int>::type = 0>
    bool remove(Entity entity) {
        const Entity component = registered_component<T>();
        return remove(entity, component);
    }

    bool remove(Entity entity, Entity component) {
        const ComponentRecord& record = require_component_record(component);
        if (record.singleton) {
            return false;
        }
        if (!alive(entity)) {
            return false;
        }

        if (auto* found = find_storage(component)) {
            const std::uint32_t index = entity_index(entity);
            if (!found->contains_index(index)) {
                return false;
            }
            remove_from_groups_before_component_removal(index, entity_index(component));
            return found->remove(index);
        }

        return false;
    }

    template <typename T, typename std::enable_if<!detail::is_tag_query<T>::value, int>::type = 0>
    const T* get(Entity entity) const {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return static_cast<const T*>(get(singleton_entity_, component));
        }
        return static_cast<const T*>(get(entity, component));
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    const T* get() const {
        const Entity component = registered_component<T>();
        return static_cast<const T*>(get(singleton_entity_, component));
    }

    const void* get(Entity entity, Entity component) const {
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ecs tags cannot be read as components");
        }
        if (record.singleton) {
            entity = singleton_entity_;
        }
        if (!alive(entity)) {
            return nullptr;
        }

        const auto* found = find_storage(component);
        return found != nullptr ? found->get(entity_index(entity)) : nullptr;
    }

    template <typename T, typename std::enable_if<!detail::is_tag_query<T>::value, int>::type = 0>
    T* write(Entity entity) {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return static_cast<T*>(write(singleton_entity_, component));
        }
        return static_cast<T*>(write(entity, component));
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    T* write() {
        const Entity component = registered_component<T>();
        return static_cast<T*>(write(singleton_entity_, component));
    }

    void* write(Entity entity, Entity component) {
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ecs tags cannot be written");
        }
        if (record.singleton) {
            entity = singleton_entity_;
        }
        if (!alive(entity)) {
            return nullptr;
        }

        auto* found = find_storage(component);
        return found != nullptr ? found->write(entity_index(entity)) : nullptr;
    }

    template <typename T, typename std::enable_if<detail::is_tag_query<T>::value, int>::type = 0>
    bool has(Entity entity) const {
        const Entity component = registered_component<T>();
        return has(entity, component);
    }

    bool has(Entity entity, Entity component) const {
        const ComponentRecord& record = require_component_record(component);
        if (!record.info.tag) {
            throw std::logic_error("ecs component entity is not a tag");
        }
        if (!alive(entity)) {
            return false;
        }

        const auto* found = find_storage(component);
        return found != nullptr && found->contains_index(entity_index(entity));
    }

    template <typename T>
    bool clear_dirty(Entity entity) {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return clear_dirty(singleton_entity_, component);
        }
        return clear_dirty(entity, component);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool clear_dirty() {
        const Entity component = registered_component<T>();
        return clear_dirty(singleton_entity_, component);
    }

    bool clear_dirty(Entity entity, Entity component) {
        const ComponentRecord& record = require_component_record(component);
        if (record.singleton) {
            entity = singleton_entity_;
        }
        if (!alive(entity)) {
            return false;
        }

        auto* found = find_storage(component);
        return found != nullptr && found->clear_dirty(entity_index(entity));
    }

    template <typename T>
    bool is_dirty(Entity entity) const {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return is_dirty(singleton_entity_, component);
        }
        return is_dirty(entity, component);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool is_dirty() const {
        const Entity component = registered_component<T>();
        return is_dirty(singleton_entity_, component);
    }

    bool is_dirty(Entity entity, Entity component) const {
        const ComponentRecord& record = require_component_record(component);
        if (record.singleton) {
            entity = singleton_entity_;
        }
        if (!alive(entity)) {
            return false;
        }

        const auto* found = find_storage(component);
        return found != nullptr && found->is_dirty(entity_index(entity));
    }

    template <typename T>
    void clear_all_dirty() {
        const Entity component = registered_component<T>();
        clear_all_dirty(component);
    }

    void clear_all_dirty(Entity component) {
        require_component_record(component);
        if (auto* found = find_storage(component)) {
            found->clear_all_dirty();
        }
    }

    template <typename... Components>
    View<Components...> view();

    template <typename... Components>
    JobView<Components...> job(int order);

    void run_jobs() {
        std::vector<std::size_t> ordered(jobs_.size());
        std::iota(ordered.begin(), ordered.end(), std::size_t{0});
        std::sort(ordered.begin(), ordered.end(), [&](std::size_t lhs, std::size_t rhs) {
            const JobRecord& left = jobs_[lhs];
            const JobRecord& right = jobs_[rhs];
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.sequence < right.sequence;
        });

        for (std::size_t index : ordered) {
            jobs_[index].run(*this);
        }
    }

    template <typename... Owned>
    void declare_owned_group();

    Snapshot snapshot() const;
    DeltaSnapshot delta_snapshot(const Snapshot& baseline) const;
    DeltaSnapshot delta_snapshot(const DeltaSnapshot& baseline) const;
    void restore(const Snapshot& snapshot);
    void restore(const DeltaSnapshot& snapshot);

    std::string debug_print(Entity entity, Entity component) const {
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            return has(entity, component) ? record.name + "{}" : "<missing>";
        }
        const void* value = get(entity, component);
        if (value == nullptr) {
            return "<missing>";
        }

        std::ostringstream out;
        out << record.name << "{";

        for (std::size_t i = 0; i < record.fields.size(); ++i) {
            const ComponentField& field = record.fields[i];
            if (i != 0) {
                out << ", ";
            }

            out << field.name << "=";
            print_field(out, static_cast<const unsigned char*>(value) + field.offset, field);
        }

        out << "}";
        return out.str();
    }

    static constexpr std::uint32_t entity_index(Entity entity) noexcept {
        return static_cast<std::uint32_t>(entity.value);
    }

    static constexpr std::uint32_t entity_version(Entity entity) noexcept {
        return static_cast<std::uint32_t>(entity.value >> 32U);
    }

private:
    static constexpr std::size_t npos_type_id = std::numeric_limits<std::size_t>::max();

    enum class PrimitiveKind {
        None,
        Bool,
        I32,
        U32,
        I64,
        U64,
        F32,
        F64
    };

    struct ComponentLifecycle {
        using CopyConstruct = void (*)(void*, const void*);
        using MoveConstruct = void (*)(void*, void*);
        using Destroy = void (*)(void*);

        bool trivially_copyable = true;
        CopyConstruct copy_construct = nullptr;
        MoveConstruct move_construct = nullptr;
        Destroy destroy = nullptr;
    };

    struct ComponentRecord {
        Entity entity;
        std::string name;
        ComponentInfo info;
        std::vector<ComponentField> fields;
        ComponentLifecycle lifecycle;
        std::size_t type_id = npos_type_id;
        PrimitiveKind primitive = PrimitiveKind::None;
        bool singleton = false;
    };

    class TypeErasedStorage {
    public:
        static constexpr std::uint32_t npos = std::numeric_limits<std::uint32_t>::max();

        explicit TypeErasedStorage(const ComponentRecord& record)
            : info_(record.info), lifecycle_(record.lifecycle) {}

        TypeErasedStorage(const TypeErasedStorage& other)
            : dense_indices_(other.dense_indices_),
              sparse_(other.sparse_),
              dirty_(other.dirty_),
              tombstones_(other.tombstones_),
              info_(other.info_),
              lifecycle_(other.lifecycle_) {
            copy_from(other);
        }

        TypeErasedStorage& operator=(const TypeErasedStorage& other) {
            if (this != &other) {
                TypeErasedStorage copy(other);
                *this = std::move(copy);
            }

            return *this;
        }

        TypeErasedStorage(TypeErasedStorage&& other) noexcept
            : dense_indices_(std::move(other.dense_indices_)),
              sparse_(std::move(other.sparse_)),
              dirty_(std::move(other.dirty_)),
              tombstones_(std::move(other.tombstones_)),
              data_(other.data_),
              size_(other.size_),
              capacity_(other.capacity_),
              info_(other.info_),
              lifecycle_(other.lifecycle_) {
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }

        TypeErasedStorage& operator=(TypeErasedStorage&& other) noexcept {
            if (this != &other) {
                clear();
                deallocate(data_, info_.alignment);
                dense_indices_ = std::move(other.dense_indices_);
                sparse_ = std::move(other.sparse_);
                dirty_ = std::move(other.dirty_);
                tombstones_ = std::move(other.tombstones_);
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                info_ = other.info_;
                lifecycle_ = other.lifecycle_;
                other.data_ = nullptr;
                other.size_ = 0;
                other.capacity_ = 0;
            }

            return *this;
        }

        ~TypeErasedStorage() {
            clear();
            deallocate(data_, info_.alignment);
        }

        template <typename T, typename... Args>
        T* emplace_or_replace(std::uint32_t index, Args&&... args) {
            if (info_.tag) {
                throw std::logic_error("ecs tags do not store component values");
            }
            validate_type<T>();

            if (void* existing = get(index)) {
                dirty_[sparse_[index]] = true;

                if constexpr (std::is_trivially_copyable<T>::value) {
                    T replacement(std::forward<Args>(args)...);
                    std::memcpy(existing, &replacement, sizeof(T));
                } else {
                    T replacement(std::forward<Args>(args)...);
                    static_cast<T*>(existing)->~T();
                    new (existing) T(std::move(replacement));
                }

                return static_cast<T*>(existing);
            }

            ensure_sparse(index);
            tombstones_[index] = no_tombstone;
            ensure_capacity(size_ + 1);

            const std::uint32_t dense = static_cast<std::uint32_t>(size_);
            dense_indices_.push_back(index);
            dirty_.push_back(true);
            sparse_[index] = dense;
            void* target = data_ + size_ * info_.size;
            new (target) T(std::forward<Args>(args)...);
            ++size_;

            return static_cast<T*>(target);
        }

        void emplace_or_replace_tag(std::uint32_t index) {
            if (!info_.tag) {
                throw std::logic_error("ecs component storage is not a tag");
            }

            if (contains(index)) {
                dirty_[sparse_[index]] = true;
                return;
            }

            ensure_sparse(index);
            tombstones_[index] = no_tombstone;

            const std::uint32_t dense = static_cast<std::uint32_t>(size_);
            dense_indices_.push_back(index);
            dirty_.push_back(true);
            sparse_[index] = dense;
            ++size_;
        }

        void* emplace_or_replace_bytes(std::uint32_t index, const void* value) {
            if (info_.tag) {
                throw std::logic_error("ecs tags do not store component values");
            }
            if (!info_.trivially_copyable) {
                throw std::logic_error("runtime byte add requires a trivially copyable component");
            }

            if (void* existing = get(index)) {
                dirty_[sparse_[index]] = true;
                assign_bytes(existing, value);
                return existing;
            }

            ensure_sparse(index);
            tombstones_[index] = no_tombstone;
            ensure_capacity(size_ + 1);

            const std::uint32_t dense = static_cast<std::uint32_t>(size_);
            dense_indices_.push_back(index);
            dirty_.push_back(true);
            sparse_[index] = dense;
            void* target = data_ + size_ * info_.size;
            assign_bytes(target, value);
            ++size_;
            return target;
        }

        void emplace_or_replace_copy(std::uint32_t index, const void* value) {
            if (info_.tag) {
                emplace_or_replace_tag(index);
                return;
            }
            if (void* existing = get(index)) {
                dirty_[sparse_[index]] = true;
                replace_copy(existing, value);
                return;
            }

            ensure_sparse(index);
            tombstones_[index] = no_tombstone;
            ensure_capacity(size_ + 1);

            const std::uint32_t dense = static_cast<std::uint32_t>(size_);
            dense_indices_.push_back(index);
            dirty_.push_back(true);
            sparse_[index] = dense;
            construct_copy(data_ + size_ * info_.size, value);
            ++size_;
        }

        void* ensure(std::uint32_t index) {
            if (info_.tag) {
                throw std::logic_error("ecs tags do not store component values");
            }
            if (void* existing = write(index)) {
                return existing;
            }

            return emplace_or_replace_bytes(index, nullptr);
        }

        bool remove(std::uint32_t index) {
            if (!contains(index)) {
                return false;
            }

            erase_at(sparse_[index]);
            mark_tombstone(index, tombstone_dirty);
            return true;
        }

        void remove_index(std::uint32_t index) {
            (void)remove(index);
        }

        void remove_index_for_destroy(std::uint32_t index) {
            if (!contains(index)) {
                return;
            }

            erase_at(sparse_[index]);
            mark_tombstone(index, tombstone_dirty | tombstone_destroy_entity);
        }

        void* get(std::uint32_t index) {
            if (!contains(index)) {
                return nullptr;
            }
            if (info_.tag) {
                return nullptr;
            }

            return data_ + sparse_[index] * info_.size;
        }

        const void* get(std::uint32_t index) const {
            if (!contains(index)) {
                return nullptr;
            }
            if (info_.tag) {
                return nullptr;
            }

            return data_ + sparse_[index] * info_.size;
        }

        void* write(std::uint32_t index) {
            if (!contains(index)) {
                return nullptr;
            }

            const std::uint32_t dense = sparse_[index];
            dirty_[dense] = true;
            if (info_.tag) {
                return nullptr;
            }
            return data_ + dense * info_.size;
        }

        bool clear_dirty(std::uint32_t index) {
            if (!contains(index)) {
                if (has_tombstone(index)) {
                    tombstones_[index] = no_tombstone;
                    return true;
                }
                return false;
            }

            dirty_[sparse_[index]] = false;
            return true;
        }

        bool is_dirty(std::uint32_t index) const {
            if (!contains(index)) {
                return has_tombstone(index);
            }

            return dirty_[sparse_[index]];
        }

        void clear_all_dirty() {
            std::fill(dirty_.begin(), dirty_.end(), false);
            std::fill(tombstones_.begin(), tombstones_.end(), no_tombstone);
        }

        std::size_t dense_size() const noexcept {
            return size_;
        }

        std::uint32_t dense_index_at(std::size_t dense) const {
            return dense_indices_[dense];
        }

        std::uint32_t dense_position(std::uint32_t index) const {
            return contains(index) ? sparse_[index] : npos;
        }

        bool contains_index(std::uint32_t index) const {
            return contains(index);
        }

        bool has_dirty_entries() const {
            for (bool dirty : dirty_) {
                if (dirty) {
                    return true;
                }
            }
            for (unsigned char tombstone : tombstones_) {
                if (tombstone != no_tombstone) {
                    return true;
                }
            }
            return false;
        }

        bool has_destroy_tombstone(std::uint32_t index) const {
            return has_tombstone(index) && (tombstones_[index] & tombstone_destroy_entity) != 0;
        }

        std::size_t tombstone_size() const noexcept {
            return tombstones_.size();
        }

        bool has_dirty_tombstone_at(std::uint32_t index) const {
            return has_tombstone(index);
        }

        void swap_dense(std::uint32_t lhs, std::uint32_t rhs) {
            if (lhs == rhs) {
                return;
            }
            if (lhs >= size_ || rhs >= size_) {
                throw std::out_of_range("ecs dense storage swap index is out of range");
            }

            if (!info_.tag) {
                unsigned char* lhs_value = data_ + lhs * info_.size;
                unsigned char* rhs_value = data_ + rhs * info_.size;

                if (info_.trivially_copyable) {
                    std::vector<unsigned char> temp(info_.size);
                    std::memcpy(temp.data(), lhs_value, info_.size);
                    std::memcpy(lhs_value, rhs_value, info_.size);
                    std::memcpy(rhs_value, temp.data(), info_.size);
                } else {
                    unsigned char* temp = allocate(1, info_);
                    try {
                        lifecycle_.move_construct(temp, lhs_value);
                        lifecycle_.destroy(lhs_value);
                        lifecycle_.move_construct(lhs_value, rhs_value);
                        lifecycle_.destroy(rhs_value);
                        lifecycle_.move_construct(rhs_value, temp);
                        lifecycle_.destroy(temp);
                    } catch (...) {
                        deallocate(temp, info_.alignment);
                        throw;
                    }
                    deallocate(temp, info_.alignment);
                }
            }

            const std::uint32_t lhs_index = dense_indices_[lhs];
            const std::uint32_t rhs_index = dense_indices_[rhs];
            dense_indices_[lhs] = rhs_index;
            dense_indices_[rhs] = lhs_index;
            sparse_[lhs_index] = rhs;
            sparse_[rhs_index] = lhs;
            const bool lhs_dirty = dirty_[lhs];
            dirty_[lhs] = dirty_[rhs];
            dirty_[rhs] = lhs_dirty;
        }

        void move_index_to_dense(std::uint32_t index, std::uint32_t dense) {
            const std::uint32_t current = dense_position(index);
            if (current == npos) {
                return;
            }
            swap_dense(current, dense);
        }

        std::unique_ptr<TypeErasedStorage> clone() const {
            return std::make_unique<TypeErasedStorage>(*this);
        }

        std::unique_ptr<TypeErasedStorage> clone_dirty() const {
            auto copy = std::make_unique<TypeErasedStorage>(*this);
            for (std::size_t dense = copy->size_; dense > 0; --dense) {
                const std::uint32_t position = static_cast<std::uint32_t>(dense - 1);
                if (!copy->dirty_[position]) {
                    copy->erase_at(position);
                }
            }
            return copy;
        }

        std::unique_ptr<TypeErasedStorage> clone_excluding(const std::vector<bool>& excluded) const {
            auto copy = clone_filtered(excluded, false);
            copy->copy_tombstones_excluding(tombstones_, excluded);
            return copy;
        }

        std::unique_ptr<TypeErasedStorage> clone_dirty_excluding(const std::vector<bool>& excluded) const {
            auto copy = clone_filtered(excluded, true);
            copy->copy_tombstones_excluding(tombstones_, excluded);
            return copy;
        }

    private:
        TypeErasedStorage(ComponentInfo info, ComponentLifecycle lifecycle)
            : info_(info), lifecycle_(lifecycle) {}

        template <typename T>
        void validate_type() const {
            if (sizeof(T) != info_.size || alignof(T) != info_.alignment) {
                throw std::logic_error("registered component metadata does not match T");
            }
        }

        static unsigned char* allocate(std::size_t capacity, const ComponentInfo& info) {
            if (capacity == 0 || info.tag) {
                return nullptr;
            }

            return static_cast<unsigned char*>(
                ::operator new(info.size * capacity, std::align_val_t{info.alignment}));
        }

        static void deallocate(unsigned char* data, std::size_t alignment) noexcept {
            if (data != nullptr) {
                ::operator delete(data, std::align_val_t{alignment});
            }
        }

        void assign_bytes(void* target, const void* value) {
            if (info_.tag) {
                throw std::logic_error("ecs tags do not store component values");
            }
            if (value != nullptr) {
                std::memcpy(target, value, info_.size);
            } else {
                std::memset(target, 0, info_.size);
            }
        }

        void construct_copy(void* target, const void* value) {
            if (info_.tag) {
                return;
            }
            if (info_.trivially_copyable) {
                std::memcpy(target, value, info_.size);
                return;
            }
            if (lifecycle_.copy_construct == nullptr) {
                throw std::logic_error("ecs component storage is not copyable");
            }
            lifecycle_.copy_construct(target, value);
        }

        void replace_copy(void* target, const void* value) {
            if (info_.tag) {
                return;
            }
            if (info_.trivially_copyable) {
                std::memcpy(target, value, info_.size);
                return;
            }
            if (lifecycle_.copy_construct == nullptr) {
                throw std::logic_error("ecs component storage is not copyable");
            }
            unsigned char* replacement = allocate(1, info_);
            bool replacement_constructed = false;
            try {
                lifecycle_.copy_construct(replacement, value);
                replacement_constructed = true;
                lifecycle_.destroy(target);
                lifecycle_.move_construct(target, replacement);
                lifecycle_.destroy(replacement);
                replacement_constructed = false;
            } catch (...) {
                if (replacement_constructed) {
                    lifecycle_.destroy(replacement);
                }
                deallocate(replacement, info_.alignment);
                throw;
            }
            deallocate(replacement, info_.alignment);
        }

        void ensure_sparse(std::uint32_t index) {
            if (index >= sparse_.size()) {
                sparse_.resize(static_cast<std::size_t>(index) + 1, npos);
                tombstones_.resize(static_cast<std::size_t>(index) + 1, no_tombstone);
            }
        }

        bool contains(std::uint32_t index) const {
            if (index >= sparse_.size()) {
                return false;
            }

            const std::uint32_t dense = sparse_[index];
            return dense != npos && dense < size_ && dense_indices_[dense] == index;
        }

        void ensure_capacity(std::size_t required) {
            if (info_.tag) {
                capacity_ = std::max(capacity_, required);
                return;
            }
            if (required <= capacity_) {
                return;
            }

            const std::size_t next_capacity = std::max<std::size_t>(required, capacity_ == 0 ? 8 : capacity_ * 2);
            unsigned char* next = allocate(next_capacity, info_);

            if (info_.trivially_copyable) {
                if (size_ != 0) {
                    std::memcpy(next, data_, info_.size * size_);
                }
            } else {
                std::size_t constructed = 0;
                try {
                    for (; constructed < size_; ++constructed) {
                        lifecycle_.move_construct(
                            next + constructed * info_.size,
                            data_ + constructed * info_.size);
                    }
                } catch (...) {
                    for (std::size_t i = 0; i < constructed; ++i) {
                        lifecycle_.destroy(next + i * info_.size);
                    }
                    deallocate(next, info_.alignment);
                    throw;
                }

                for (std::size_t i = 0; i < size_; ++i) {
                    lifecycle_.destroy(data_ + i * info_.size);
                }
            }

            deallocate(data_, info_.alignment);
            data_ = next;
            capacity_ = next_capacity;
        }

        void erase_at(std::uint32_t dense) {
            const std::uint32_t removed_index = dense_indices_[dense];
            const std::uint32_t last_dense = static_cast<std::uint32_t>(size_ - 1);

            if (dense != last_dense) {
                const std::uint32_t moved_index = dense_indices_[last_dense];

                if (!info_.tag) {
                    unsigned char* target = data_ + dense * info_.size;
                    unsigned char* last = data_ + last_dense * info_.size;
                    if (info_.trivially_copyable) {
                        std::memcpy(target, last, info_.size);
                    } else {
                        lifecycle_.destroy(target);
                        lifecycle_.move_construct(target, last);
                        lifecycle_.destroy(last);
                    }
                }

                dense_indices_[dense] = moved_index;
                dirty_[dense] = dirty_[last_dense];
                sparse_[moved_index] = dense;
            } else if (!info_.tag && !info_.trivially_copyable) {
                unsigned char* target = data_ + dense * info_.size;
                lifecycle_.destroy(target);
            }

            sparse_[removed_index] = npos;
            dense_indices_.pop_back();
            dirty_.pop_back();
            --size_;
        }

        void clear() noexcept {
            if (!info_.tag && !info_.trivially_copyable) {
                for (std::size_t i = 0; i < size_; ++i) {
                    lifecycle_.destroy(data_ + i * info_.size);
                }
            }

            size_ = 0;
            dense_indices_.clear();
            dirty_.clear();
            std::fill(sparse_.begin(), sparse_.end(), npos);
            std::fill(tombstones_.begin(), tombstones_.end(), no_tombstone);
        }

        void mark_tombstone(std::uint32_t index, unsigned char flags) {
            ensure_sparse(index);
            tombstones_[index] = flags;
        }

        bool has_tombstone(std::uint32_t index) const {
            return index < tombstones_.size() && tombstones_[index] != no_tombstone;
        }

        std::unique_ptr<TypeErasedStorage> clone_filtered(
            const std::vector<bool>& excluded,
            bool dirty_only) const {
            auto copy = std::unique_ptr<TypeErasedStorage>(new TypeErasedStorage(info_, lifecycle_));
            for (std::size_t dense = 0; dense < size_; ++dense) {
                const std::uint32_t index = dense_indices_[dense];
                if ((index < excluded.size() && excluded[index]) || (dirty_only && !dirty_[dense])) {
                    continue;
                }

                copy->emplace_or_replace_copy(index, get(index));
                copy->dirty_[copy->sparse_[index]] = dirty_[dense];
            }
            return copy;
        }

        void copy_tombstones_excluding(
            const std::vector<unsigned char>& tombstones,
            const std::vector<bool>& excluded) {
            tombstones_ = tombstones;
            for (std::size_t index = 0; index < tombstones_.size() && index < excluded.size(); ++index) {
                if (excluded[index]) {
                    tombstones_[index] = no_tombstone;
                }
            }
        }

        void copy_from(const TypeErasedStorage& other) {
            size_ = other.size_;
            capacity_ = other.capacity_;
            if (capacity_ == 0 || info_.tag) {
                return;
            }

            data_ = allocate(capacity_, info_);
            if (info_.trivially_copyable) {
                if (size_ != 0) {
                    std::memcpy(data_, other.data_, info_.size * size_);
                }
                return;
            }

            if (lifecycle_.copy_construct == nullptr) {
                deallocate(data_, info_.alignment);
                data_ = nullptr;
                size_ = 0;
                capacity_ = 0;
                throw std::logic_error("ecs component storage is not copyable");
            }

            std::size_t constructed = 0;
            try {
                for (; constructed < size_; ++constructed) {
                    lifecycle_.copy_construct(
                        data_ + constructed * info_.size,
                        other.data_ + constructed * info_.size);
                }
            } catch (...) {
                for (std::size_t i = 0; i < constructed; ++i) {
                    lifecycle_.destroy(data_ + i * info_.size);
                }
                deallocate(data_, info_.alignment);
                data_ = nullptr;
                size_ = 0;
                capacity_ = 0;
                throw;
            }
        }

        std::vector<std::uint32_t> dense_indices_;
        std::vector<std::uint32_t> sparse_;
        std::vector<bool> dirty_;
        std::vector<unsigned char> tombstones_;
        unsigned char* data_ = nullptr;
        std::size_t size_ = 0;
        std::size_t capacity_ = 0;
        ComponentInfo info_;
        ComponentLifecycle lifecycle_;

        static constexpr unsigned char no_tombstone = 0;
        static constexpr unsigned char tombstone_dirty = 1U;
        static constexpr unsigned char tombstone_destroy_entity = 2U;
    };

    struct GroupRecord {
        std::vector<std::uint32_t> owned;
        std::size_t size = 0;
    };

    struct JobRecord {
        Entity entity;
        int order = 0;
        std::uint64_t sequence = 0;
        std::vector<std::uint32_t> reads;
        std::vector<std::uint32_t> writes;
        std::function<void(Registry&)> run;
    };

    struct JobAccessMetadata {
        std::vector<std::uint32_t> reads;
        std::vector<std::uint32_t> writes;
    };

    static void append_unique_component(std::vector<std::uint32_t>& components, std::uint32_t component) {
        if (std::find(components.begin(), components.end(), component) == components.end()) {
            components.push_back(component);
        }
    }

    static void remove_written_reads(JobAccessMetadata& metadata) {
        metadata.reads.erase(
            std::remove_if(
                metadata.reads.begin(),
                metadata.reads.end(),
                [&](std::uint32_t read) {
                    return std::find(metadata.writes.begin(), metadata.writes.end(), read) != metadata.writes.end();
                }),
            metadata.reads.end());
    }

    template <typename T>
    void append_job_access_component(JobAccessMetadata& metadata) const {
        const Entity component = registered_component<detail::component_query_t<T>>();
        const std::uint32_t component_index = entity_index(component);
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            append_unique_component(metadata.reads, component_index);
        } else {
            append_unique_component(metadata.writes, component_index);
        }
    }

    template <typename... Components>
    JobAccessMetadata make_job_access_metadata() const {
        JobAccessMetadata metadata;
        (append_job_access_component<Components>(metadata), ...);
        remove_written_reads(metadata);
        return metadata;
    }

    Entity add_job(int order, JobAccessMetadata metadata, std::function<void(Registry&)> run) {
        const Entity entity = create();
        add_system_tag(entity);
        jobs_.push_back(JobRecord{
            entity,
            order,
            next_job_sequence_++,
            std::move(metadata.reads),
            std::move(metadata.writes),
            std::move(run)});
        return entity;
    }

    template <typename... Owned>
    static std::vector<std::uint32_t> make_group_key(const Registry& registry) {
        std::vector<std::uint32_t> key{
            entity_index(registry.registered_component<detail::component_query_t<Owned>>())...};
        std::sort(key.begin(), key.end());
        return key;
    }

    template <typename T>
    void append_view_component_key(std::vector<std::uint32_t>& key) const {
        if constexpr (!detail::is_singleton_query<T>::value) {
            key.push_back(entity_index(registered_component<detail::component_query_t<T>>()));
        }
    }

    template <typename... Components>
    GroupRecord* best_group_for_view() {
        std::vector<std::uint32_t> key;
        key.reserve(sizeof...(Components));
        (append_view_component_key<Components>(key), ...);
        if (key.empty()) {
            return nullptr;
        }

        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());

        GroupRecord* best = nullptr;
        for (const auto& group : groups_) {
            if (!includes_all(key, group->owned)) {
                continue;
            }
            if (best == nullptr ||
                group->owned.size() > best->owned.size() ||
                (group->owned.size() == best->owned.size() && group->size < best->size)) {
                best = group.get();
            }
        }
        return best;
    }

    static bool includes_all(const std::vector<std::uint32_t>& lhs, const std::vector<std::uint32_t>& rhs) {
        return std::includes(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

    static bool intersects(const std::vector<std::uint32_t>& lhs, const std::vector<std::uint32_t>& rhs) {
        auto left = lhs.begin();
        auto right = rhs.begin();
        while (left != lhs.end() && right != rhs.end()) {
            if (*left == *right) {
                return true;
            }
            if (*left < *right) {
                ++left;
            } else {
                ++right;
            }
        }
        return false;
    }

    GroupRecord& group_for_key(const std::vector<std::uint32_t>& key) {
        for (const auto& group : groups_) {
            if (group->owned == key) {
                return *group;
            }
        }

        validate_group_key(key);
        auto group = std::make_unique<GroupRecord>();
        group->owned = key;
        groups_.push_back(std::move(group));
        GroupRecord& created = *groups_.back();
        build_group(created);
        return created;
    }

    void validate_group_key(const std::vector<std::uint32_t>& key) const {
        for (const auto& group : groups_) {
            if (!intersects(key, group->owned)) {
                continue;
            }
            if (!includes_all(key, group->owned) && !includes_all(group->owned, key)) {
                throw std::logic_error("ecs owned groups with shared components must be nested");
            }
        }
    }

    void build_group(GroupRecord& group) {
        group.size = 0;
        TypeErasedStorage* driver = smallest_storage(group);
        if (driver == nullptr) {
            return;
        }

        const std::size_t dense_size = driver->dense_size();
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (group_contains_all(group, index)) {
                enter_group(group, index);
            }
        }
    }

    TypeErasedStorage* smallest_storage(const GroupRecord& group) {
        TypeErasedStorage* driver = nullptr;
        for (std::uint32_t component : group.owned) {
            TypeErasedStorage* storage = find_storage(Entity{entities_[component]});
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }
        return driver;
    }

    bool group_contains_all(const GroupRecord& group, std::uint32_t index) const {
        for (std::uint32_t component : group.owned) {
            const TypeErasedStorage* storage = find_storage(Entity{entities_[component]});
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        return true;
    }

    bool group_contains_component(const GroupRecord& group, std::uint32_t component) const {
        return std::binary_search(group.owned.begin(), group.owned.end(), component);
    }

    bool group_contains_index(const GroupRecord& group, std::uint32_t index) const {
        if (group.owned.empty() || group.size == 0) {
            return false;
        }
        const TypeErasedStorage* storage = find_storage(Entity{entities_[group.owned.front()]});
        if (storage == nullptr) {
            return false;
        }
        const std::uint32_t dense = storage->dense_position(index);
        return dense != TypeErasedStorage::npos && dense < group.size;
    }

    void enter_group(GroupRecord& group, std::uint32_t index) {
        if (group_contains_index(group, index)) {
            return;
        }
        const std::uint32_t target = static_cast<std::uint32_t>(group.size);
        for (std::uint32_t component : group.owned) {
            storage_for(Entity{entities_[component]}).move_index_to_dense(index, target);
        }
        ++group.size;
    }

    void leave_group(GroupRecord& group, std::uint32_t index) {
        if (!group_contains_index(group, index)) {
            return;
        }
        --group.size;
        const std::uint32_t target = static_cast<std::uint32_t>(group.size);
        for (std::uint32_t component : group.owned) {
            storage_for(Entity{entities_[component]}).move_index_to_dense(index, target);
        }
    }

    void refresh_groups_after_add(std::uint32_t index) {
        std::sort(groups_.begin(), groups_.end(), [](const auto& lhs, const auto& rhs) {
            return lhs->owned.size() < rhs->owned.size();
        });
        for (const auto& group : groups_) {
            if (group_contains_all(*group, index)) {
                enter_group(*group, index);
            }
        }
    }

    void remove_from_groups_before_component_removal(std::uint32_t index, std::uint32_t component) {
        std::sort(groups_.begin(), groups_.end(), [](const auto& lhs, const auto& rhs) {
            return lhs->owned.size() > rhs->owned.size();
        });
        for (const auto& group : groups_) {
            if (group_contains_component(*group, component)) {
                leave_group(*group, index);
            }
        }
    }

    static constexpr std::uint64_t pack(std::uint32_t index, std::uint32_t version) noexcept {
        return (static_cast<std::uint64_t>(version) << 32U) | index;
    }

    static constexpr std::uint32_t slot_index(std::uint64_t slot) noexcept {
        return static_cast<std::uint32_t>(slot);
    }

    static constexpr std::uint32_t slot_version(std::uint64_t slot) noexcept {
        return static_cast<std::uint32_t>(slot >> 32U);
    }

    template <typename T>
    static void copy_construct(void* target, const void* source) {
        new (target) T(*static_cast<const T*>(source));
    }

    template <typename T>
    static void move_construct(void* target, void* source) {
        new (target) T(std::move(*static_cast<T*>(source)));
    }

    template <typename T>
    static void destroy_value(void* value) {
        static_cast<T*>(value)->~T();
    }

    static std::size_t next_type_id() {
        static std::atomic<std::size_t> next{0};
        return next++;
    }

    static std::uint64_t next_state_token() {
        static std::atomic<std::uint64_t> next{1};
        return next++;
    }

    template <typename T>
    static std::size_t type_id() {
        static const std::size_t id = next_type_id();
        return id;
    }

    void ensure_typed_capacity(std::size_t id) {
        if (id >= typed_components_.size()) {
            typed_components_.resize(id + 1);
        }
    }

    template <typename T>
    Entity registered_component() const {
        const std::size_t id = type_id<T>();
        if (id >= typed_components_.size() || !typed_components_[id]) {
            throw std::logic_error("ecs component type is not registered");
        }

        return typed_components_[id];
    }

    Entity singleton_entity() {
        if (!singleton_entity_) {
            singleton_entity_ = create();
        }
        return singleton_entity_;
    }

    Entity register_component_impl(
        ComponentDesc desc,
        ComponentLifecycle lifecycle,
        bool trivially_copyable,
        bool singleton,
        bool tag,
        std::size_t typed_id) {
        if (desc.size == 0 && !tag) {
            throw std::invalid_argument("component size must be greater than zero");
        }
        if (desc.alignment == 0) {
            throw std::invalid_argument("component alignment must be greater than zero");
        }

        if (!desc.name.empty()) {
            const auto by_name = component_names_.find(desc.name);
            if (by_name != component_names_.end()) {
                const Entity existing = components_.at(by_name->second).entity;
                ComponentRecord& record = components_.at(by_name->second);
                if (record.info.size != desc.size ||
                    record.info.alignment != desc.alignment ||
                    record.info.trivially_copyable != trivially_copyable ||
                    record.info.tag != tag ||
                    record.singleton != singleton) {
                    throw std::logic_error("component name is already registered with different metadata");
                }

                if (typed_id != npos_type_id) {
                    ensure_typed_capacity(typed_id);
                    typed_components_[typed_id] = existing;
                    record.type_id = typed_id;
                    record.lifecycle = lifecycle;
                }

                return existing;
            }
        }

        const Entity component = create();
        ComponentRecord record;
        record.entity = component;
        record.name = std::move(desc.name);
        record.info = ComponentInfo{desc.size, desc.alignment, trivially_copyable, tag};
        record.fields = std::move(desc.fields);
        record.lifecycle = lifecycle;
        record.type_id = typed_id;
        record.singleton = singleton;

        const std::uint32_t index = entity_index(component);
        if (!record.name.empty()) {
            component_names_[record.name] = index;
        }
        components_[index] = std::move(record);
        return component;
    }

    ComponentRecord* find_component_record(Entity component) {
        if (!alive(component)) {
            return nullptr;
        }

        auto found = components_.find(entity_index(component));
        if (found == components_.end() || found->second.entity != component) {
            return nullptr;
        }

        return &found->second;
    }

    const ComponentRecord* find_component_record(Entity component) const {
        if (!alive(component)) {
            return nullptr;
        }

        auto found = components_.find(entity_index(component));
        if (found == components_.end() || found->second.entity != component) {
            return nullptr;
        }

        return &found->second;
    }

    ComponentRecord& require_component_record(Entity component) {
        ComponentRecord* record = find_component_record(component);
        if (record == nullptr) {
            throw std::logic_error("ecs component entity is not registered");
        }

        return *record;
    }

    const ComponentRecord& require_component_record(Entity component) const {
        const ComponentRecord* record = find_component_record(component);
        if (record == nullptr) {
            throw std::logic_error("ecs component entity is not registered");
        }

        return *record;
    }

    TypeErasedStorage& storage_for(Entity component) {
        const ComponentRecord& record = require_component_record(component);
        const std::uint32_t component_index = entity_index(component);

        auto found = storages_.find(component_index);
        if (found == storages_.end()) {
            auto inserted = storages_.emplace(component_index, std::make_unique<TypeErasedStorage>(record));
            found = inserted.first;
        }

        return *found->second;
    }

    TypeErasedStorage* find_storage(Entity component) {
        auto found = storages_.find(entity_index(component));
        return found != storages_.end() ? found->second.get() : nullptr;
    }

    const TypeErasedStorage* find_storage(Entity component) const {
        auto found = storages_.find(entity_index(component));
        return found != storages_.end() ? found->second.get() : nullptr;
    }

    void require_tag_component(Entity component) const {
        const ComponentRecord& record = require_component_record(component);
        if (!record.info.tag) {
            throw std::logic_error("ecs component entity is not a tag");
        }
    }

    void unregister_component_entity(Entity component) {
        ComponentRecord* record = find_component_record(component);
        if (record == nullptr) {
            return;
        }

        const std::uint32_t component_index = entity_index(component);
        groups_.erase(
            std::remove_if(groups_.begin(), groups_.end(), [&](const auto& group) {
                return group_contains_component(*group, component_index);
            }),
            groups_.end());
        storages_.erase(component_index);

        if (!record->name.empty()) {
            component_names_.erase(record->name);
        }

        if (record->type_id != npos_type_id && record->type_id < typed_components_.size()) {
            typed_components_[record->type_id] = Entity{};
        }

        for (Entity& cached : typed_components_) {
            if (cached == component) {
                cached = Entity{};
            }
        }

        for (Entity& primitive : primitive_types_) {
            if (primitive == component) {
                primitive = Entity{};
            }
        }

        components_.erase(component_index);
    }

    void register_primitive_types() {
        primitive_types_[static_cast<std::size_t>(PrimitiveType::Bool)] =
            register_primitive("bool", sizeof(bool), alignof(bool), PrimitiveKind::Bool);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::I32)] =
            register_primitive("i32", sizeof(std::int32_t), alignof(std::int32_t), PrimitiveKind::I32);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::U32)] =
            register_primitive("u32", sizeof(std::uint32_t), alignof(std::uint32_t), PrimitiveKind::U32);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::I64)] =
            register_primitive("i64", sizeof(std::int64_t), alignof(std::int64_t), PrimitiveKind::I64);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::U64)] =
            register_primitive("u64", sizeof(std::uint64_t), alignof(std::uint64_t), PrimitiveKind::U64);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::F32)] =
            register_primitive("f32", sizeof(float), alignof(float), PrimitiveKind::F32);
        primitive_types_[static_cast<std::size_t>(PrimitiveType::F64)] =
            register_primitive("f64", sizeof(double), alignof(double), PrimitiveKind::F64);
    }

    Entity register_primitive(std::string name, std::size_t size, std::size_t alignment, PrimitiveKind kind) {
        ComponentDesc desc;
        desc.name = std::move(name);
        desc.size = size;
        desc.alignment = alignment;

        ComponentLifecycle lifecycle;
        lifecycle.trivially_copyable = true;

        const Entity type = register_component_impl(std::move(desc), lifecycle, true, false, false, npos_type_id);
        components_[entity_index(type)].primitive = kind;
        add_system_tag(type);
        return type;
    }

    void register_system_tag() {
        ComponentDesc desc;
        desc.name = "ecs.system";
        desc.size = 0;
        desc.alignment = 1;

        ComponentLifecycle lifecycle;
        lifecycle.trivially_copyable = true;

        system_tag_ = register_component_impl(std::move(desc), lifecycle, true, false, true, npos_type_id);
        add_system_tag(system_tag_);
    }

    void add_system_tag(Entity entity) {
        if (!system_tag_ || !alive(entity)) {
            return;
        }
        storage_for(system_tag_).emplace_or_replace_tag(entity_index(entity));
    }

    bool is_snapshot_excluded_index(std::uint32_t index) const {
        if (!system_tag_ || components_.find(index) != components_.end()) {
            return false;
        }

        const TypeErasedStorage* system_storage = find_storage(system_tag_);
        return system_storage != nullptr && system_storage->contains_index(index);
    }

    std::vector<bool> make_snapshot_exclusion_mask() const {
        std::vector<bool> excluded(entities_.size(), false);
        for (std::uint32_t index = 0; index < excluded.size(); ++index) {
            excluded[index] = is_snapshot_excluded_index(index);
        }
        return excluded;
    }

    std::vector<std::uint64_t> make_snapshot_entities(const std::vector<bool>& excluded) const {
        std::vector<std::uint64_t> entities = entities_;
        for (std::size_t index = 0; index < entities.size() && index < excluded.size(); ++index) {
            if (excluded[index]) {
                entities[index] = 0;
            }
        }
        return entities;
    }

    std::vector<Entity> current_snapshot_excluded_entities() const {
        std::vector<Entity> entities;
        const TypeErasedStorage* system_storage = system_tag_ ? find_storage(system_tag_) : nullptr;
        if (system_storage != nullptr) {
            for (std::size_t dense = 0; dense < system_storage->dense_size(); ++dense) {
                const std::uint32_t index = system_storage->dense_index_at(dense);
                if (is_snapshot_excluded_index(index) && index < entities_.size()) {
                    entities.push_back(Entity{entities_[index]});
                }
            }
        }

        for (const JobRecord& job : jobs_) {
            if (alive(job.entity) && std::find(entities.begin(), entities.end(), job.entity) == entities.end()) {
                entities.push_back(job.entity);
            }
        }
        return entities;
    }

    static bool slot_is_alive_at(std::uint32_t index, std::uint64_t slot) {
        return slot_index(slot) == index && slot_version(slot) != 0;
    }

    static std::uint32_t rebuild_free_list(std::vector<std::uint64_t>& entities) {
        std::uint32_t free_head = invalid_index;
        for (std::size_t offset = entities.size(); offset > 0; --offset) {
            const std::uint32_t index = static_cast<std::uint32_t>(offset - 1);
            if (slot_is_alive_at(index, entities[index])) {
                continue;
            }

            std::uint32_t version = slot_version(entities[index]);
            if (version == 0) {
                version = 1;
            }
            entities[index] = pack(free_head, version);
            free_head = index;
        }
        return free_head;
    }

    void merge_current_system_entities(std::vector<std::uint64_t>& entities) const {
        for (Entity entity : current_snapshot_excluded_entities()) {
            const std::uint32_t index = entity_index(entity);
            if (index >= entities.size()) {
                entities.resize(static_cast<std::size_t>(index) + 1, 0);
            }
            entities[index] = entity.value;
        }
    }

    void restore_internal_bookkeeping_tags() {
        add_system_tag(system_tag_);
        for (Entity primitive : primitive_types_) {
            add_system_tag(primitive);
        }
        for (const JobRecord& job : jobs_) {
            add_system_tag(job.entity);
        }
    }

    void print_field(std::ostringstream& out, const unsigned char* data, const ComponentField& field) const {
        const ComponentRecord* type = find_component_record(field.type);
        if (type == nullptr || field.count != 1) {
            out << "<unprintable>";
            return;
        }

        switch (type->primitive) {
        case PrimitiveKind::Bool:
            out << (*reinterpret_cast<const bool*>(data) ? "true" : "false");
            break;
        case PrimitiveKind::I32:
            out << *reinterpret_cast<const std::int32_t*>(data);
            break;
        case PrimitiveKind::U32:
            out << *reinterpret_cast<const std::uint32_t*>(data);
            break;
        case PrimitiveKind::I64:
            out << *reinterpret_cast<const std::int64_t*>(data);
            break;
        case PrimitiveKind::U64:
            out << *reinterpret_cast<const std::uint64_t*>(data);
            break;
        case PrimitiveKind::F32:
            out << *reinterpret_cast<const float*>(data);
            break;
        case PrimitiveKind::F64:
            out << *reinterpret_cast<const double*>(data);
            break;
        case PrimitiveKind::None:
            out << "<unprintable>";
            break;
        }
    }

    std::vector<std::uint64_t> entities_;
    std::uint32_t free_head_ = invalid_index;
    std::unordered_map<std::uint32_t, ComponentRecord> components_;
    std::unordered_map<std::string, std::uint32_t> component_names_;
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages_;
    std::vector<Entity> typed_components_;
    std::vector<std::unique_ptr<GroupRecord>> groups_;
    std::vector<JobRecord> jobs_;
    std::uint64_t next_job_sequence_ = 0;
    std::uint64_t state_token_ = next_state_token();
    Entity singleton_entity_;
    Entity primitive_types_[7]{};
    Entity system_tag_;
};

class Registry::Snapshot {
public:
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

private:
    friend class Registry;

    std::vector<std::uint64_t> entities_;
    std::uint32_t free_head_ = invalid_index;
    std::unordered_map<std::uint32_t, ComponentRecord> components_;
    std::unordered_map<std::string, std::uint32_t> component_names_;
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages_;
    std::vector<Entity> typed_components_;
    std::vector<std::unique_ptr<GroupRecord>> groups_;
    Entity singleton_entity_;
    Entity primitive_types_[7]{};
    Entity system_tag_;
    std::uint64_t state_token_ = 0;
};

class Registry::DeltaSnapshot {
public:
    DeltaSnapshot() = default;
    DeltaSnapshot(const DeltaSnapshot&) = delete;
    DeltaSnapshot& operator=(const DeltaSnapshot&) = delete;
    DeltaSnapshot(DeltaSnapshot&&) noexcept = default;
    DeltaSnapshot& operator=(DeltaSnapshot&&) noexcept = default;
    ~DeltaSnapshot() = default;

private:
    friend class Registry;

    std::vector<std::uint64_t> entities_;
    std::uint32_t free_head_ = invalid_index;
    std::unordered_map<std::uint32_t, ComponentRecord> components_;
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages_;
    std::uint64_t baseline_token_ = 0;
    std::uint64_t state_token_ = 0;
};

inline Registry::Snapshot Registry::snapshot() const {
    Snapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    result.entities_ = make_snapshot_entities(excluded);
    result.free_head_ = rebuild_free_list(result.entities_);
    result.components_ = components_;
    result.component_names_ = component_names_;
    result.typed_components_ = typed_components_;
    result.singleton_entity_ = singleton_entity_;
    result.system_tag_ = system_tag_;
    result.state_token_ = state_token_;
    std::copy(std::begin(primitive_types_), std::end(primitive_types_), std::begin(result.primitive_types_));

    result.storages_.reserve(storages_.size());
    for (const auto& storage : storages_) {
        if (system_tag_ && storage.first == entity_index(system_tag_)) {
            continue;
        }
        result.storages_.emplace(storage.first, storage.second->clone_excluding(excluded));
    }

    result.groups_.reserve(groups_.size());
    for (const auto& group : groups_) {
        result.groups_.push_back(std::make_unique<GroupRecord>(*group));
    }

    return result;
}

inline Registry::DeltaSnapshot Registry::delta_snapshot(const Snapshot& baseline) const {
    DeltaSnapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    result.entities_ = make_snapshot_entities(excluded);
    result.free_head_ = rebuild_free_list(result.entities_);
    result.baseline_token_ = baseline.state_token_;
    result.state_token_ = next_state_token();

    for (const auto& storage : storages_) {
        if (system_tag_ && storage.first == entity_index(system_tag_)) {
            continue;
        }
        if (!storage.second->has_dirty_entries()) {
            continue;
        }
        auto found_component = components_.find(storage.first);
        if (found_component == components_.end()) {
            continue;
        }
        auto dirty = storage.second->clone_dirty_excluding(excluded);
        if (!dirty->has_dirty_entries()) {
            continue;
        }
        result.components_.emplace(found_component->first, found_component->second);
        result.storages_.emplace(storage.first, std::move(dirty));
    }

    return result;
}

inline Registry::DeltaSnapshot Registry::delta_snapshot(const DeltaSnapshot& baseline) const {
    DeltaSnapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    result.entities_ = make_snapshot_entities(excluded);
    result.free_head_ = rebuild_free_list(result.entities_);
    result.baseline_token_ = baseline.state_token_;
    result.state_token_ = next_state_token();

    for (const auto& storage : storages_) {
        if (system_tag_ && storage.first == entity_index(system_tag_)) {
            continue;
        }
        if (!storage.second->has_dirty_entries()) {
            continue;
        }
        auto found_component = components_.find(storage.first);
        if (found_component == components_.end()) {
            continue;
        }
        auto dirty = storage.second->clone_dirty_excluding(excluded);
        if (!dirty->has_dirty_entries()) {
            continue;
        }
        result.components_.emplace(found_component->first, found_component->second);
        result.storages_.emplace(storage.first, std::move(dirty));
    }

    return result;
}

inline void Registry::restore(const Snapshot& snapshot) {
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages;
    storages.reserve(snapshot.storages_.size());
    for (const auto& storage : snapshot.storages_) {
        storages.emplace(storage.first, storage.second->clone());
    }

    std::vector<std::unique_ptr<GroupRecord>> groups;
    groups.reserve(snapshot.groups_.size());
    for (const auto& group : snapshot.groups_) {
        groups.push_back(std::make_unique<GroupRecord>(*group));
    }

    std::vector<std::uint64_t> entities = snapshot.entities_;
    merge_current_system_entities(entities);
    const std::uint32_t free_head = rebuild_free_list(entities);

    entities_ = std::move(entities);
    free_head_ = free_head;
    components_ = snapshot.components_;
    component_names_ = snapshot.component_names_;
    storages_ = std::move(storages);
    typed_components_ = snapshot.typed_components_;
    groups_ = std::move(groups);
    singleton_entity_ = snapshot.singleton_entity_;
    system_tag_ = snapshot.system_tag_;
    state_token_ = snapshot.state_token_;
    std::copy(std::begin(snapshot.primitive_types_), std::end(snapshot.primitive_types_), std::begin(primitive_types_));
    restore_internal_bookkeeping_tags();
}

inline void Registry::restore(const DeltaSnapshot& snapshot) {
    if (state_token_ != snapshot.baseline_token_) {
        throw std::logic_error("ecs delta snapshot baseline does not match registry state");
    }

    for (const auto& component : snapshot.components_) {
        const auto found = components_.find(component.first);
        if (found == components_.end()) {
            throw std::logic_error("ecs delta snapshot component is not registered");
        }

        const ComponentRecord& current = found->second;
        const ComponentRecord& captured = component.second;
        if (current.entity != captured.entity ||
            current.info.size != captured.info.size ||
            current.info.alignment != captured.info.alignment ||
            current.info.trivially_copyable != captured.info.trivially_copyable ||
            current.info.tag != captured.info.tag ||
            current.singleton != captured.singleton) {
            throw std::logic_error("ecs delta snapshot component metadata does not match registry");
        }
    }

    std::vector<std::uint64_t> entities = snapshot.entities_;
    merge_current_system_entities(entities);
    entities_ = std::move(entities);
    free_head_ = rebuild_free_list(entities_);

    std::vector<std::uint32_t> destroyed_indices;
    for (const auto& storage : snapshot.storages_) {
        const TypeErasedStorage& delta_storage = *storage.second;
        for (std::size_t index = 0; index < delta_storage.tombstone_size(); ++index) {
            const std::uint32_t entity_index = static_cast<std::uint32_t>(index);
            if (delta_storage.has_destroy_tombstone(entity_index) &&
                std::find(destroyed_indices.begin(), destroyed_indices.end(), entity_index) == destroyed_indices.end()) {
                destroyed_indices.push_back(entity_index);
            }
        }
    }

    for (std::uint32_t index : destroyed_indices) {
        for (auto& storage : storages_) {
            if (storage.second->contains_index(index)) {
                remove_from_groups_before_component_removal(index, storage.first);
                storage.second->remove_index(index);
            }
        }
    }

    for (const auto& storage : snapshot.storages_) {
        const std::uint32_t component_index = storage.first;
        const Entity component{entities_.at(component_index)};
        const TypeErasedStorage& delta_storage = *storage.second;
        TypeErasedStorage* target_storage = find_storage(component);

        for (std::size_t index = 0; index < delta_storage.tombstone_size(); ++index) {
            const std::uint32_t entity_index = static_cast<std::uint32_t>(index);
            if (!delta_storage.has_dirty_tombstone_at(entity_index) ||
                delta_storage.has_destroy_tombstone(entity_index)) {
                continue;
            }
            if (target_storage == nullptr || !target_storage->contains_index(entity_index)) {
                throw std::logic_error("ecs delta snapshot component removal does not match registry state");
            }
            remove_from_groups_before_component_removal(entity_index, component_index);
            target_storage->remove_index(entity_index);
        }

        for (std::size_t dense = 0; dense < delta_storage.dense_size(); ++dense) {
            const std::uint32_t entity_index = delta_storage.dense_index_at(dense);
            if (entity_index >= entities_.size() || slot_index(entities_[entity_index]) != entity_index) {
                throw std::logic_error("ecs delta snapshot entity is not alive");
            }
            storage_for(component).emplace_or_replace_copy(entity_index, delta_storage.get(entity_index));
            refresh_groups_after_add(entity_index);
        }
    }

    state_token_ = snapshot.state_token_;
    restore_internal_bookkeeping_tags();
}

template <typename... Components>
class Registry::View {
    static_assert(sizeof...(Components) > 0, "ecs views require at least one component");
    static_assert(detail::unique_components<Components...>::value, "ecs views cannot repeat component types");

public:
    explicit View(Registry& registry)
        : registry_(&registry),
          storages_{{resolve_storage<Components>(registry)...}},
          group_(registry.best_group_for_view<Components...>()) {}

    template <typename Fn>
    void each(Fn&& fn) {
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<Components...>::value) {
                Fn& callback = fn;
                call_each(callback, Entity{}, entity_index(registry_->singleton_entity_));
            }
            return;
        }

        Fn& callback = fn;
        const std::size_t dense_size = group_ != nullptr ? group_->size : driver->dense_size();
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (!contains_all(index)) {
                continue;
            }

            call_each(callback, Entity{registry_->entities_[index]}, index);
        }
    }

    template <typename... AccessComponents>
    AccessView<detail::type_list<Components...>, AccessComponents...> access() const {
        return AccessView<detail::type_list<Components...>, AccessComponents...>(*registry_, storages_);
    }

    template <typename... Tags>
    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<Tags...>, detail::type_list<>>
    with_tags() const {
        return TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<Tags...>,
            detail::type_list<>>(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
    }

    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
    with_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
            view(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_with_tags(tags, false);
        return view;
    }

    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
    with_mutable_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
            view(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_with_tags(tags, true);
        return view;
    }

    template <typename... Tags>
    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<Tags...>>
    without_tags() const {
        return TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<Tags...>>(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
    }

    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
    without_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
            view(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_without_tags(tags, false);
        return view;
    }

    TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
    without_mutable_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<>, detail::type_list<>>
            view(*registry_, storages_, std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_without_tags(tags, true);
        return view;
    }

    template <typename T, typename std::enable_if<detail::contains_component<T, Components...>::value, int>::type = 0>
    const detail::component_query_t<T>* get(Entity entity) const {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value && detail::contains_component<T, Components...>::value,
            int>::type = 0>
    const detail::component_query_t<T>* get() const {
        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(entity_index(registry_->singleton_entity_)));
    }

    template <typename T, typename std::enable_if<
                              !std::is_const<typename std::remove_reference<T>::type>::value &&
                                  detail::contains_mutable_component<T, Components...>::value,
                              int>::type = 0>
    detail::component_query_t<T>* write(Entity entity) {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, Components...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write() {
        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(entity_index(registry_->singleton_entity_)));
    }

private:
    static constexpr std::size_t component_count = sizeof...(Components);

    template <typename T>
    static constexpr bool has_component() {
        return detail::contains_component<T, Components...>::value;
    }

    template <typename T>
    using component_ref_t = typename std::conditional<
        std::is_const<typename std::remove_reference<T>::type>::value,
        const detail::component_query_t<T>&,
        detail::component_query_t<T>&>::type;

    template <typename T, typename First, typename... Rest>
    static constexpr std::size_t component_position() {
        if constexpr (std::is_same<detail::component_query_t<T>, detail::component_query_t<First>>::value) {
            return 0;
        } else {
            return 1 + component_position<T, Rest...>();
        }
    }

    template <typename T>
    static TypeErasedStorage* resolve_storage(Registry& registry) {
        const Entity component = registry.registered_component<detail::component_query_t<T>>();
        return registry.find_storage(component);
    }

    TypeErasedStorage* driver_storage() const {
        if (group_ != nullptr) {
            return registry_->find_storage(Entity{registry_->entities_[group_->owned.front()]});
        }

        TypeErasedStorage* driver = nullptr;
        constexpr bool singleton_flags[] = {detail::is_singleton_query<Components>::value...};
        for (std::size_t position = 0; position < storages_.size(); ++position) {
            TypeErasedStorage* storage = storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }
        return driver;
    }

    bool contains_all(std::uint32_t index) const {
        constexpr bool singleton_flags[] = {detail::is_singleton_query<Components>::value...};
        for (std::size_t position = 0; position < storages_.size(); ++position) {
            const TypeErasedStorage* storage = storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    TypeErasedStorage* storage_for_type() {
        static_assert(has_component<T>(), "component is not part of this ecs view");
        constexpr std::size_t position = component_position<T, Components...>();
        return storages_[position];
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        static_assert(has_component<T>(), "component is not part of this ecs view");
        constexpr std::size_t position = component_position<T, Components...>();
        return storages_[position];
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
        if constexpr (std::is_invocable<Fn&, View&, Entity, component_ref_t<Components>...>::value) {
            callback(*this, entity, component_ref<Components>(index)...);
        } else {
            callback(entity, component_ref<Components>(index)...);
        }
    }

    template <typename T>
    component_ref_t<T> component_ref(std::uint32_t index) {
        TypeErasedStorage* storage = storage_for_type<T>();
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get(index));
        } else {
            return *static_cast<detail::component_query_t<T>*>(storage->write(index));
        }
    }

    Registry* registry_;
    std::array<TypeErasedStorage*, component_count> storages_;
    GroupRecord* group_ = nullptr;
};

template <typename... IterComponents, typename... AccessComponents>
class Registry::AccessView<detail::type_list<IterComponents...>, AccessComponents...> {
    static_assert(sizeof...(IterComponents) > 0, "ecs views require at least one component");
    static_assert(detail::unique_components<IterComponents...>::value, "ecs views cannot repeat component types");
    static_assert(detail::unique_components<AccessComponents...>::value, "ecs access components cannot repeat types");
    static_assert(
        detail::access_components_allowed<detail::type_list<IterComponents...>>::template with<AccessComponents...>::
            value,
        "ecs access components can only repeat iterated const components as mutable access components");

public:
    AccessView(Registry& registry, std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages)
        : registry_(&registry),
          iter_storages_(iter_storages),
          access_storages_{{resolve_storage<AccessComponents>(registry)...}},
          group_(registry.best_group_for_view<IterComponents...>()) {}

    template <typename Fn>
    void each(Fn&& fn) {
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<IterComponents...>::value) {
                Fn& callback = fn;
                call_each(callback, Entity{}, entity_index(registry_->singleton_entity_));
            }
            return;
        }

        Fn& callback = fn;
        const std::size_t dense_size = group_ != nullptr ? group_->size : driver->dense_size();
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (!contains_all(index)) {
                continue;
            }

            call_each(callback, Entity{registry_->entities_[index]}, index);
        }
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const {
        return TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<Tags...>,
            detail::type_list<>>(*registry_, iter_storages_, access_storages_);
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    with_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_);
        view.add_runtime_with_tags(tags, false);
        return view;
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    with_mutable_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_);
        view.add_runtime_with_tags(tags, true);
        return view;
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const {
        return TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>,
            detail::type_list<Tags...>>(*registry_, iter_storages_, access_storages_);
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    without_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_);
        view.add_runtime_without_tags(tags, false);
        return view;
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    without_mutable_tags(std::initializer_list<Entity> tags) const {
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_);
        view.add_runtime_without_tags(tags, true);
        return view;
    }

    template <
        typename T,
        typename std::enable_if<detail::contains_component<T, IterComponents..., AccessComponents...>::value, int>::
            type = 0>
    const detail::component_query_t<T>* get(Entity entity) const {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>* get() const {
        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(entity_index(registry_->singleton_entity_)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write(Entity entity) {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write() {
        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(entity_index(registry_->singleton_entity_)));
    }

private:
    template <typename T>
    using component_ref_t = typename std::conditional<
        std::is_const<typename std::remove_reference<T>::type>::value,
        const detail::component_query_t<T>&,
        detail::component_query_t<T>&>::type;

    template <typename T, typename First, typename... Rest>
    static constexpr std::size_t component_position() {
        if constexpr (std::is_same<detail::component_query_t<T>, detail::component_query_t<First>>::value) {
            return 0;
        } else {
            return 1 + component_position<T, Rest...>();
        }
    }

    template <typename T>
    static TypeErasedStorage* resolve_storage(Registry& registry) {
        const Entity component = registry.registered_component<detail::component_query_t<T>>();
        return registry.find_storage(component);
    }

    TypeErasedStorage* driver_storage() const {
        if (group_ != nullptr) {
            return registry_->find_storage(Entity{registry_->entities_[group_->owned.front()]});
        }

        TypeErasedStorage* driver = nullptr;
        constexpr bool singleton_flags[] = {detail::is_singleton_query<IterComponents>::value...};
        for (std::size_t position = 0; position < iter_storages_.size(); ++position) {
            TypeErasedStorage* storage = iter_storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }
        return driver;
    }

    bool contains_all(std::uint32_t index) const {
        constexpr bool singleton_flags[] = {detail::is_singleton_query<IterComponents>::value...};
        for (std::size_t position = 0; position < iter_storages_.size(); ++position) {
            const TypeErasedStorage* storage = iter_storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    TypeErasedStorage* storage_for_type() {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            static_assert(
                detail::contains_component<T, AccessComponents...>::value,
                "component is not part of this ecs view");
            constexpr std::size_t position = component_position<T, AccessComponents...>();
            return access_storages_[position];
        }
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            static_assert(
                detail::contains_component<T, AccessComponents...>::value,
                "component is not part of this ecs view");
            constexpr std::size_t position = component_position<T, AccessComponents...>();
            return access_storages_[position];
        }
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
        if constexpr (std::is_invocable<Fn&, AccessView&, Entity, component_ref_t<IterComponents>...>::value) {
            callback(*this, entity, component_ref<IterComponents>(index)...);
        } else {
            callback(entity, component_ref<IterComponents>(index)...);
        }
    }

    template <typename T>
    component_ref_t<T> component_ref(std::uint32_t index) {
        TypeErasedStorage* storage = storage_for_type<T>();
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get(index));
        } else {
            return *static_cast<detail::component_query_t<T>*>(storage->write(index));
        }
    }

    Registry* registry_;
    std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages_;
    std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages_;
    GroupRecord* group_ = nullptr;
};

template <
    typename... IterComponents,
    typename... AccessComponents,
    typename... WithTags,
    typename... WithoutTags>
class Registry::TagFilteredView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<WithTags...>,
    detail::type_list<WithoutTags...>> {
    static_assert(sizeof...(IterComponents) > 0, "ecs views require at least one component");
    static_assert((!detail::is_tag_query<IterComponents>::value && ...), "ecs tags must be view filters");
    static_assert((!detail::is_tag_query<AccessComponents>::value && ...), "ecs tags cannot be access components");
    static_assert((detail::is_tag_query<WithTags>::value && ...), "ecs with_tags types must be empty tags");
    static_assert((detail::is_tag_query<WithoutTags>::value && ...), "ecs without_tags types must be empty tags");

public:
    TagFilteredView(
        Registry& registry,
        std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages,
        std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages)
        : registry_(&registry),
          iter_storages_(iter_storages),
          access_storages_(access_storages),
          with_tag_storages_{{resolve_tag_storage<WithTags>(registry)...}},
          without_tag_storages_{{resolve_tag_storage<WithoutTags>(registry)...}} {}

    void add_runtime_with_tags(std::initializer_list<Entity> tags, bool mutable_access) {
        append_runtime_tags(tags, runtime_with_tags_, mutable_access);
    }

    void add_runtime_without_tags(std::initializer_list<Entity> tags, bool mutable_access) {
        append_runtime_tags(tags, runtime_without_tags_, mutable_access);
    }

    template <typename... Tags>
    auto with_tags() const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type,
            detail::type_list<WithoutTags...>> {
        using NextWith =
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type;
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            NextWith,
            detail::type_list<WithoutTags...>>
            view(*registry_, iter_storages_, access_storages_);
        copy_runtime_filters_to(view);
        return view;
    }

    auto with_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        auto view = *this;
        view.add_runtime_with_tags(tags, false);
        return view;
    }

    auto with_mutable_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        auto view = *this;
        view.add_runtime_with_tags(tags, true);
        return view;
    }

    template <typename... Tags>
    auto without_tags() const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type> {
        using NextWithout =
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type;
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            NextWithout>
            view(*registry_, iter_storages_, access_storages_);
        copy_runtime_filters_to(view);
        return view;
    }

    auto without_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        auto view = *this;
        view.add_runtime_without_tags(tags, false);
        return view;
    }

    auto without_mutable_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        auto view = *this;
        view.add_runtime_without_tags(tags, true);
        return view;
    }

    template <typename Fn>
    void each(Fn&& fn) {
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            return;
        }

        Fn& callback = fn;
        const std::size_t dense_size = driver->dense_size();
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (!contains_all(index)) {
                continue;
            }

            call_each(callback, Entity{registry_->entities_[index]}, index);
        }
    }

    template <
        typename T,
        typename std::enable_if<detail::contains_component<T, IterComponents..., AccessComponents...>::value, int>::
            type = 0>
    const detail::component_query_t<T>* get(Entity entity) const {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>* get() const {
        const TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<const detail::component_query_t<T>*>(storage->get(entity_index(registry_->singleton_entity_)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write(Entity entity) {
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        } else {
            if (!registry_->alive(entity)) {
                return nullptr;
            }
        }

        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write() {
        TypeErasedStorage* storage = storage_for_type<T>();
        if (storage == nullptr) {
            return nullptr;
        }

        return static_cast<detail::component_query_t<T>*>(storage->write(entity_index(registry_->singleton_entity_)));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool has_tag(Entity entity) const {
        return registry_->template has<detail::component_query_t<T>>(entity);
    }

    bool has_tag(Entity entity, Entity tag) const {
        return registry_->has(entity, tag);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_mutable_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool add_tag(Entity entity) {
        return registry_->template add<detail::component_query_t<T>>(entity);
    }

    bool add_tag(Entity entity, Entity tag) {
        require_mutable_runtime_tag(tag);
        return registry_->add_tag(entity, tag);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_mutable_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool remove_tag(Entity entity) {
        return registry_->template remove<detail::component_query_t<T>>(entity);
    }

    bool remove_tag(Entity entity, Entity tag) {
        require_mutable_runtime_tag(tag);
        return registry_->remove_tag(entity, tag);
    }

private:
    template <typename IterList, typename AccessList, typename WithList, typename WithoutList>
    friend class Registry::TagFilteredView;

    template <typename T>
    using component_ref_t = typename std::conditional<
        std::is_const<typename std::remove_reference<T>::type>::value,
        const detail::component_query_t<T>&,
        detail::component_query_t<T>&>::type;

    template <typename T, typename First, typename... Rest>
    static constexpr std::size_t component_position() {
        if constexpr (std::is_same<detail::component_query_t<T>, detail::component_query_t<First>>::value) {
            return 0;
        } else {
            return 1 + component_position<T, Rest...>();
        }
    }

    template <typename T>
    static TypeErasedStorage* resolve_tag_storage(Registry& registry) {
        const Entity tag = registry.registered_component<detail::component_query_t<T>>();
        registry.require_tag_component(tag);
        return registry.find_storage(tag);
    }

    void append_runtime_tags(
        std::initializer_list<Entity> tags,
        std::vector<Entity>& target,
        bool mutable_access) {
        for (Entity tag : tags) {
            registry_->require_tag_component(tag);
            target.push_back(tag);
            if (mutable_access && std::find(mutable_runtime_tags_.begin(), mutable_runtime_tags_.end(), tag) ==
                                      mutable_runtime_tags_.end()) {
                mutable_runtime_tags_.push_back(tag);
            }
        }
    }

    template <typename OtherView>
    void copy_runtime_filters_to(OtherView& other) const {
        other.runtime_with_tags_ = runtime_with_tags_;
        other.runtime_without_tags_ = runtime_without_tags_;
        other.mutable_runtime_tags_ = mutable_runtime_tags_;
    }

    TypeErasedStorage* driver_storage() const {
        TypeErasedStorage* driver = nullptr;
        constexpr bool singleton_flags[] = {detail::is_singleton_query<IterComponents>::value...};
        for (std::size_t position = 0; position < iter_storages_.size(); ++position) {
            TypeErasedStorage* storage = iter_storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }

        for (TypeErasedStorage* storage : with_tag_storages_) {
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }

        for (Entity tag : runtime_with_tags_) {
            TypeErasedStorage* storage = registry_->find_storage(tag);
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }

        return driver;
    }

    bool contains_all(std::uint32_t index) const {
        constexpr bool singleton_flags[] = {detail::is_singleton_query<IterComponents>::value...};
        for (std::size_t position = 0; position < iter_storages_.size(); ++position) {
            const TypeErasedStorage* storage = iter_storages_[position];
            if (singleton_flags[position]) {
                continue;
            }
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        for (const TypeErasedStorage* storage : with_tag_storages_) {
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        for (const TypeErasedStorage* storage : without_tag_storages_) {
            if (storage != nullptr && storage->contains_index(index)) {
                return false;
            }
        }
        for (Entity tag : runtime_with_tags_) {
            const TypeErasedStorage* storage = registry_->find_storage(tag);
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        for (Entity tag : runtime_without_tags_) {
            const TypeErasedStorage* storage = registry_->find_storage(tag);
            if (storage != nullptr && storage->contains_index(index)) {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    TypeErasedStorage* storage_for_type() {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            static_assert(
                detail::contains_component<T, AccessComponents...>::value,
                "component is not part of this ecs view");
            constexpr std::size_t position = component_position<T, AccessComponents...>();
            return access_storages_[position];
        }
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            static_assert(
                detail::contains_component<T, AccessComponents...>::value,
                "component is not part of this ecs view");
            constexpr std::size_t position = component_position<T, AccessComponents...>();
            return access_storages_[position];
        }
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
        if constexpr (std::is_invocable<Fn&, TagFilteredView&, Entity, component_ref_t<IterComponents>...>::value) {
            callback(*this, entity, component_ref<IterComponents>(index)...);
        } else {
            callback(entity, component_ref<IterComponents>(index)...);
        }
    }

    template <typename T>
    component_ref_t<T> component_ref(std::uint32_t index) {
        TypeErasedStorage* storage = storage_for_type<T>();
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->singleton_entity_);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get(index));
        } else {
            return *static_cast<detail::component_query_t<T>*>(storage->write(index));
        }
    }

    void require_mutable_runtime_tag(Entity tag) const {
        registry_->require_tag_component(tag);
        if (std::find(mutable_runtime_tags_.begin(), mutable_runtime_tags_.end(), tag) ==
            mutable_runtime_tags_.end()) {
            throw std::logic_error("ecs runtime tag was not added as mutable view access");
        }
    }

    Registry* registry_;
    std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages_;
    std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages_;
    std::array<TypeErasedStorage*, sizeof...(WithTags)> with_tag_storages_;
    std::array<TypeErasedStorage*, sizeof...(WithoutTags)> without_tag_storages_;
    std::vector<Entity> runtime_with_tags_;
    std::vector<Entity> runtime_without_tags_;
    std::vector<Entity> mutable_runtime_tags_;
};

template <typename... Components>
class Registry::JobView {
    static_assert(sizeof...(Components) > 0, "ecs jobs require at least one component");

public:
    JobView(Registry& registry, int order)
        : registry_(&registry), order_(order), view_(registry) {}

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        Callback callback(std::forward<Fn>(fn));
        return registry_->add_job(
            order_,
            registry_->template make_job_access_metadata<Components...>(),
            [callback = std::move(callback)](Registry& registry) mutable {
                registry.template view<Components...>().each(callback);
            });
    }

    template <typename... AccessComponents>
    JobAccessView<detail::type_list<Components...>, AccessComponents...> access() const {
        return JobAccessView<detail::type_list<Components...>, AccessComponents...>(
            *registry_,
            order_,
            view_.template access<AccessComponents...>());
    }

    template <typename T, typename std::enable_if<detail::contains_component<T, Components...>::value, int>::type = 0>
    const detail::component_query_t<T>* get(Entity entity) const {
        return view_.template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value && detail::contains_component<T, Components...>::value,
            int>::type = 0>
    const detail::component_query_t<T>* get() const {
        return view_.template get<T>();
    }

    template <typename T, typename std::enable_if<
                              !std::is_const<typename std::remove_reference<T>::type>::value &&
                                  detail::contains_mutable_component<T, Components...>::value,
                              int>::type = 0>
    detail::component_query_t<T>* write(Entity entity) {
        return view_.template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, Components...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write() {
        return view_.template write<T>();
    }

private:
    Registry* registry_;
    int order_ = 0;
    View<Components...> view_;
};

template <typename... IterComponents, typename... AccessComponents>
class Registry::JobAccessView<detail::type_list<IterComponents...>, AccessComponents...> {
public:
    JobAccessView(
        Registry& registry,
        int order,
        AccessView<detail::type_list<IterComponents...>, AccessComponents...> view)
        : registry_(&registry), order_(order), view_(std::move(view)) {}

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        Callback callback(std::forward<Fn>(fn));
        return registry_->add_job(
            order_,
            registry_->template make_job_access_metadata<IterComponents..., AccessComponents...>(),
            [callback = std::move(callback)](Registry& registry) mutable {
                registry.template view<IterComponents...>().template access<AccessComponents...>().each(callback);
            });
    }

    template <
        typename T,
        typename std::enable_if<detail::contains_component<T, IterComponents..., AccessComponents...>::value, int>::
            type = 0>
    const detail::component_query_t<T>* get(Entity entity) const {
        return view_.template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>* get() const {
        return view_.template get<T>();
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write(Entity entity) {
        return view_.template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* write() {
        return view_.template write<T>();
    }

private:
    Registry* registry_;
    int order_ = 0;
    AccessView<detail::type_list<IterComponents...>, AccessComponents...> view_;
};

template <typename... Components>
Registry::View<Components...> Registry::view() {
    return View<Components...>(*this);
}

template <typename... Components>
Registry::JobView<Components...> Registry::job(int order) {
    return JobView<Components...>(*this, order);
}

class Orchestrator {
public:
    explicit Orchestrator(const Registry& registry)
        : registry_(&registry) {}

    JobSchedule schedule() const {
        JobSchedule result;

        std::vector<std::size_t> ordered(registry_->jobs_.size());
        std::iota(ordered.begin(), ordered.end(), std::size_t{0});
        std::sort(ordered.begin(), ordered.end(), [&](std::size_t lhs, std::size_t rhs) {
            const Registry::JobRecord& left = registry_->jobs_[lhs];
            const Registry::JobRecord& right = registry_->jobs_[rhs];
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.sequence < right.sequence;
        });

        std::vector<std::size_t> job_stages(registry_->jobs_.size(), 0);
        for (std::size_t ordered_position = 0; ordered_position < ordered.size(); ++ordered_position) {
            const std::size_t job_index = ordered[ordered_position];
            const Registry::JobRecord& job = registry_->jobs_[job_index];

            std::size_t stage_index = 0;
            for (std::size_t previous_position = 0; previous_position < ordered_position; ++previous_position) {
                const std::size_t previous_index = ordered[previous_position];
                const Registry::JobRecord& previous = registry_->jobs_[previous_index];
                if (conflicts(previous, job)) {
                    stage_index = std::max(stage_index, job_stages[previous_index] + 1);
                }
            }

            if (result.stages.size() <= stage_index) {
                result.stages.resize(stage_index + 1);
            }
            result.stages[stage_index].jobs.push_back(job.entity);
            job_stages[job_index] = stage_index;
        }

        return result;
    }

private:
    static bool contains_component(const std::vector<std::uint32_t>& components, std::uint32_t component) {
        return std::find(components.begin(), components.end(), component) != components.end();
    }

    static bool any_overlap(const std::vector<std::uint32_t>& left, const std::vector<std::uint32_t>& right) {
        for (std::uint32_t component : left) {
            if (contains_component(right, component)) {
                return true;
            }
        }
        return false;
    }

    static bool conflicts(const Registry::JobRecord& left, const Registry::JobRecord& right) {
        return any_overlap(left.writes, right.reads) || any_overlap(left.reads, right.writes) ||
            any_overlap(left.writes, right.writes);
    }

    const Registry* registry_;
};

template <typename... Owned>
void Registry::declare_owned_group() {
    static_assert(sizeof...(Owned) > 0, "ecs owned groups require at least one component");
    static_assert(detail::unique_components<Owned...>::value, "ecs owned groups cannot repeat component types");
    static_assert((!detail::is_singleton_query<Owned>::value && ...), "ecs owned groups cannot own singleton components");
    static_assert(
        (!std::is_const<typename std::remove_reference<Owned>::type>::value && ...),
        "ecs owned groups cannot own const component types");

    (void)std::initializer_list<int>{
        (static_cast<void>(storage_for(registered_component<detail::component_query_t<Owned>>())), 0)...};
    std::vector<std::uint32_t> key = make_group_key<Owned...>(*this);
    (void)group_for_key(key);
}

}  // namespace ecs
