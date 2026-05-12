#pragma once

#include "ashiato/bit_buffer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ashiato::sync {
class ReplicationServer;
}

#ifndef ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
#ifdef NDEBUG
#define ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING 0
#else
#define ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING 1
#endif
#endif

namespace ashiato {

template <typename T>
struct is_singleton_component : std::false_type {};

class ComponentSerializationRegistry;
class PersistentSnapshotAccess;

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

template <typename... Components>
struct contains_tag_query;

template <>
struct contains_tag_query<> : std::false_type {};

template <typename First, typename... Rest>
struct contains_tag_query<First, Rest...>
    : std::conditional<
          is_tag_query<First>::value,
          std::true_type,
          contains_tag_query<Rest...>>::type {};

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

template <typename Requested, typename Available>
struct component_access_allowed_pair
    : std::integral_constant<
          bool,
          std::is_same<component_query_t<Requested>, component_query_t<Available>>::value &&
              (std::is_const<typename std::remove_reference<Requested>::type>::value ||
               !std::is_const<typename std::remove_reference<Available>::type>::value)> {};

template <typename Requested, typename... AvailableComponents>
struct contains_accessible_component
    : std::integral_constant<bool, (component_access_allowed_pair<Requested, AvailableComponents>::value || ...)> {};

template <typename AvailableList>
struct requested_components_allowed;

template <typename... AvailableComponents>
struct requested_components_allowed<type_list<AvailableComponents...>> {
    template <typename... RequestedComponents>
    struct with
        : std::integral_constant<
              bool,
              (contains_accessible_component<RequestedComponents, AvailableComponents...>::value && ...)> {};
};

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
inline thread_local std::size_t registry_access_forbidden_depth = 0;
inline thread_local std::size_t registry_access_allowed_depth = 0;

class RegistryAccessForbiddenScope {
public:
    RegistryAccessForbiddenScope() {
        ++registry_access_forbidden_depth;
    }

    RegistryAccessForbiddenScope(const RegistryAccessForbiddenScope&) = delete;
    RegistryAccessForbiddenScope& operator=(const RegistryAccessForbiddenScope&) = delete;

    ~RegistryAccessForbiddenScope() {
        --registry_access_forbidden_depth;
    }
};

class RegistryAccessAllowedScope {
public:
    RegistryAccessAllowedScope() {
        ++registry_access_allowed_depth;
    }

    RegistryAccessAllowedScope(const RegistryAccessAllowedScope&) = delete;
    RegistryAccessAllowedScope& operator=(const RegistryAccessAllowedScope&) = delete;

    ~RegistryAccessAllowedScope() {
        --registry_access_allowed_depth;
    }
};

inline bool registry_access_forbidden() {
    return registry_access_forbidden_depth != 0 && registry_access_allowed_depth == 0;
}
#endif

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

enum class EntityKind {
    Invalid,
    User,
    Component,
    Job,
    System
};

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

struct RunJobsOptions {
    bool force_single_threaded = false;
    std::vector<Entity> excluded_job_tags;
};

struct EntityComponentInfo {
    Entity component;
    std::string name;
    ComponentInfo info;
    bool singleton = false;
    bool dirty = false;
    std::string debug_value;
};

struct JobInfo {
    Entity entity;
    std::string name;
    int order = 0;
    std::vector<Entity> reads;
    std::vector<Entity> writes;
    bool structural = false;
    bool single_thread = true;
    std::size_t max_threads = 1;
    std::size_t min_entities_per_thread = 1;
};

struct SnapshotComponentOptions {
    std::vector<Entity> include_components;
    std::vector<Entity> exclude_components;
};

class Registry;

struct ComponentSerializationContext {
    void* userContext = nullptr;
};

struct ComponentSerializationOps {
    using QuantizeFn = void (*)(const void*, std::uint8_t*);
    using DequantizeFn = void (*)(const std::uint8_t*, void*);
    using PushToRegistryFn = bool (*)(Registry&, Entity, const std::uint8_t*);
    using SerializeFn = void (*)(
        const std::uint8_t*,
        const std::uint8_t*,
        BitBuffer&,
        ComponentSerializationContext*);
    using DeserializeFn = bool (*)(
        BitBuffer&,
        const std::uint8_t*,
        std::uint8_t*,
        ComponentSerializationContext*);

    std::string name;
    Entity component;
    std::size_t component_size = 0;
    std::size_t quantized_size = 0;
    QuantizeFn quantize = nullptr;
    DequantizeFn dequantize = nullptr;
    PushToRegistryFn push_to_registry = nullptr;
    SerializeFn serialize = nullptr;
    DeserializeFn deserialize = nullptr;
    bool uses_context = false;
};

template <typename T>
struct ComponentSerializationTraits {
    using Quantized = T;

    static Quantized quantize(const T& value) {
        static_assert(
            std::is_trivially_copyable<T>::value,
            "default ComponentSerializationTraits require a trivially copyable component");
        return value;
    }

    static T dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* /*baseline*/, const Quantized& current, BitBuffer& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default ComponentSerializationTraits serialization requires a trivially copyable quantized state");
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(BitBuffer& in, const Quantized* /*baseline*/, Quantized& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default ComponentSerializationTraits deserialization requires a trivially copyable quantized state");
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }
};

struct JobThreadTask {
    Entity job;
    std::size_t thread_index = 0;
    std::size_t thread_count = 1;
    std::function<void()> run;
};

using JobThreadExecutor = std::function<void(const std::vector<JobThreadTask>&)>;

class JobGraph {
public:
    JobGraph() = default;
    explicit JobGraph(JobSchedule schedule)
        : schedule_(std::move(schedule)) {}

    const JobSchedule& schedule() const noexcept {
        return schedule_;
    }

    void tick(Registry& registry, RunJobsOptions options = {}) const;
    void tick_for_entities(Registry& registry, const std::vector<Entity>& entities, RunJobsOptions options = {}) const;

private:
    JobSchedule schedule_;
};

namespace detail {

template <typename Callback, typename View, typename... Args>
void invoke_job_view_callback(Callback& callback, View& view, Entity entity, Args&&... args) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    RegistryAccessForbiddenScope scope;
#endif
    if constexpr (std::is_invocable<Callback&, View&, Entity, Args...>::value) {
        callback(view, entity, std::forward<Args>(args)...);
    } else {
        callback(entity, std::forward<Args>(args)...);
    }
}

template <typename Callback, typename Context, typename... Args>
void invoke_job_context_callback(Callback& callback, Context& context, Entity entity, Args&&... args) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    RegistryAccessForbiddenScope scope;
#endif
    if constexpr (std::is_invocable<Callback&, Context&, Entity, Args...>::value) {
        callback(context, entity, std::forward<Args>(args)...);
    } else {
        callback(entity, std::forward<Args>(args)...);
    }
}

}  // namespace detail

enum class PrimitiveType {
    Bool,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    String
};

class Registry {
    friend class Orchestrator;
    friend class JobGraph;
    friend class ashiato::sync::ReplicationServer;

public:
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    struct ComponentRemoval {
        std::uint32_t entity_index = invalid_index;
        bool entity_destroyed = false;
    };

    template <typename... Components>
    class View;

    template <typename IterList, typename AccessList, typename OptionalList>
    class AccessView;

    template <typename IterList, typename AccessList, typename OptionalList, typename WithList, typename WithoutList>
    class TagFilteredView;

    template <typename... Components>
    class JobView;

    template <typename IterList, typename AccessList, typename OptionalList>
    class JobAccessView;

    template <typename IterList, typename AccessList, typename OptionalList, typename WithList, typename WithoutList>
    class JobTagFilteredView;

    template <typename IterList, typename StructuralList>
    class JobStructuralView;

    template <typename IterList, typename AccessList, typename OptionalList, typename StructuralList>
    class JobStructuralAccessView;

    template <
        typename IterList,
        typename AccessList,
        typename OptionalList,
        typename WithList,
        typename WithoutList,
        typename StructuralList>
    class JobTagFilteredStructuralView;

    template <typename ViewType, typename... StructuralComponents>
    class JobStructuralContext;

    class Snapshot;
    class DeltaSnapshot;

    Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;
    ~Registry() = default;

    Entity create() {
        require_runtime_registry_access_allowed("create");
        if (entity_store_.free_head != invalid_index) {
            const std::uint32_t index = entity_store_.free_head;
            const std::uint64_t slot = entity_store_.slots[index];
            const std::uint32_t version = slot_version(slot);

            entity_store_.free_head = slot_index(slot);
            entity_store_.slots[index] = pack(index, version);
            return Entity{entity_store_.slots[index]};
        }

        if (entity_store_.slots.size() >= invalid_index) {
            throw std::length_error("ashiato entity index space exhausted");
        }

        const std::uint32_t index = static_cast<std::uint32_t>(entity_store_.slots.size());
        entity_store_.slots.push_back(pack(index, 1));
        return Entity{entity_store_.slots.back()};
    }

    bool destroy(Entity entity) {
        require_runtime_registry_access_allowed("destroy");
        if (!alive(entity)) {
            return false;
        }

        const std::uint32_t index = entity_index(entity);
        for (auto& storage : storage_registry_.storages) {
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

        entity_store_.slots[index] = pack(entity_store_.free_head, next_version);
        entity_store_.free_head = index;
        return true;
    }

    bool alive(Entity entity) const {
        const std::uint32_t index = entity_index(entity);
        const std::uint32_t version = entity_version(entity);

        if (version == 0 || index >= entity_store_.slots.size()) {
            return false;
        }

        const std::uint64_t slot = entity_store_.slots[index];
        return slot_index(slot) == index && slot_version(slot) == version;
    }

    EntityKind entity_kind(Entity entity) const {
        require_runtime_registry_access_allowed("entity_kind");
        if (!alive(entity)) {
            return EntityKind::Invalid;
        }

        const std::uint32_t index = entity_index(entity);
        if (component_catalog_.records.find(index) != component_catalog_.records.end()) {
            return EntityKind::Component;
        }
        if (job_registry_.index_by_entity.find(entity.value) != job_registry_.index_by_entity.end()) {
            return EntityKind::Job;
        }
        const TypeErasedStorage* system_storage =
            component_catalog_.system_tag ? find_storage(component_catalog_.system_tag) : nullptr;
        if (system_storage != nullptr && system_storage->contains_index(index)) {
            return EntityKind::System;
        }
        return EntityKind::User;
    }

    bool is_user_entity(Entity entity) const {
        require_runtime_registry_access_allowed("is_user_entity");
        return entity_kind(entity) == EntityKind::User;
    }

    template <typename T>
    Entity register_component(std::string name = {}) {
        require_runtime_registry_access_allowed("register_component");
        static_assert(
            !is_singleton_component<T>::value || std::is_default_constructible<T>::value,
            "ashiato singleton components must be default constructible");
        static_assert(
            std::is_trivially_copyable<T>::value || std::is_nothrow_move_constructible<T>::value,
            "ashiato non-trivially-copyable components must be nothrow move constructible");

        const std::size_t id = type_id<T>();
        if (id < component_catalog_.typed_components.size() && component_catalog_.typed_components[id]) {
            return component_catalog_.typed_components[id];
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
        lifecycle.nothrow_move_constructible = std::is_nothrow_move_constructible<T>::value;
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

        record_typed_component_binding(
            id,
            component_catalog_.records.at(entity_index(component)).name,
            ComponentInfo{
                detail::is_tag_query<T>::value ? 0 : sizeof(T),
                alignof(T),
                std::is_trivially_copyable<T>::value,
                detail::is_tag_query<T>::value},
            is_singleton_component<T>::value);
        ensure_typed_capacity(id);
        component_catalog_.typed_components[id] = component;
        if constexpr (is_singleton_component<T>::value) {
            const Entity singleton = singleton_entity();
            TypeErasedStorage& storage = storage_for(component);
            storage.template emplace_or_replace<T>(entity_index(singleton));
            storage.clear_added(entity_index(singleton));
        }
        return component;
    }

    Entity register_component(ComponentDesc desc);
    Entity register_tag(std::string name = {});

    template <typename T>
    Entity component() const {
        require_runtime_registry_access_allowed("component");
        return registered_component<T>();
    }

    Entity primitive_type(PrimitiveType type) const;
    Entity system_tag() const;
    Entity job_tag() const;
    const ComponentInfo* component_info(Entity component) const;
    std::string component_name(Entity component) const;
    const std::vector<ComponentField>* component_fields(Entity component) const;
    bool set_component_fields(Entity component, std::vector<ComponentField> fields);
    bool add_component_field(Entity component, ComponentField field);

    template <
        typename T,
        typename std::enable_if<!is_singleton_component<T>::value && !detail::is_tag_query<T>::value, int>::type = 0,
        typename... Args>
    T* add(Entity entity, Args&&... args) {
        require_runtime_registry_access_allowed("add");
        const Entity component = registered_component<T>();
        if (!alive(entity)) {
            return nullptr;
        }

        T* value = storage_for(component).emplace_or_replace<T>(
            entity_index(entity),
            std::forward<Args>(args)...);
        refresh_group_after_add(entity_index(entity), entity_index(component));
        return value;
    }

    template <typename T, typename std::enable_if<detail::is_tag_query<T>::value, int>::type = 0>
    bool add(Entity entity) {
        require_runtime_registry_access_allowed("add");
        const Entity component = registered_component<T>();
        return add_tag(entity, component);
    }

    bool add_tag(Entity entity, Entity tag);
    bool remove_tag(Entity entity, Entity tag);
    void* add(Entity entity, Entity component, const void* value = nullptr);
    void* ensure(Entity entity, Entity component) {
        require_runtime_registry_access_allowed("ensure");
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ashiato tags cannot be ensured as writable components");
        }
        if (record.singleton) {
            return storage_for(component).ensure(entity_index(singleton_entity()));
        }
        if (!alive(entity)) {
            return nullptr;
        }

        void* ensured = storage_for(component).ensure(entity_index(entity));
        refresh_group_after_add(entity_index(entity), entity_index(component));
        return ensured;
    }

    template <typename T, typename std::enable_if<!is_singleton_component<T>::value, int>::type = 0>
    bool remove(Entity entity) {
        require_runtime_registry_access_allowed("remove");
        const Entity component = registered_component<T>();
        return remove(entity, component);
    }

    bool remove(Entity entity, Entity component) {
        require_runtime_registry_access_allowed("remove");
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

    template <
        typename T,
        typename std::enable_if<!detail::is_tag_query<T>::value && !is_singleton_component<T>::value, int>::type = 0>
    bool contains(Entity entity) const {
        require_runtime_registry_access_allowed("contains");
        registered_component<T>();
        if (!alive(entity)) {
            return false;
        }

        const TypeErasedStorage* storage = typed_storage<T>();
        return storage != nullptr && storage->contains_index(entity_index(entity));
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_tag_query<T>::value && !is_singleton_component<T>::value, int>::type = 0>
    const T* try_get(Entity entity) const {
        require_runtime_registry_access_allowed("try_get");
        registered_component<T>();
        if (!alive(entity)) {
            return nullptr;
        }

        const TypeErasedStorage* storage = typed_storage<T>();
        return storage != nullptr ? static_cast<const T*>(storage->get(entity_index(entity))) : nullptr;
    }

    template <typename T, typename std::enable_if<!detail::is_tag_query<T>::value, int>::type = 0>
    const T& get(Entity entity) const {
        require_runtime_registry_access_allowed("get");
        registered_component<T>();
        std::uint32_t index = entity_index(entity);
        if constexpr (is_singleton_component<T>::value) {
            index = entity_index(component_catalog_.singleton_entity);
        }
        const TypeErasedStorage* storage = typed_storage<T>();
        assert(storage != nullptr);
        return *static_cast<const T*>(storage->get_unchecked(index));
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    const T& get() const {
        require_runtime_registry_access_allowed("get");
        registered_component<T>();
        const TypeErasedStorage* storage = typed_storage<T>();
        assert(storage != nullptr);
        return *static_cast<const T*>(storage->get_unchecked(entity_index(component_catalog_.singleton_entity)));
    }

    const void* get(Entity entity, Entity component) const {
        require_runtime_registry_access_allowed("get");
        const ComponentRecord& record = require_component_record(component);
        if (record.info.tag) {
            throw std::logic_error("ashiato tags cannot be read as components");
        }
        if (record.singleton) {
            entity = component_catalog_.singleton_entity;
        }
        if (!alive(entity)) {
            return nullptr;
        }

        const auto* found = find_storage(component);
        return found != nullptr ? found->get(entity_index(entity)) : nullptr;
    }

    template <typename T, typename std::enable_if<!detail::is_tag_query<T>::value, int>::type = 0>
    T& write(Entity entity) {
        require_runtime_registry_access_allowed("write");
        registered_component<T>();
        std::uint32_t index = entity_index(entity);
        if constexpr (is_singleton_component<T>::value) {
            index = entity_index(component_catalog_.singleton_entity);
        }
        TypeErasedStorage* storage = typed_storage<T>();
        assert(storage != nullptr);
        return *static_cast<T*>(storage->write_unchecked(index));
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    T& write() {
        require_runtime_registry_access_allowed("write");
        registered_component<T>();
        TypeErasedStorage* storage = typed_storage<T>();
        assert(storage != nullptr);
        return *static_cast<T*>(storage->write_unchecked(entity_index(component_catalog_.singleton_entity)));
    }

    void* write(Entity entity, Entity component);

    template <typename T>
    bool clear_dirty(Entity entity) {
        require_runtime_registry_access_allowed("clear_dirty");
        return dirty_clear<T>(entity);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool clear_dirty() {
        require_runtime_registry_access_allowed("clear_dirty");
        return dirty_clear<T>();
    }

    bool clear_dirty(Entity entity, Entity component) {
        require_runtime_registry_access_allowed("clear_dirty");
        return dirty_clear(entity, component);
    }

    template <typename T>
    bool is_dirty(Entity entity) const {
        require_runtime_registry_access_allowed("is_dirty");
        return dirty_is<T>(entity);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool is_dirty() const {
        require_runtime_registry_access_allowed("is_dirty");
        return dirty_is<T>();
    }

    bool is_dirty(Entity entity, Entity component) const {
        require_runtime_registry_access_allowed("is_dirty");
        return dirty_is(entity, component);
    }

    template <typename T>
    void clear_all_dirty() {
        require_runtime_registry_access_allowed("clear_all_dirty");
        dirty_clear_all(registered_component<T>());
    }

    void clear_all_dirty(Entity component) {
        require_runtime_registry_access_allowed("clear_all_dirty");
        dirty_clear_all(component);
    }

    template <typename T, typename Fn>
    void each_dirty(Fn&& fn) const {
        require_runtime_registry_access_allowed("each_dirty");
        dirty_each<T>(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void each_dirty(Entity component, Fn&& fn) const {
        require_runtime_registry_access_allowed("each_dirty");
        dirty_each(component, std::forward<Fn>(fn));
    }

    template <typename T, typename Fn, typename std::enable_if<!is_singleton_component<T>::value, int>::type = 0>
    void each_added(Fn&& fn) const {
        require_runtime_registry_access_allowed("each_added");
        dirty_each_added<T>(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void each_added(Entity component, Fn&& fn) const {
        require_runtime_registry_access_allowed("each_added");
        dirty_each_added(component, std::forward<Fn>(fn));
    }

    template <typename T, typename Fn>
    void each_removed(Fn&& fn) const {
        require_runtime_registry_access_allowed("each_removed");
        dirty_each_removed<T>(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void each_removed(Entity component, Fn&& fn) const {
        require_runtime_registry_access_allowed("each_removed");
        dirty_each_removed(component, std::forward<Fn>(fn));
    }

    template <typename T, typename std::enable_if<detail::is_tag_query<T>::value, int>::type = 0>
    bool has(Entity entity) const {
        require_runtime_registry_access_allowed("has");
        const Entity component = registered_component<T>();
        return has(entity, component);
    }

    bool has(Entity entity, Entity component) const;

    class DirtyView {
    public:
        DirtyView() = default;

        template <typename T>
        bool is_dirty(Entity entity) const {
            return registry_ != nullptr && registry_->dirty_is<T>(entity);
        }

        template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
        bool is_dirty() const {
            return registry_ != nullptr && registry_->dirty_is<T>();
        }

        bool is_dirty(Entity entity, Entity component) const {
            return registry_ != nullptr && registry_->dirty_is(entity, component);
        }

        template <typename T, typename Fn>
        void each_dirty(Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each<T>(std::forward<Fn>(fn));
            }
        }

        template <typename Fn>
        void each_dirty(Entity component, Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each(component, std::forward<Fn>(fn));
            }
        }

        template <typename T, typename Fn, typename std::enable_if<!is_singleton_component<T>::value, int>::type = 0>
        void each_added(Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each_added<T>(std::forward<Fn>(fn));
            }
        }

        template <typename Fn>
        void each_added(Entity component, Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each_added(component, std::forward<Fn>(fn));
            }
        }

        template <typename T, typename Fn>
        void each_removed(Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each_removed<T>(std::forward<Fn>(fn));
            }
        }

        template <typename Fn>
        void each_removed(Entity component, Fn&& fn) const {
            if (registry_ != nullptr) {
                registry_->dirty_each_removed(component, std::forward<Fn>(fn));
            }
        }

    private:
        friend class Registry;
        explicit DirtyView(const Registry& registry) : registry_(&registry) {}

        const Registry* registry_ = nullptr;
    };

    class DirtyFrameScope {
    public:
        DirtyFrameScope(const DirtyFrameScope&) = delete;
        DirtyFrameScope& operator=(const DirtyFrameScope&) = delete;
        DirtyFrameScope(DirtyFrameScope&& other) noexcept : registry_(other.registry_) {
            other.registry_ = nullptr;
        }
        DirtyFrameScope& operator=(DirtyFrameScope&& other) noexcept {
            if (this != &other) {
                clear();
                registry_ = other.registry_;
                other.registry_ = nullptr;
            }
            return *this;
        }
        ~DirtyFrameScope() {
            clear();
        }

        DirtyView view() const {
            return registry_ != nullptr ? DirtyView(*registry_) : DirtyView{};
        }

    private:
        friend class Registry;
        explicit DirtyFrameScope(Registry& registry) : registry_(&registry) {}

        void clear() {
            if (registry_ != nullptr) {
                registry_->clear_all_dirty_entries();
                registry_ = nullptr;
            }
        }

        Registry* registry_ = nullptr;
    };

    template <typename... Components>
    typename std::enable_if<!detail::contains_tag_query<Components...>::value, View<Components...>>::type view();

    template <typename... Components>
    JobView<Components...> job(int order);

    void set_job_thread_executor(JobThreadExecutor executor);

    void run_jobs(RunJobsOptions options = {});
    void run_jobs_for_entities(const std::vector<Entity>& entities, RunJobsOptions options = {});
    JobGraph compile_job_graph(const std::vector<Entity>& jobs) const;
    JobGraph compile_all_jobs_graph() const;

    template <typename... Owned>
    void declare_owned_group();

    Snapshot create_snapshot() const;
    DeltaSnapshot create_delta_snapshot(const Snapshot& baseline) const;
    DeltaSnapshot create_delta_snapshot(const DeltaSnapshot& baseline) const;
    void restore_snapshot(const Snapshot& snapshot);
    void restore_delta_snapshot(const DeltaSnapshot& snapshot);

    std::string debug_print(Entity entity, Entity component) const;
    std::vector<EntityComponentInfo> components(Entity entity) const;
    std::vector<EntityComponentInfo> singleton_components() const;
    Entity singleton_storage_entity() const;
    std::optional<JobInfo> job_info(Entity job) const;
    std::vector<Entity> job_matching_entities(Entity job) const;

    static constexpr std::uint32_t entity_index(Entity entity) noexcept {
        return static_cast<std::uint32_t>(entity.value);
    }

    static constexpr std::uint32_t entity_version(Entity entity) noexcept {
        return static_cast<std::uint32_t>(entity.value >> 32U);
    }

private:
    friend class ComponentSerializationRegistry;
    friend class RegistryDirtyFrameBroadcaster;
    friend class PersistentSnapshotAccess;
    static constexpr std::size_t npos_type_id = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t default_min_entities_per_thread = 1024;

    void require_runtime_registry_access_allowed(const char* operation) const {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        if (detail::registry_access_forbidden()) {
            std::string message = "ashiato Registry::";
            message += operation;
            message += " cannot be used inside an ashiato job callback; use the callback view/context and declare";
            message += " required components on the job with job<...>(), access_other_entities<...>(), or structural<...>()";
            throw std::logic_error(message);
        }
#else
        (void)operation;
#endif
    }

    DirtyFrameScope dirty_scope() {
        require_runtime_registry_access_allowed("begin_dirty_frame");
        return DirtyFrameScope(*this);
    }

    bool dirty_clear(Entity entity, Entity component);
    void dirty_clear_all(Entity component);
    void clear_all_dirty_entries();
    bool dirty_is(Entity entity, Entity component) const;

    template <typename T>
    bool dirty_clear(Entity entity) {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return dirty_clear(component_catalog_.singleton_entity, component);
        }
        return dirty_clear(entity, component);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool dirty_clear() {
        const Entity component = registered_component<T>();
        return dirty_clear(component_catalog_.singleton_entity, component);
    }

    template <typename T>
    bool dirty_is(Entity entity) const {
        const Entity component = registered_component<T>();
        if constexpr (is_singleton_component<T>::value) {
            return dirty_is(component_catalog_.singleton_entity, component);
        }
        return dirty_is(entity, component);
    }

    template <typename T, typename std::enable_if<is_singleton_component<T>::value, int>::type = 0>
    bool dirty_is() const {
        const Entity component = registered_component<T>();
        return dirty_is(component_catalog_.singleton_entity, component);
    }

    template <typename T, typename Fn>
    void dirty_each(Fn&& fn) const {
        const Entity component = registered_component<T>();
        dirty_each(component, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void dirty_each(Entity component, Fn&& fn) const {
        const ComponentRecord& record = require_component_record(component);
        Entity singleton = Entity{};
        if (record.singleton) {
            singleton = component_catalog_.singleton_entity;
        }

        const auto* found = find_storage(component);
        if (found == nullptr) {
            return;
        }

        Fn& callback = fn;
        found->each_dirty([&](std::uint32_t index, const void* value) {
            callback(record.singleton ? singleton : Entity{entity_store_.slots[index]}, value);
        });
    }

    template <typename T, typename Fn, typename std::enable_if<!is_singleton_component<T>::value, int>::type = 0>
    void dirty_each_added(Fn&& fn) const {
        const Entity component = registered_component<T>();
        dirty_each_added(component, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void dirty_each_added(Entity component, Fn&& fn) const {
        const ComponentRecord& record = require_component_record(component);
        if (record.singleton) {
            throw std::logic_error("ashiato singleton components do not track additions");
        }

        const auto* found = find_storage(component);
        if (found == nullptr) {
            return;
        }

        Fn& callback = fn;
        found->each_added([&](std::uint32_t index, const void* value) {
            callback(Entity{entity_store_.slots[index]}, value);
        });
    }

    template <typename T, typename Fn>
    void dirty_each_removed(Fn&& fn) const {
        const Entity component = registered_component<T>();
        dirty_each_removed(component, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void dirty_each_removed(Entity component, Fn&& fn) const {
        const ComponentRecord& record = require_component_record(component);
        if (record.singleton) {
            return;
        }

        const auto* found = find_storage(component);
        if (found == nullptr) {
            return;
        }

        Fn& callback = fn;
        found->each_removed([&](ComponentRemoval removal) {
            callback(removal);
        });
    }

    enum class PrimitiveKind {
        None,
        Bool,
        I32,
        U32,
        I64,
        U64,
        F32,
        F64,
        String
    };

    struct ComponentLifecycle {
        using CopyConstruct = void (*)(void*, const void*);
        using MoveConstruct = void (*)(void*, void*);
        using Destroy = void (*)(void*);

        bool trivially_copyable = true;
        bool nothrow_move_constructible = true;
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

        explicit TypeErasedStorage(const ComponentRecord& record);
        TypeErasedStorage(const TypeErasedStorage& other);
        TypeErasedStorage& operator=(const TypeErasedStorage& other);
        TypeErasedStorage(TypeErasedStorage&& other) noexcept;
        TypeErasedStorage& operator=(TypeErasedStorage&& other) noexcept;
        ~TypeErasedStorage();

        template <typename T, typename... Args>
        T* emplace_or_replace(std::uint32_t index, Args&&... args) {
            if (info_.tag) {
                throw std::logic_error("ashiato tags do not store component values");
            }
            validate_type<T>();

            if (void* existing = get(index)) {
                mark_dirty_dense(sparse_[index]);

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
            clear_tombstone(index);
            ensure_capacity(size_ + 1);

            const std::uint32_t dense = static_cast<std::uint32_t>(size_);
            dense_indices_.push_back(index);
            dirty_.push_back(1);
            added_.push_back(1);
            ++dirty_count_;
            ++added_count_;
            sparse_[index] = dense;
            void* target = data_ + size_ * info_.size;
            new (target) T(std::forward<Args>(args)...);
            ++size_;

            return static_cast<T*>(target);
        }

        void emplace_or_replace_tag(std::uint32_t index);
        void* emplace_or_replace_bytes(std::uint32_t index, const void* value);
        void emplace_or_replace_copy(std::uint32_t index, const void* value);
        void* ensure(std::uint32_t index);
        bool remove(std::uint32_t index);
        void remove_index(std::uint32_t index);
        void remove_index_for_destroy(std::uint32_t index);
        void* get(std::uint32_t index);
        const void* get(std::uint32_t index) const;
        void* write(std::uint32_t index);
        const void* get_unchecked(std::uint32_t index) const;
        void* write_unchecked(std::uint32_t index);
        bool clear_dirty(std::uint32_t index);
        bool clear_added(std::uint32_t index);
        bool is_dirty(std::uint32_t index) const;
        void clear_all_dirty();
        void mark_dirty(std::uint32_t index);
        std::size_t dense_size() const noexcept;
        std::uint32_t dense_index_at(std::size_t dense) const;
        std::uint32_t dense_position(std::uint32_t index) const;
        bool contains_index(std::uint32_t index) const;
        bool has_dirty_entries() const;

        template <typename Fn>
        void each_dirty(Fn&& fn) const {
            Fn& callback = fn;
            for (std::size_t dense = 0; dense < size_; ++dense) {
                if (dirty_[dense] == 0) {
                    continue;
                }
                callback(dense_indices_[dense], info_.tag ? nullptr : get_dense(dense));
            }
        }

        template <typename Fn>
        void each_added(Fn&& fn) const {
            Fn& callback = fn;
            for (std::size_t dense = 0; dense < size_; ++dense) {
                if (added_[dense] == 0) {
                    continue;
                }
                callback(dense_indices_[dense], info_.tag ? nullptr : get_dense(dense));
            }
        }

        template <typename Fn>
        void each_removed(Fn&& fn) const {
            Fn& callback = fn;
            for (std::size_t position = 0; position < tombstone_entry_count(); ++position) {
                if (!has_dirty_tombstone_at_position(position)) {
                    continue;
                }

                callback(ComponentRemoval{
                    tombstone_index_at(position),
                    has_destroy_tombstone_at_position(position),
                });
            }
        }

        bool has_destroy_tombstone(std::uint32_t index) const;
        std::size_t tombstone_size() const noexcept;
        std::size_t tombstone_entry_count() const noexcept;
        std::uint32_t tombstone_index_at(std::size_t position) const;
        unsigned char tombstone_flags_at(std::size_t position) const;
        bool has_dirty_tombstone_at(std::uint32_t index) const;
        bool has_dirty_tombstone_at_position(std::size_t position) const;
        bool has_destroy_tombstone_at_position(std::size_t position) const;
        const void* get_dense(std::size_t dense) const;
        void swap_dense(std::uint32_t lhs, std::uint32_t rhs);
        void move_index_to_dense(std::uint32_t index, std::uint32_t dense);
        void* write_unchecked_without_dirty(std::uint32_t index);

        struct DeferredDirtyWrite {
            TypeErasedStorage* storage = nullptr;
            std::uint32_t index = 0;
        };

        using DeferredDirtyWrites = std::vector<DeferredDirtyWrite>;

        class DeferredDirtyScope {
        public:
            DeferredDirtyScope(DeferredDirtyWrites& writes, bool defer_range_dirty);
            DeferredDirtyScope(const DeferredDirtyScope&) = delete;
            DeferredDirtyScope& operator=(const DeferredDirtyScope&) = delete;
            ~DeferredDirtyScope();

        private:
            DeferredDirtyWrites* previous_writes_ = nullptr;
            bool previous_defer_range_dirty_ = false;
        };

        static bool range_dirty_deferred() noexcept;
        std::unique_ptr<TypeErasedStorage> clone() const;
        std::unique_ptr<TypeErasedStorage> clone_for_restore() const;
        std::unique_ptr<TypeErasedStorage> clone_dirty() const;
        std::unique_ptr<TypeErasedStorage> clone_excluding(const std::vector<bool>& excluded) const;
        std::unique_ptr<TypeErasedStorage> clone_dirty_excluding(const std::vector<bool>& excluded) const;

        friend void write_storage(std::ostream& out, const Registry::TypeErasedStorage& storage);
        friend std::unique_ptr<Registry::TypeErasedStorage> read_storage(
            std::istream& in,
            const Registry::ComponentRecord& record);
        friend class PersistentSnapshotAccess;

    private:
        TypeErasedStorage(ComponentInfo info, ComponentLifecycle lifecycle);

        template <typename T>
        void validate_type() const {
            if (sizeof(T) != info_.size || alignof(T) != info_.alignment) {
                throw std::logic_error("registered component metadata does not match T");
            }
        }

        static unsigned char* allocate(std::size_t capacity, const ComponentInfo& info);
        static void deallocate(unsigned char* data, std::size_t alignment) noexcept;
        void assign_bytes(void* target, const void* value);
        void construct_copy(void* target, const void* value);
        void replace_copy(void* target, const void* value);
        void ensure_sparse(std::uint32_t index);
        bool contains(std::uint32_t index) const;
        void ensure_capacity(std::size_t required);
        void erase_at(std::uint32_t dense);
        void clear() noexcept;
        void mark_tombstone(std::uint32_t index, unsigned char flags);
        void clear_tombstone(std::uint32_t index);
        bool has_tombstone(std::uint32_t index) const;
        void mark_dirty_dense(std::uint32_t dense);
        void clear_dirty_dense(std::uint32_t dense);
        void clear_added_dense(std::uint32_t dense);
        void mark_dirty_or_defer(std::uint32_t index, std::uint32_t dense);
        void erase_tombstone_index(std::uint32_t index);
        std::unique_ptr<TypeErasedStorage> clone_compact_filtered(
            const std::vector<bool>& excluded,
            bool dirty_only) const;
        bool has_excluded_storage_entries(const std::vector<bool>& excluded) const;
        void copy_compact_tombstones_excluding(
            TypeErasedStorage& copy,
            const std::vector<bool>& excluded) const;
        void rebuild_lookup();
        void copy_from(const TypeErasedStorage& other);

        std::vector<std::uint32_t> dense_indices_;
        std::vector<std::uint32_t> sparse_;
        std::vector<unsigned char> dirty_;
        std::vector<unsigned char> added_;
        std::vector<unsigned char> tombstones_;
        std::vector<std::uint32_t> tombstone_indices_;
        unsigned char* data_ = nullptr;
        std::size_t size_ = 0;
        std::size_t capacity_ = 0;
        std::size_t dirty_count_ = 0;
        std::size_t added_count_ = 0;
        std::size_t tombstone_count_ = 0;
        bool compact_lookup_ = false;
        ComponentInfo info_;
        ComponentLifecycle lifecycle_;
        std::vector<unsigned char> swap_scratch_;

        static thread_local DeferredDirtyWrites* deferred_dirty_writes_;
        static thread_local bool defer_range_dirty_;

        static constexpr unsigned char no_tombstone = 0;
        static constexpr unsigned char tombstone_dirty = 1U;
        static constexpr unsigned char tombstone_destroy_entity = 2U;
    };

    struct GroupRecord {
        std::vector<std::uint32_t> owned;
        std::size_t size = 0;
    };

    friend void write_component_record(std::ostream& out, const Registry::ComponentRecord& record);
    friend Registry::ComponentRecord read_component_record(std::istream& in);
    friend void write_storage(std::ostream& out, const Registry::TypeErasedStorage& storage);
    friend std::unique_ptr<Registry::TypeErasedStorage> read_storage(
        std::istream& in,
        const Registry::ComponentRecord& record);
    friend void validate_serializable_component(const Registry::ComponentRecord& record);
    friend void write_snapshot_common(
        std::ostream& out,
        std::uint32_t kind,
        const std::vector<std::uint64_t>& entities,
        std::uint32_t free_head,
        const std::unordered_map<std::uint32_t, Registry::ComponentRecord>& components,
        const std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages,
        const std::vector<Entity>& typed_components,
        const std::vector<std::unique_ptr<Registry::GroupRecord>>* groups,
        Entity singleton_entity,
        const Entity* primitive_types,
        Entity system_tag,
        bool has_entities,
        std::uint64_t baseline_token,
        std::uint64_t state_token,
        const SnapshotComponentOptions& options);
    friend void read_snapshot_header(
        std::istream& in,
        std::uint32_t expected_kind,
        bool& has_entities,
        std::uint64_t& baseline_token,
        std::uint64_t& state_token,
        std::uint32_t& free_head,
        Entity& singleton_entity,
        Entity* primitive_types,
        Entity& system_tag,
        std::vector<std::uint64_t>& entities,
        std::unordered_map<std::uint32_t, Registry::ComponentRecord>& components,
        std::vector<Entity>& typed_components,
        std::vector<std::unique_ptr<Registry::GroupRecord>>& groups,
        std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages);
    friend void write_persistent_snapshot(
        std::ostream& out,
        const Registry::Snapshot& snapshot,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::Snapshot read_persistent_snapshot(
        std::istream& in,
        const Registry& schema,
        const ComponentSerializationRegistry& serialization);
    friend void write_persistent_delta_snapshot(
        std::ostream& out,
        const Registry::DeltaSnapshot& snapshot,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::DeltaSnapshot read_persistent_delta_snapshot(
        std::istream& in,
        const Registry& schema,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization);

    struct JobRecord {
        Entity entity;
        std::string name;
        int order = 0;
        std::uint64_t sequence = 0;
        std::vector<std::uint32_t> reads;
        std::vector<std::uint32_t> writes;
        std::function<void(Registry&)> run;
        std::function<std::vector<std::uint32_t>(Registry&)> collect_indices;
        std::function<void(Registry&, const std::vector<std::uint32_t>&, std::size_t, std::size_t)> run_range;
        std::vector<std::uint32_t> range_dirty_writes;
        std::size_t max_threads = 1;
        std::size_t min_entities_per_thread = default_min_entities_per_thread;
        bool single_thread = true;
        bool structural = false;
    };

    struct JobAccessMetadata {
        std::vector<std::uint32_t> reads;
        std::vector<std::uint32_t> writes;
    };

    static void canonicalize_components(std::vector<std::uint32_t>& components);
    static void canonicalize_job_metadata(JobAccessMetadata& metadata);
    static void append_unique_component(std::vector<std::uint32_t>& components, std::uint32_t component);

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

    template <typename T>
    void append_job_filter_component(JobAccessMetadata& metadata) const {
        const Entity component = registered_component<detail::component_query_t<T>>();
        append_unique_component(metadata.reads, entity_index(component));
    }

    template <typename... Components>
    JobAccessMetadata make_job_access_metadata() const {
        JobAccessMetadata metadata;
        (append_job_access_component<Components>(metadata), ...);
        canonicalize_job_metadata(metadata);
        return metadata;
    }

    template <typename... Components>
    std::vector<std::uint32_t> make_job_range_dirty_writes() const {
        JobAccessMetadata metadata;
        (append_job_access_component<Components>(metadata), ...);
        canonicalize_job_metadata(metadata);
        return std::move(metadata.writes);
    }

    template <typename... Components>
    void append_job_filter_metadata(JobAccessMetadata& metadata) const {
        (append_job_filter_component<Components>(metadata), ...);
        canonicalize_job_metadata(metadata);
    }

    template <typename... Components>
    void append_job_structural_metadata(JobAccessMetadata& metadata) const {
        (append_unique_component(
             metadata.writes,
             entity_index(registered_component<detail::component_query_t<Components>>())),
         ...);
        canonicalize_job_metadata(metadata);
    }

    struct JobThreadingOptions {
        std::size_t max_threads = 1;
        std::size_t min_entities_per_thread = default_min_entities_per_thread;
        bool single_thread = true;
    };

    Entity add_job(
        int order,
        std::string name,
        JobAccessMetadata metadata,
        std::function<void(Registry&)> run,
        std::function<std::vector<std::uint32_t>(Registry&)> collect_indices,
        std::function<void(Registry&, const std::vector<std::uint32_t>&, std::size_t, std::size_t)> run_range,
        std::vector<std::uint32_t> range_dirty_writes,
        JobThreadingOptions threading,
        bool structural);

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
        for (const auto& group : group_index_.groups) {
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

    static bool includes_all(const std::vector<std::uint32_t>& lhs, const std::vector<std::uint32_t>& rhs);

    GroupRecord& group_for_key(const std::vector<std::uint32_t>& key);
    void validate_group_key(const std::vector<std::uint32_t>& key) const;
    void register_group_ownership(GroupRecord& group);
    void rebuild_group_ownership();
    void build_group(GroupRecord& group);
    TypeErasedStorage* smallest_storage(const GroupRecord& group);
    bool group_contains_all(const GroupRecord& group, std::uint32_t index) const;
    bool group_contains_component(const GroupRecord& group, std::uint32_t component) const;
    bool group_contains_index(const GroupRecord& group, std::uint32_t index) const;
    void enter_group(GroupRecord& group, std::uint32_t index);
    void leave_group(GroupRecord& group, std::uint32_t index);
    void refresh_group_after_add(std::uint32_t index, std::uint32_t component);
    void remove_from_groups_before_component_removal(std::uint32_t index, std::uint32_t component);

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

    static std::size_t next_type_id();
    static std::uint64_t next_state_token();

    struct TypedComponentBinding {
        std::string name;
        ComponentInfo info;
        bool singleton = false;
        bool registered = false;
    };

    static std::vector<TypedComponentBinding>& typed_component_bindings();
    static std::mutex& typed_component_bindings_mutex();

    template <typename T>
    static std::size_t type_id() {
        static const std::size_t id = next_type_id();
        return id;
    }

    static void record_typed_component_binding(
        std::size_t id,
        std::string name,
        ComponentInfo info,
        bool singleton);

    void rebind_typed_components_by_registered_names();
    void ensure_typed_capacity(std::size_t id);

    template <typename T>
    Entity registered_component() const {
        const std::size_t id = type_id<T>();
        if (id >= component_catalog_.typed_components.size() || !component_catalog_.typed_components[id]) {
            throw std::logic_error("ashiato component type is not registered");
        }

        return component_catalog_.typed_components[id];
    }

    Entity singleton_entity();
    Entity register_component_impl(
        ComponentDesc desc,
        ComponentLifecycle lifecycle,
        bool trivially_copyable,
        bool singleton,
        bool tag,
        std::size_t typed_id);
    ComponentRecord* find_component_record(Entity component);
    const ComponentRecord* find_component_record(Entity component) const;
    ComponentRecord& require_component_record(Entity component);
    const ComponentRecord& require_component_record(Entity component) const;
    bool valid_component_field(const ComponentRecord& component, const ComponentField& field) const;
    bool valid_component_fields(const ComponentRecord& component, const std::vector<ComponentField>& fields) const;
    TypeErasedStorage& storage_for(Entity component) {
        const ComponentRecord& record = require_component_record(component);
        const std::uint32_t component_index = entity_index(component);

        auto found = storage_registry_.storages.find(component_index);
        if (found == storage_registry_.storages.end()) {
            auto inserted = storage_registry_.storages.emplace(component_index, std::make_unique<TypeErasedStorage>(record));
            found = inserted.first;
            bump_view_topology_token();
        }

        if (record.type_id != npos_type_id) {
            ensure_typed_capacity(record.type_id);
            storage_registry_.typed_storages[record.type_id] = found->second.get();
        }

        return *found->second;
    }

    void bump_view_topology_token() noexcept {
        ++storage_registry_.view_topology_token;
        if (storage_registry_.view_topology_token == 0) {
            storage_registry_.view_topology_token = 1;
        }
    }

    template <typename T>
    TypeErasedStorage* typed_storage() {
        const std::size_t id = type_id<T>();
        return id < storage_registry_.typed_storages.size() ? storage_registry_.typed_storages[id] : nullptr;
    }

    template <typename T>
    const TypeErasedStorage* typed_storage() const {
        const std::size_t id = type_id<T>();
        return id < storage_registry_.typed_storages.size() ? storage_registry_.typed_storages[id] : nullptr;
    }

    void rebuild_typed_storages();

    TypeErasedStorage* find_storage(Entity component) {
        auto found = storage_registry_.storages.find(entity_index(component));
        return found != storage_registry_.storages.end() ? found->second.get() : nullptr;
    }

    const TypeErasedStorage* find_storage(Entity component) const {
        auto found = storage_registry_.storages.find(entity_index(component));
        return found != storage_registry_.storages.end() ? found->second.get() : nullptr;
    }

    void require_tag_component(Entity component) const;
    void unregister_component_entity(Entity component);
    void register_primitive_types();
    Entity register_primitive(std::string name, std::size_t size, std::size_t alignment, PrimitiveKind kind);
    void register_system_tag();
    void register_job_tag();
    void add_system_tag(Entity entity);
    void add_job_tag(Entity entity);

    bool is_snapshot_excluded_index(std::uint32_t index) const;
    std::vector<bool> make_snapshot_exclusion_mask() const;
    std::vector<std::uint64_t> make_snapshot_entities(const std::vector<bool>& excluded) const;
    std::vector<Entity> current_snapshot_excluded_entities() const;

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

    void merge_current_system_entities(std::vector<std::uint64_t>& entities) const;
    void restore_internal_bookkeeping_tags();
    void print_field(std::ostringstream& out, const unsigned char* data, const ComponentField& field) const;
    const std::vector<std::size_t>& ordered_job_indices() const;
    std::size_t job_index(Entity job) const;

    void mark_component_dirty_range(
        const std::vector<std::uint32_t>& components,
        const std::vector<std::uint32_t>& indices,
        std::size_t begin,
        std::size_t end);
    void merge_deferred_dirty_writes(const TypeErasedStorage::DeferredDirtyWrites& writes);

    bool job_excluded_by_options(const JobRecord& job, const RunJobsOptions& options) const;

    const JobGraph& cached_all_jobs_graph() const;

    struct EntityStore {
        std::vector<std::uint64_t> slots;
        std::uint32_t free_head = invalid_index;
    };

    struct ComponentCatalog {
        std::unordered_map<std::uint32_t, ComponentRecord> records;
        std::unordered_map<std::string, std::uint32_t> names;
        std::vector<Entity> typed_components;
        Entity singleton_entity;
        Entity primitive_types[8]{};
        Entity system_tag;
        Entity job_tag;
    };

    struct StorageRegistry {
        std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages;
        std::vector<TypeErasedStorage*> typed_storages;
        std::uint64_t view_topology_token = 1;
    };

    struct GroupIndex {
        std::vector<std::unique_ptr<GroupRecord>> groups;
        std::unordered_map<std::uint32_t, GroupRecord*> owned_component_groups;
    };

    struct JobRegistry {
        std::vector<JobRecord> jobs;
        std::unordered_map<std::uint64_t, std::size_t> index_by_entity;
        JobThreadExecutor thread_executor;
        mutable JobSchedule cached_schedule;
        mutable bool schedule_cache_valid = false;
        mutable JobGraph cached_graph;
        mutable bool graph_cache_valid = false;
        mutable std::vector<std::size_t> ordered_indices_cache;
        mutable bool ordered_indices_cache_valid = false;
        std::uint64_t next_sequence = 0;
    };

    EntityStore entity_store_;
    ComponentCatalog component_catalog_;
    StorageRegistry storage_registry_;
    GroupIndex group_index_;
    JobRegistry job_registry_;
    std::uint64_t state_token_ = next_state_token();
};

class Registry::Snapshot {
public:
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

    void write(std::ostream& out, const SnapshotComponentOptions& options = {}) const;
    static Snapshot read(std::istream& in);
    void write_native(std::ostream& out, const SnapshotComponentOptions& options = {}) const;
    static Snapshot read_native(std::istream& in);

private:
    friend class Registry;
    friend class PersistentSnapshotAccess;
    friend void write_persistent_snapshot(
        std::ostream& out,
        const Registry::Snapshot& snapshot,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::Snapshot read_persistent_snapshot(
        std::istream& in,
        const Registry& schema,
        const ComponentSerializationRegistry& serialization);
    friend void write_persistent_delta_snapshot(
        std::ostream& out,
        const Registry::DeltaSnapshot& snapshot,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::DeltaSnapshot read_persistent_delta_snapshot(
        std::istream& in,
        const Registry& schema,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization);

    std::vector<std::uint64_t> entities_;
    std::uint32_t free_head_ = invalid_index;
    std::unordered_map<std::uint32_t, ComponentRecord> components_;
    std::unordered_map<std::string, std::uint32_t> component_names_;
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages_;
    std::vector<Entity> typed_components_;
    std::vector<std::unique_ptr<GroupRecord>> groups_;
    Entity singleton_entity_;
    Entity primitive_types_[8]{};
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

    void write(std::ostream& out, const SnapshotComponentOptions& options = {}) const;
    static DeltaSnapshot read(std::istream& in);
    void write_native(std::ostream& out, const SnapshotComponentOptions& options = {}) const;
    static DeltaSnapshot read_native(std::istream& in);

private:
    friend class Registry;
    friend class PersistentSnapshotAccess;
    friend void write_persistent_delta_snapshot(
        std::ostream& out,
        const Registry::DeltaSnapshot& snapshot,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::DeltaSnapshot read_persistent_delta_snapshot(
        std::istream& in,
        const Registry& schema,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization);

    std::vector<std::uint64_t> entities_;
    std::uint32_t free_head_ = invalid_index;
    std::unordered_map<std::uint32_t, ComponentRecord> components_;
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages_;
    bool has_entities_ = true;
    std::uint64_t baseline_token_ = 0;
    std::uint64_t state_token_ = 0;
};

namespace detail {

template <typename Traits, typename Quantized, typename = void>
struct has_context_component_serialize : std::false_type {};

template <typename Traits, typename Quantized>
struct has_context_component_serialize<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::serialize(
        static_cast<const Quantized*>(nullptr),
        std::declval<const Quantized&>(),
        std::declval<BitBuffer&>(),
        std::declval<ComponentSerializationContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized, typename = void>
struct has_context_component_deserialize : std::false_type {};

template <typename Traits, typename Quantized>
struct has_context_component_deserialize<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::deserialize(
        std::declval<BitBuffer&>(),
        static_cast<const Quantized*>(nullptr),
        std::declval<Quantized&>(),
        std::declval<ComponentSerializationContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
void serialize_component_quantized(
    const Quantized* previous,
    const Quantized& current,
    BitBuffer& out,
    ComponentSerializationContext* context) {
    if constexpr (has_context_component_serialize<Traits, Quantized>::value) {
        ComponentSerializationContext empty_context;
        Traits::serialize(previous, current, out, context != nullptr ? *context : empty_context);
    } else {
        (void)context;
        Traits::serialize(previous, current, out);
    }
}

template <typename Traits, typename Quantized>
bool deserialize_component_quantized(
    BitBuffer& in,
    const Quantized* previous,
    Quantized& out,
    ComponentSerializationContext* context) {
    if constexpr (has_context_component_deserialize<Traits, Quantized>::value) {
        ComponentSerializationContext empty_context;
        return Traits::deserialize(in, previous, out, context != nullptr ? *context : empty_context);
    } else {
        (void)context;
        return Traits::deserialize(in, previous, out);
    }
}

}  // namespace detail

template <typename T, typename Traits = ComponentSerializationTraits<T>>
ComponentSerializationOps make_component_serialization_ops(std::string name = {}) {
    using Quantized = typename Traits::Quantized;
    static_assert(
        std::is_trivially_copyable<Quantized>::value,
        "component serialization quantized state must be trivially copyable");
    static_assert(
        std::is_copy_constructible<T>::value,
        "component serialization components must be copy constructible when decoded");

    ComponentSerializationOps ops;
    ops.name = std::move(name);
    ops.component_size = sizeof(T);
    ops.quantized_size = sizeof(Quantized);
    ops.quantize = [](const void* value, std::uint8_t* out) {
        const Quantized quantized = Traits::quantize(*static_cast<const T*>(value));
        std::memcpy(out, &quantized, sizeof(Quantized));
    };
    ops.dequantize = [](const std::uint8_t* quantized_bytes, void* out) {
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
        *static_cast<T*>(out) = Traits::dequantize(quantized);
    };
    ops.push_to_registry = [](Registry& registry, Entity entity, const std::uint8_t* quantized_bytes) {
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
        if constexpr (is_singleton_component<T>::value) {
            (void)entity;
            registry.write<T>() = Traits::dequantize(quantized);
            return true;
        } else {
            return registry.add<T>(entity, Traits::dequantize(quantized)) != nullptr;
        }
    };
    ops.serialize = [](
        const std::uint8_t* previous_bytes,
        const std::uint8_t* current_bytes,
        BitBuffer& out,
        ComponentSerializationContext* context) {
        Quantized current{};
        std::memcpy(&current, current_bytes, sizeof(Quantized));

        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr) {
            std::memcpy(&previous, previous_bytes, sizeof(Quantized));
            previous_ptr = &previous;
        }

        detail::serialize_component_quantized<Traits, Quantized>(previous_ptr, current, out, context);
    };
    ops.deserialize = [](
        BitBuffer& in,
        const std::uint8_t* previous_bytes,
        std::uint8_t* out,
        ComponentSerializationContext* context) {
        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr) {
            std::memcpy(&previous, previous_bytes, sizeof(Quantized));
            previous_ptr = &previous;
        }

        Quantized quantized{};
        if (!detail::deserialize_component_quantized<Traits, Quantized>(in, previous_ptr, quantized, context)) {
            return false;
        }
        std::memcpy(out, &quantized, sizeof(Quantized));
        return true;
    };
    ops.uses_context =
        detail::has_context_component_serialize<Traits, Quantized>::value ||
        detail::has_context_component_deserialize<Traits, Quantized>::value;
    return ops;
}

class ComponentSerializationRegistry {
public:
    template <typename T, typename Traits = ComponentSerializationTraits<T>>
    Entity register_component(Registry& registry, std::string name = {}) {
        using Quantized = typename Traits::Quantized;
        std::string component_name = name;
        const Entity component = registry.register_component<T>(std::move(name));
        const auto* record_info = registry.component_info(component);
        if (record_info == nullptr || record_info->tag) {
            throw std::logic_error("component serialization cannot be registered for tags");
        }
        const auto found_record = registry.component_catalog_.records.find(Registry::entity_index(component));
        if (found_record == registry.component_catalog_.records.end() || found_record->second.name.empty()) {
            throw std::logic_error("component serialization requires a non-empty component name");
        }

        Entry entry;
        entry.ops = make_component_serialization_ops<T, Traits>(component_name);
        entry.ops.component = component;
        entry.emplace = [](Registry::TypeErasedStorage& storage, std::uint32_t index, const std::uint8_t* quantized) {
            Quantized quantized_value{};
            std::memcpy(&quantized_value, quantized, sizeof(Quantized));
            const T value = Traits::dequantize(quantized_value);
            storage.emplace_or_replace_copy(index, &value);
        };

        entries_[found_record->second.name] = std::move(entry);
        return component;
    }

    const ComponentSerializationOps* find(std::string const& name) const {
        const auto found = entries_.find(name);
        return found == entries_.end() ? nullptr : &found->second.ops;
    }

    const ComponentSerializationOps* find(Entity component) const {
        for (const auto& entry : entries_) {
            if (entry.second.ops.component == component) {
                return &entry.second.ops;
            }
        }
        return nullptr;
    }

private:
    friend class PersistentSnapshotAccess;
    friend void write_persistent_snapshot(
        std::ostream& out,
        const Registry::Snapshot& snapshot,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::Snapshot read_persistent_snapshot(
        std::istream& in,
        const Registry& schema,
        const ComponentSerializationRegistry& serialization);
    friend void write_persistent_delta_snapshot(
        std::ostream& out,
        const Registry::DeltaSnapshot& snapshot,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization,
        const SnapshotComponentOptions& options);
    friend Registry::DeltaSnapshot read_persistent_delta_snapshot(
        std::istream& in,
        const Registry& schema,
        const Registry::Snapshot& baseline,
        const ComponentSerializationRegistry& serialization);

    struct Entry {
        ComponentSerializationOps ops;
        std::function<void(Registry::TypeErasedStorage&, std::uint32_t, const std::uint8_t*)> emplace;
    };

    std::unordered_map<std::string, Entry> entries_;
};

void write_persistent_snapshot(
    std::ostream& out,
    const Registry::Snapshot& snapshot,
    const ComponentSerializationRegistry& serialization,
    const SnapshotComponentOptions& options = {});
Registry::Snapshot read_persistent_snapshot(
    std::istream& in,
    const Registry& schema,
    const ComponentSerializationRegistry& serialization);
void write_persistent_delta_snapshot(
    std::ostream& out,
    const Registry::DeltaSnapshot& snapshot,
    const Registry::Snapshot& baseline,
    const ComponentSerializationRegistry& serialization,
    const SnapshotComponentOptions& options = {});
Registry::DeltaSnapshot read_persistent_delta_snapshot(
    std::istream& in,
    const Registry& schema,
    const Registry::Snapshot& baseline,
    const ComponentSerializationRegistry& serialization);

struct RegistryDirtyFrame {
    Registry& registry;
    Registry::DirtyView dirty;
};

class RegistryDirtyFrameBroadcastListener {
public:
    virtual ~RegistryDirtyFrameBroadcastListener() = default;
    virtual void on_registry_dirty_frame(const RegistryDirtyFrame& frame) = 0;
};

class RegistryDirtyFrameBroadcastSubscription {
public:
    RegistryDirtyFrameBroadcastSubscription() = default;
    RegistryDirtyFrameBroadcastSubscription(const RegistryDirtyFrameBroadcastSubscription&) = delete;
    RegistryDirtyFrameBroadcastSubscription& operator=(const RegistryDirtyFrameBroadcastSubscription&) = delete;
    RegistryDirtyFrameBroadcastSubscription(RegistryDirtyFrameBroadcastSubscription&& other) noexcept;
    RegistryDirtyFrameBroadcastSubscription& operator=(RegistryDirtyFrameBroadcastSubscription&& other) noexcept;
    ~RegistryDirtyFrameBroadcastSubscription();

    void reset();
    bool active() const noexcept;

private:
    friend class RegistryDirtyFrameBroadcaster;

    struct Entry {
        std::uint64_t id = 0;
        RegistryDirtyFrameBroadcastListener* listener = nullptr;
    };

    struct State {
        std::uint64_t next_id = 1;
        std::vector<Entry> consumers;
    };

    RegistryDirtyFrameBroadcastSubscription(std::shared_ptr<State> state, std::uint64_t id);

    std::weak_ptr<State> state_;
    std::uint64_t id_ = 0;
};

class RegistryDirtyFrameBroadcaster {
public:
    RegistryDirtyFrameBroadcaster();

    RegistryDirtyFrameBroadcastSubscription subscribe(RegistryDirtyFrameBroadcastListener& listener);
    void broadcast(Registry& registry);

private:
    void remove_unsubscribed();

    std::shared_ptr<RegistryDirtyFrameBroadcastSubscription::State> listeners_;
};

enum class DirtySnapshotFrameKind : std::uint32_t {
    Full = 1,
    Delta = 2
};

struct DirtySnapshotFrame {
    Registry* registry = nullptr;
    DirtySnapshotFrameKind kind = DirtySnapshotFrameKind::Full;
    const Registry::Snapshot* full = nullptr;
    const Registry::DeltaSnapshot* delta = nullptr;
};

struct DirtySnapshotTrackerOptions {
    SnapshotComponentOptions component_options;
    std::uint64_t full_snapshot_interval_dirty_frames = 60;
    std::function<void(const DirtySnapshotFrame&)> write;
};

class DirtySnapshotTracker final : public RegistryDirtyFrameBroadcastListener {
public:
    explicit DirtySnapshotTracker(DirtySnapshotTrackerOptions options = {});
    ~DirtySnapshotTracker();

    DirtySnapshotTracker(const DirtySnapshotTracker&) = delete;
    DirtySnapshotTracker& operator=(const DirtySnapshotTracker&) = delete;
    DirtySnapshotTracker(DirtySnapshotTracker&&) noexcept;
    DirtySnapshotTracker& operator=(DirtySnapshotTracker&&) noexcept;

    void on_registry_dirty_frame(const RegistryDirtyFrame& frame) override;

private:
    DirtySnapshotTrackerOptions options_;
    std::uint64_t dirty_frame_count_ = 0;
    std::unique_ptr<Registry::Snapshot> last_full_;
    std::unique_ptr<Registry::DeltaSnapshot> last_delta_;
};

template <typename... Components>
class Registry::View {
    static_assert(sizeof...(Components) > 0, "ashiato views require at least one component");
    static_assert(detail::unique_components<Components...>::value, "ashiato views cannot repeat component types");
    static_assert(!detail::contains_tag_query<Components...>::value, "ashiato tags must be view filters");

public:
    explicit View(Registry& registry)
        : registry_(&registry),
          storages_{{resolve_storage<Components>(registry)...}},
          group_(registry.best_group_for_view<Components...>()),
          cache_token_(registry.storage_registry_.view_topology_token) {}

    template <typename Fn>
    void each(Fn&& fn) {
        refresh_cache_if_needed();
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<Components...>::value) {
                Fn& callback = fn;
                call_each(callback, Entity{}, entity_index(registry_->component_catalog_.singleton_entity));
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

            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    std::vector<std::uint32_t> matching_indices() const {
        refresh_cache_if_needed();
        std::vector<std::uint32_t> indices;
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<Components...>::value) {
                indices.push_back(entity_index(registry_->component_catalog_.singleton_entity));
            }
            return indices;
        }

        const std::size_t dense_size = group_ != nullptr ? group_->size : driver->dense_size();
        indices.reserve(dense_size);
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (contains_all(index)) {
                indices.push_back(index);
            }
        }
        return indices;
    }

    template <typename Fn>
    void each_index_range(Fn&& fn, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end) {
        refresh_cache_if_needed();
        Fn& callback = fn;
        for (std::size_t position = begin; position < end; ++position) {
            const std::uint32_t index = indices[position];
            if (!contains_all(index)) {
                continue;
            }
            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    template <typename... AccessComponents>
    AccessView<detail::type_list<Components...>, detail::type_list<AccessComponents...>, detail::type_list<>> access() const {
        refresh_cache_if_needed();
        return AccessView<detail::type_list<Components...>, detail::type_list<AccessComponents...>, detail::type_list<>>(
            *registry_,
            storages_);
    }

    template <typename... OptionalComponents>
    AccessView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<OptionalComponents...>> optional()
        const {
        refresh_cache_if_needed();
        return AccessView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<OptionalComponents...>>(
            *registry_,
            storages_);
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const {
        refresh_cache_if_needed();
        return TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<Tags...>,
            detail::type_list<>>(
            *registry_,
            storages_,
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{});
    }

    TagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
    with_tags(std::initializer_list<Entity> tags) const {
        refresh_cache_if_needed();
        TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>>
            view(
                *registry_,
                storages_,
                std::array<TypeErasedStorage*, 0>{},
                std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_with_tags(tags);
        return view;
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const {
        refresh_cache_if_needed();
        return TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<Tags...>>(
            *registry_,
            storages_,
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{});
    }

    TagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
    without_tags(std::initializer_list<Entity> tags) const {
        refresh_cache_if_needed();
        TagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>>
            view(
                *registry_,
                storages_,
                std::array<TypeErasedStorage*, 0>{},
                std::array<TypeErasedStorage*, 0>{});
        view.add_runtime_without_tags(tags);
        return view;
    }

    template <typename... ViewComponents>
    typename std::enable_if<
        detail::requested_components_allowed<detail::type_list<Components...>>::template with<ViewComponents...>::
            value &&
            !detail::contains_tag_query<ViewComponents...>::value,
        View<ViewComponents...>>::type
    view() const {
        return View<ViewComponents...>(*registry_);
    }

    View& job_callback_scope() {
        job_callback_scope_ = true;
        return *this;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, Components...>::value && !detail::is_singleton_query<T>::value,
            int>::type = 0>
    bool contains(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return false;
        }
        const std::uint32_t index = entity_index(entity);
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr && storage->contains_index(index);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, Components...>::value && !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>* try_get(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return nullptr;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr
            ? static_cast<const detail::component_query_t<T>*>(storage->get(entity_index(entity)))
            : nullptr;
    }

    template <typename T, typename std::enable_if<detail::contains_component<T, Components...>::value, int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        refresh_cache_if_needed();
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->component_catalog_.singleton_entity);
        }

        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value && detail::contains_component<T, Components...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        refresh_cache_if_needed();
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(
            storage->get_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
    }

    template <typename T, typename std::enable_if<
                              !std::is_const<typename std::remove_reference<T>::type>::value &&
                                  detail::contains_mutable_component<T, Components...>::value,
                              int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        refresh_cache_if_needed();
        std::uint32_t index = entity_index(entity);
        if constexpr (detail::is_singleton_query<T>::value) {
            index = entity_index(registry_->component_catalog_.singleton_entity);
        }

        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(storage->write_unchecked(index));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, Components...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        refresh_cache_if_needed();
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(
            storage->write_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
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

    void refresh_cache_if_needed() const {
        if (cache_token_ == registry_->storage_registry_.view_topology_token) {
            return;
        }
        storages_ = {{resolve_storage<Components>(*registry_)...}};
        group_ = registry_->best_group_for_view<Components...>();
        cache_token_ = registry_->storage_registry_.view_topology_token;
    }

    TypeErasedStorage* driver_storage() const {
        if (group_ != nullptr) {
            return registry_->find_storage(Entity{registry_->entity_store_.slots[group_->owned.front()]});
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
        static_assert(has_component<T>(), "component is not part of this ashiato view");
        constexpr std::size_t position = component_position<T, Components...>();
        return storages_[position];
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        static_assert(has_component<T>(), "component is not part of this ashiato view");
        constexpr std::size_t position = component_position<T, Components...>();
        return storages_[position];
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        if (job_callback_scope_) {
            detail::RegistryAccessForbiddenScope scope;
            call_each_unchecked(callback, entity, index);
            return;
        }
#endif
        call_each_unchecked(callback, entity, index);
    }

    template <typename Fn>
    void call_each_unchecked(Fn& callback, Entity entity, std::uint32_t index) {
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
            index = entity_index(registry_->component_catalog_.singleton_entity);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(index));
        } else {
            void* value = TypeErasedStorage::range_dirty_deferred()
                ? storage->write_unchecked_without_dirty(index)
                : storage->write_unchecked(index);
            return *static_cast<detail::component_query_t<T>*>(value);
        }
    }

    Registry* registry_;
    mutable std::array<TypeErasedStorage*, component_count> storages_;
    mutable GroupRecord* group_ = nullptr;
    mutable std::uint64_t cache_token_ = 0;
    bool job_callback_scope_ = false;
};

template <>
class Registry::View<> {
public:
    explicit View(Registry& registry)
        : registry_(&registry) {}

    template <typename Fn>
    void each(Fn&& fn) {
        Fn& callback = fn;
        for (std::uint32_t index = 0; index < registry_->entity_store_.slots.size(); ++index) {
            Entity entity{registry_->entity_store_.slots[index]};
            if (!registry_->alive(entity)) {
                continue;
            }
            call_each(callback, entity);
        }
    }

    std::vector<std::uint32_t> matching_indices() const {
        std::vector<std::uint32_t> indices;
        indices.reserve(registry_->entity_store_.slots.size());
        for (std::uint32_t index = 0; index < registry_->entity_store_.slots.size(); ++index) {
            Entity entity{registry_->entity_store_.slots[index]};
            if (registry_->alive(entity)) {
                indices.push_back(index);
            }
        }
        return indices;
    }

    template <typename Fn>
    void each_index_range(Fn&& fn, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end) {
        Fn& callback = fn;
        for (std::size_t position = begin; position < end; ++position) {
            const std::uint32_t index = indices[position];
            if (index >= registry_->entity_store_.slots.size()) {
                continue;
            }
            Entity entity{registry_->entity_store_.slots[index]};
            if (registry_->alive(entity)) {
                call_each(callback, entity);
            }
        }
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const;

    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
    with_tags(std::initializer_list<Entity> tags) const;

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const;

    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
    without_tags(std::initializer_list<Entity> tags) const;

    template <typename... ViewComponents>
    typename std::enable_if<!detail::contains_tag_query<ViewComponents...>::value, View<ViewComponents...>>::type
    view() const {
        return View<ViewComponents...>(*registry_);
    }

    View& job_callback_scope() {
        job_callback_scope_ = true;
        return *this;
    }

private:
    template <typename Fn>
    void call_each(Fn& callback, Entity entity) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        if (job_callback_scope_) {
            detail::RegistryAccessForbiddenScope scope;
            call_each_unchecked(callback, entity);
            return;
        }
#endif
        call_each_unchecked(callback, entity);
    }

    template <typename Fn>
    void call_each_unchecked(Fn& callback, Entity entity) {
        if constexpr (std::is_invocable<Fn&, View&, Entity>::value) {
            callback(*this, entity);
        } else {
            callback(entity);
        }
    }

    Registry* registry_;
    bool job_callback_scope_ = false;
};

template <typename... IterComponents, typename... AccessComponents, typename... OptionalComponents>
class Registry::AccessView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<OptionalComponents...>> {
    static_assert(sizeof...(IterComponents) > 0, "ashiato views require at least one component");
    static_assert(detail::unique_components<IterComponents...>::value, "ashiato views cannot repeat component types");
    static_assert(detail::unique_components<AccessComponents...>::value, "ashiato access components cannot repeat types");
    static_assert(detail::unique_components<OptionalComponents...>::value, "ashiato optional components cannot repeat types");
    static_assert(
        detail::disjoint_from<detail::type_list<AccessComponents...>>::template with<OptionalComponents...>::value,
        "ashiato access and optional components cannot repeat types");
    static_assert(
        detail::access_components_allowed<detail::type_list<IterComponents...>>::template with<AccessComponents...>::
            value,
        "ashiato access components can only repeat iterated const components as mutable access components");
    static_assert(
        detail::access_components_allowed<detail::type_list<IterComponents...>>::template with<OptionalComponents...>::
            value,
        "ashiato optional components can only repeat iterated const components as mutable optional components");

public:
    AccessView(Registry& registry, std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages)
        : registry_(&registry),
          iter_storages_(iter_storages),
          access_storages_{{resolve_storage<AccessComponents>(registry)...}},
          optional_storages_{{resolve_storage<OptionalComponents>(registry)...}},
          group_(registry.best_group_for_view<IterComponents...>()),
          cache_token_(registry.storage_registry_.view_topology_token) {}

    template <typename Fn>
    void each(Fn&& fn) {
        refresh_cache_if_needed();
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<IterComponents...>::value) {
                Fn& callback = fn;
                call_each(callback, Entity{}, entity_index(registry_->component_catalog_.singleton_entity));
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

            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    std::vector<std::uint32_t> matching_indices() const {
        refresh_cache_if_needed();
        std::vector<std::uint32_t> indices;
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (!detail::contains_non_singleton_component<IterComponents...>::value) {
                indices.push_back(entity_index(registry_->component_catalog_.singleton_entity));
            }
            return indices;
        }

        const std::size_t dense_size = group_ != nullptr ? group_->size : driver->dense_size();
        indices.reserve(dense_size);
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (contains_all(index)) {
                indices.push_back(index);
            }
        }
        return indices;
    }

    template <typename Fn>
    void each_index_range(Fn&& fn, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end) {
        refresh_cache_if_needed();
        Fn& callback = fn;
        for (std::size_t position = begin; position < end; ++position) {
            const std::uint32_t index = indices[position];
            if (!contains_all(index)) {
                continue;
            }
            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    template <typename... MoreOptionalComponents>
    auto optional() const
        -> AccessView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            typename detail::type_list_concat<
                detail::type_list<OptionalComponents...>,
                detail::type_list<MoreOptionalComponents...>>::type> {
        refresh_cache_if_needed();
        using NextOptional = typename detail::type_list_concat<
            detail::type_list<OptionalComponents...>,
            detail::type_list<MoreOptionalComponents...>>::type;
        return AccessView<detail::type_list<IterComponents...>, detail::type_list<AccessComponents...>, NextOptional>(
            *registry_,
            iter_storages_);
    }

    template <typename... MoreAccessComponents>
    auto access() const
        -> AccessView<
            detail::type_list<IterComponents...>,
            typename detail::type_list_concat<
                detail::type_list<AccessComponents...>,
                detail::type_list<MoreAccessComponents...>>::type,
            detail::type_list<OptionalComponents...>> {
        refresh_cache_if_needed();
        using NextAccess = typename detail::type_list_concat<
            detail::type_list<AccessComponents...>,
            detail::type_list<MoreAccessComponents...>>::type;
        return AccessView<detail::type_list<IterComponents...>, NextAccess, detail::type_list<OptionalComponents...>>(
            *registry_,
            iter_storages_);
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const {
        refresh_cache_if_needed();
        return TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<Tags...>,
            detail::type_list<>>(*registry_, iter_storages_, access_storages_, optional_storages_);
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    with_tags(std::initializer_list<Entity> tags) const {
        refresh_cache_if_needed();
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_, optional_storages_);
        view.add_runtime_with_tags(tags);
        return view;
    }

    template <typename... Tags>
    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const {
        refresh_cache_if_needed();
        return TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<>,
            detail::type_list<Tags...>>(*registry_, iter_storages_, access_storages_, optional_storages_);
    }

    TagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<>,
        detail::type_list<>>
    without_tags(std::initializer_list<Entity> tags) const {
        refresh_cache_if_needed();
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<>,
            detail::type_list<>>
            view(*registry_, iter_storages_, access_storages_, optional_storages_);
        view.add_runtime_without_tags(tags);
        return view;
    }

    template <typename... ViewComponents>
    typename std::enable_if<
        detail::requested_components_allowed<detail::type_list<IterComponents..., AccessComponents...>>::
            template with<ViewComponents...>::value &&
            !detail::contains_tag_query<ViewComponents...>::value,
        View<ViewComponents...>>::type
    view() const {
        return View<ViewComponents...>(*registry_);
    }

    AccessView& job_callback_scope() {
        job_callback_scope_ = true;
        return *this;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    bool contains(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return false;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr && storage->contains_index(entity_index(entity));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    bool contains() const {
        refresh_cache_if_needed();
        if (active_callback_index_ == invalid_index) {
            return false;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr && storage->contains_index(active_callback_index_);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>* try_get(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return nullptr;
        }
        const std::uint32_t index = entity_index(entity);
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr
            ? static_cast<const detail::component_query_t<T>*>(storage->get(index))
            : nullptr;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>* try_get() const {
        refresh_cache_if_needed();
        if (active_callback_index_ == invalid_index) {
            return nullptr;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr
            ? static_cast<const detail::component_query_t<T>*>(storage->get(active_callback_index_))
            : nullptr;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        refresh_cache_if_needed();
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(entity_index(entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        refresh_cache_if_needed();
        assert(active_callback_index_ != invalid_index);
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(active_callback_index_));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        refresh_cache_if_needed();
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(
            storage->get_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        refresh_cache_if_needed();
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(storage->write_unchecked(entity_index(entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        refresh_cache_if_needed();
        assert(active_callback_index_ != invalid_index);
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(storage->write_unchecked(active_callback_index_));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        refresh_cache_if_needed();
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(
            storage->write_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
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

    void refresh_cache_if_needed() const {
        if (cache_token_ == registry_->storage_registry_.view_topology_token) {
            return;
        }
        iter_storages_ = {{resolve_storage<IterComponents>(*registry_)...}};
        access_storages_ = {{resolve_storage<AccessComponents>(*registry_)...}};
        optional_storages_ = {{resolve_storage<OptionalComponents>(*registry_)...}};
        group_ = registry_->best_group_for_view<IterComponents...>();
        cache_token_ = registry_->storage_registry_.view_topology_token;
    }

    TypeErasedStorage* driver_storage() const {
        if (group_ != nullptr) {
            return registry_->find_storage(Entity{registry_->entity_store_.slots[group_->owned.front()]});
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
            if constexpr (detail::contains_component<T, AccessComponents...>::value) {
                constexpr std::size_t position = component_position<T, AccessComponents...>();
                return access_storages_[position];
            } else {
                static_assert(
                    detail::contains_component<T, OptionalComponents...>::value,
                    "component is not part of this ashiato view");
                constexpr std::size_t position = component_position<T, OptionalComponents...>();
                return optional_storages_[position];
            }
        }
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            if constexpr (detail::contains_component<T, AccessComponents...>::value) {
                constexpr std::size_t position = component_position<T, AccessComponents...>();
                return access_storages_[position];
            } else {
                static_assert(
                    detail::contains_component<T, OptionalComponents...>::value,
                    "component is not part of this ashiato view");
                constexpr std::size_t position = component_position<T, OptionalComponents...>();
                return optional_storages_[position];
            }
        }
    }

    template <typename T>
    void require_optional_entity(std::uint32_t index) const {
        if constexpr (detail::contains_component<T, OptionalComponents...>::value &&
                      !detail::contains_component<T, IterComponents..., AccessComponents...>::value &&
                      !detail::is_singleton_query<T>::value) {
            if (active_callback_index_ != invalid_index && active_callback_index_ != index) {
                throw std::logic_error("ashiato optional job access is limited to the entity currently being iterated");
            }
        }
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        if (job_callback_scope_) {
            detail::RegistryAccessForbiddenScope scope;
            call_each_unchecked(callback, entity, index);
            return;
        }
#endif
        call_each_unchecked(callback, entity, index);
    }

    template <typename Fn>
    void call_each_unchecked(Fn& callback, Entity entity, std::uint32_t index) {
        const std::uint32_t previous_active_callback_index = active_callback_index_;
        active_callback_index_ = index;
        struct ActiveIndexRestore {
            std::uint32_t& target;
            std::uint32_t previous;
            ~ActiveIndexRestore() {
                target = previous;
            }
        } restore{active_callback_index_, previous_active_callback_index};
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
            index = entity_index(registry_->component_catalog_.singleton_entity);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(index));
        } else {
            void* value = TypeErasedStorage::range_dirty_deferred()
                ? storage->write_unchecked_without_dirty(index)
                : storage->write_unchecked(index);
            return *static_cast<detail::component_query_t<T>*>(value);
        }
    }

    Registry* registry_;
    mutable std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(OptionalComponents)> optional_storages_;
    mutable GroupRecord* group_ = nullptr;
    mutable std::uint64_t cache_token_ = 0;
    std::uint32_t active_callback_index_ = invalid_index;
    bool job_callback_scope_ = false;
};

template <
    typename... IterComponents,
    typename... AccessComponents,
    typename... OptionalComponents,
    typename... WithTags,
    typename... WithoutTags>
class Registry::TagFilteredView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<OptionalComponents...>,
    detail::type_list<WithTags...>,
    detail::type_list<WithoutTags...>> {
    static_assert((!detail::is_tag_query<IterComponents>::value && ...), "ashiato tags must be view filters");
    static_assert((!detail::is_tag_query<AccessComponents>::value && ...), "ashiato tags cannot be access components");
    static_assert((!detail::is_tag_query<OptionalComponents>::value && ...), "ashiato tags cannot be optional components");
    static_assert((detail::is_tag_query<WithTags>::value && ...), "ashiato with_tags types must be empty tags");
    static_assert((detail::is_tag_query<WithoutTags>::value && ...), "ashiato without_tags types must be empty tags");

public:
    TagFilteredView(
        Registry& registry,
        std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages,
        std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages,
        std::array<TypeErasedStorage*, sizeof...(OptionalComponents)> optional_storages)
        : registry_(&registry),
          iter_storages_(iter_storages),
          access_storages_(access_storages),
          optional_storages_(optional_storages),
          with_tag_storages_{{resolve_tag_storage<WithTags>(registry)...}},
          without_tag_storages_{{resolve_tag_storage<WithoutTags>(registry)...}},
          cache_token_(registry.storage_registry_.view_topology_token) {}

    void add_runtime_with_tags(std::initializer_list<Entity> tags) {
        append_runtime_tags(tags, runtime_with_tags_, runtime_with_tag_storages_);
    }

    void add_runtime_without_tags(std::initializer_list<Entity> tags) {
        append_runtime_tags(tags, runtime_without_tags_, runtime_without_tag_storages_);
    }

    template <typename... Tags>
    auto with_tags() const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type,
            detail::type_list<WithoutTags...>> {
        refresh_cache_if_needed();
        using NextWith =
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type;
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            NextWith,
            detail::type_list<WithoutTags...>>
            view(*registry_, iter_storages_, access_storages_, optional_storages_);
        copy_runtime_filters_to(view);
        return view;
    }

    auto with_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        refresh_cache_if_needed();
        auto view = *this;
        view.add_runtime_with_tags(tags);
        return view;
    }

    template <typename... Tags>
    auto without_tags() const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type> {
        refresh_cache_if_needed();
        using NextWithout =
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type;
        TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            NextWithout>
            view(*registry_, iter_storages_, access_storages_, optional_storages_);
        copy_runtime_filters_to(view);
        return view;
    }

    auto without_tags(std::initializer_list<Entity> tags) const
        -> TagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        refresh_cache_if_needed();
        auto view = *this;
        view.add_runtime_without_tags(tags);
        return view;
    }

    template <typename Fn>
    void each(Fn&& fn) {
        refresh_cache_if_needed();
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (sizeof...(IterComponents) == 0 && sizeof...(WithTags) == 0) {
                if (runtime_with_tags_.empty()) {
                    Fn& callback = fn;
                    for (std::uint32_t index = 0; index < registry_->entity_store_.slots.size(); ++index) {
                        Entity entity{registry_->entity_store_.slots[index]};
                        if (registry_->alive(entity) && contains_all(index)) {
                            call_each(callback, entity, index);
                        }
                    }
                }
            }
            return;
        }

        Fn& callback = fn;
        const std::size_t dense_size = driver->dense_size();
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (!contains_all(index)) {
                continue;
            }

            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    std::vector<std::uint32_t> matching_indices() {
        refresh_cache_if_needed();
        std::vector<std::uint32_t> indices;
        TypeErasedStorage* driver = driver_storage();
        if (driver == nullptr) {
            if constexpr (sizeof...(IterComponents) == 0 && sizeof...(WithTags) == 0) {
                if (runtime_with_tags_.empty()) {
                    indices.reserve(registry_->entity_store_.slots.size());
                    for (std::uint32_t index = 0; index < registry_->entity_store_.slots.size(); ++index) {
                        Entity entity{registry_->entity_store_.slots[index]};
                        if (registry_->alive(entity) && contains_all(index)) {
                            indices.push_back(index);
                        }
                    }
                }
            }
            return indices;
        }

        const std::size_t dense_size = driver->dense_size();
        indices.reserve(dense_size);
        for (std::size_t dense = 0; dense < dense_size; ++dense) {
            const std::uint32_t index = driver->dense_index_at(dense);
            if (contains_all(index)) {
                indices.push_back(index);
            }
        }
        return indices;
    }

    template <typename Fn>
    void each_index_range(Fn&& fn, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end) {
        refresh_cache_if_needed();
        Fn& callback = fn;
        for (std::size_t position = begin; position < end; ++position) {
            const std::uint32_t index = indices[position];
            if (!contains_all(index)) {
                continue;
            }
            call_each(callback, Entity{registry_->entity_store_.slots[index]}, index);
        }
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    bool contains(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return false;
        }
        const std::uint32_t index = entity_index(entity);
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr && storage->contains_index(index);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    bool contains() const {
        refresh_cache_if_needed();
        if (active_callback_index_ == invalid_index) {
            return false;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr && storage->contains_index(active_callback_index_);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>* try_get(Entity entity) const {
        refresh_cache_if_needed();
        if (!registry_->alive(entity)) {
            return nullptr;
        }
        const std::uint32_t index = entity_index(entity);
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr
            ? static_cast<const detail::component_query_t<T>*>(storage->get(index))
            : nullptr;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>* try_get() const {
        refresh_cache_if_needed();
        if (active_callback_index_ == invalid_index) {
            return nullptr;
        }
        const TypeErasedStorage* storage = storage_for_type<T>();
        return storage != nullptr
            ? static_cast<const detail::component_query_t<T>*>(storage->get(active_callback_index_))
            : nullptr;
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        refresh_cache_if_needed();
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(entity_index(entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        refresh_cache_if_needed();
        assert(active_callback_index_ != invalid_index);
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(active_callback_index_));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        refresh_cache_if_needed();
        const TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<const detail::component_query_t<T>*>(
            storage->get_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, AccessComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        refresh_cache_if_needed();
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(storage->write_unchecked(entity_index(entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., OptionalComponents...>::value &&
                !detail::is_singleton_query<T>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        refresh_cache_if_needed();
        assert(active_callback_index_ != invalid_index);
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(storage->write_unchecked(active_callback_index_));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        refresh_cache_if_needed();
        TypeErasedStorage* storage = storage_for_type<T>();
        assert(storage != nullptr);

        return *static_cast<detail::component_query_t<T>*>(
            storage->write_unchecked(entity_index(registry_->component_catalog_.singleton_entity)));
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool has_tag(Entity entity) const {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template has<detail::component_query_t<T>>(entity);
    }

    bool has_tag(Entity entity, Entity tag) const {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->has(entity, tag);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_mutable_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool add_tag(Entity entity) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template add<detail::component_query_t<T>>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_mutable_component<T, WithTags..., WithoutTags...>::value,
            int>::type = 0>
    bool remove_tag(Entity entity) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template remove<detail::component_query_t<T>>(entity);
    }

    template <typename... ViewComponents>
    typename std::enable_if<
        detail::requested_components_allowed<detail::type_list<IterComponents..., AccessComponents...>>::
            template with<ViewComponents...>::value &&
            !detail::contains_tag_query<ViewComponents...>::value,
        decltype(std::declval<View<ViewComponents...>>()
                     .template with_tags<WithTags...>()
                     .template without_tags<WithoutTags...>())>::type
    view() const {
        return View<ViewComponents...>(*registry_)
            .template with_tags<WithTags...>()
            .template without_tags<WithoutTags...>();
    }

    TagFilteredView& job_callback_scope() {
        job_callback_scope_ = true;
        return *this;
    }

private:
    template <typename IterList, typename AccessList, typename OptionalList, typename WithList, typename WithoutList>
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

    template <typename T>
    static TypeErasedStorage* resolve_storage(Registry& registry) {
        const Entity component = registry.registered_component<detail::component_query_t<T>>();
        return registry.find_storage(component);
    }

    void append_runtime_tags(
        std::initializer_list<Entity> tags,
        std::vector<Entity>& target,
        std::vector<TypeErasedStorage*>& storage_target) {
        for (Entity tag : tags) {
            registry_->require_tag_component(tag);
            target.push_back(tag);
            storage_target.push_back(registry_->find_storage(tag));
        }
    }

    template <typename OtherView>
    void copy_runtime_filters_to(OtherView& other) const {
        other.runtime_with_tags_ = runtime_with_tags_;
        other.runtime_without_tags_ = runtime_without_tags_;
        other.runtime_with_tag_storages_ = runtime_with_tag_storages_;
        other.runtime_without_tag_storages_ = runtime_without_tag_storages_;
    }

    void refresh_cache_if_needed() const {
        if (cache_token_ == registry_->storage_registry_.view_topology_token) {
            return;
        }
        iter_storages_ = {{resolve_storage<IterComponents>(*registry_)...}};
        access_storages_ = {{resolve_storage<AccessComponents>(*registry_)...}};
        optional_storages_ = {{resolve_storage<OptionalComponents>(*registry_)...}};
        with_tag_storages_ = {{resolve_tag_storage<WithTags>(*registry_)...}};
        without_tag_storages_ = {{resolve_tag_storage<WithoutTags>(*registry_)...}};
        refresh_runtime_tag_storages();
        cache_token_ = registry_->storage_registry_.view_topology_token;
    }

    void refresh_runtime_tag_storages() const {
        refresh_runtime_tag_storages(runtime_with_tags_, runtime_with_tag_storages_);
        refresh_runtime_tag_storages(runtime_without_tags_, runtime_without_tag_storages_);
    }

    void refresh_runtime_tag_storages(
        const std::vector<Entity>& tags,
        std::vector<TypeErasedStorage*>& storages) const {
        assert(tags.size() == storages.size());
        for (std::size_t i = 0; i < tags.size(); ++i) {
            storages[i] = registry_->find_storage(tags[i]);
        }
    }

    TypeErasedStorage* driver_storage() const {
        TypeErasedStorage* driver = nullptr;
        if constexpr (sizeof...(IterComponents) > 0) {
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
        }

        for (TypeErasedStorage* storage : with_tag_storages_) {
            if (storage == nullptr) {
                return nullptr;
            }
            if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
                driver = storage;
            }
        }

        for (TypeErasedStorage* storage : runtime_with_tag_storages_) {
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
        if constexpr (sizeof...(IterComponents) > 0) {
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
        for (const TypeErasedStorage* storage : runtime_with_tag_storages_) {
            if (storage == nullptr || !storage->contains_index(index)) {
                return false;
            }
        }
        for (const TypeErasedStorage* storage : runtime_without_tag_storages_) {
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
            if constexpr (detail::contains_component<T, AccessComponents...>::value) {
                constexpr std::size_t position = component_position<T, AccessComponents...>();
                return access_storages_[position];
            } else {
                static_assert(
                    detail::contains_component<T, OptionalComponents...>::value,
                    "component is not part of this ashiato view");
                constexpr std::size_t position = component_position<T, OptionalComponents...>();
                return optional_storages_[position];
            }
        }
    }

    template <typename T>
    const TypeErasedStorage* storage_for_type() const {
        if constexpr (detail::contains_component<T, IterComponents...>::value) {
            constexpr std::size_t position = component_position<T, IterComponents...>();
            return iter_storages_[position];
        } else {
            if constexpr (detail::contains_component<T, AccessComponents...>::value) {
                constexpr std::size_t position = component_position<T, AccessComponents...>();
                return access_storages_[position];
            } else {
                static_assert(
                    detail::contains_component<T, OptionalComponents...>::value,
                    "component is not part of this ashiato view");
                constexpr std::size_t position = component_position<T, OptionalComponents...>();
                return optional_storages_[position];
            }
        }
    }

    template <typename T>
    void require_optional_entity(std::uint32_t index) const {
        if constexpr (detail::contains_component<T, OptionalComponents...>::value &&
                      !detail::contains_component<T, IterComponents..., AccessComponents...>::value &&
                      !detail::is_singleton_query<T>::value) {
            if (active_callback_index_ != invalid_index && active_callback_index_ != index) {
                throw std::logic_error("ashiato optional job access is limited to the entity currently being iterated");
            }
        }
    }

    template <typename Fn>
    void call_each(Fn& callback, Entity entity, std::uint32_t index) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        if (job_callback_scope_) {
            detail::RegistryAccessForbiddenScope scope;
            call_each_unchecked(callback, entity, index);
            return;
        }
#endif
        call_each_unchecked(callback, entity, index);
    }

    template <typename Fn>
    void call_each_unchecked(Fn& callback, Entity entity, std::uint32_t index) {
        const std::uint32_t previous_active_callback_index = active_callback_index_;
        active_callback_index_ = index;
        struct ActiveIndexRestore {
            std::uint32_t& target;
            std::uint32_t previous;
            ~ActiveIndexRestore() {
                target = previous;
            }
        } restore{active_callback_index_, previous_active_callback_index};
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
            index = entity_index(registry_->component_catalog_.singleton_entity);
        }
        if constexpr (std::is_const<typename std::remove_reference<T>::type>::value) {
            return *static_cast<const detail::component_query_t<T>*>(storage->get_unchecked(index));
        } else {
            void* value = TypeErasedStorage::range_dirty_deferred()
                ? storage->write_unchecked_without_dirty(index)
                : storage->write_unchecked(index);
            return *static_cast<detail::component_query_t<T>*>(value);
        }
    }

    Registry* registry_;
    mutable std::array<TypeErasedStorage*, sizeof...(IterComponents)> iter_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(AccessComponents)> access_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(OptionalComponents)> optional_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(WithTags)> with_tag_storages_;
    mutable std::array<TypeErasedStorage*, sizeof...(WithoutTags)> without_tag_storages_;
    std::vector<Entity> runtime_with_tags_;
    std::vector<Entity> runtime_without_tags_;
    mutable std::vector<TypeErasedStorage*> runtime_with_tag_storages_;
    mutable std::vector<TypeErasedStorage*> runtime_without_tag_storages_;
    mutable std::uint64_t cache_token_ = 0;
    std::uint32_t active_callback_index_ = invalid_index;
    bool job_callback_scope_ = false;
};

template <typename... Tags>
Registry::TagFilteredView<
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<Tags...>,
    detail::type_list<>>
Registry::View<>::with_tags() const {
    return TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>,
        detail::type_list<>>(
        *registry_,
        std::array<TypeErasedStorage*, 0>{},
        std::array<TypeErasedStorage*, 0>{},
        std::array<TypeErasedStorage*, 0>{});
}

inline Registry::TagFilteredView<
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>>
Registry::View<>::with_tags(std::initializer_list<Entity> tags) const {
    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
        view(
            *registry_,
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{});
    view.add_runtime_with_tags(tags);
    return view;
}

template <typename... Tags>
Registry::TagFilteredView<
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<Tags...>>
Registry::View<>::without_tags() const {
    return TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>>(
        *registry_,
        std::array<TypeErasedStorage*, 0>{},
        std::array<TypeErasedStorage*, 0>{},
        std::array<TypeErasedStorage*, 0>{});
}

inline Registry::TagFilteredView<
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>,
    detail::type_list<>>
Registry::View<>::without_tags(std::initializer_list<Entity> tags) const {
    TagFilteredView<
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>>
        view(
            *registry_,
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{},
            std::array<TypeErasedStorage*, 0>{});
    view.add_runtime_without_tags(tags);
    return view;
}

template <typename ViewType, typename... StructuralComponents>
class Registry::JobStructuralContext {
public:
    JobStructuralContext(Registry& registry, ViewType& view)
        : registry_(&registry), view_(&view) {}

    template <typename T>
    const detail::component_query_t<T>& get(Entity entity) const {
        return view_->template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    bool contains(Entity entity) const {
        return view_->template contains<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    bool contains() const {
        return view_->template contains<T>();
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    const detail::component_query_t<T>* try_get(Entity entity) const {
        return view_->template try_get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    const detail::component_query_t<T>* try_get() const {
        return view_->template try_get<T>();
    }

    template <
        typename T,
        typename std::enable_if<detail::is_singleton_query<T>::value, int>::type = 0>
    const detail::component_query_t<T>& get() const {
        return view_->template get<T>();
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    const detail::component_query_t<T>& get() const {
        return view_->template get<T>();
    }

    template <typename T>
    detail::component_query_t<T>& write(Entity entity) {
        return view_->template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<detail::is_singleton_query<T>::value, int>::type = 0>
    detail::component_query_t<T>& write() {
        return view_->template write<T>();
    }

    template <
        typename T,
        typename std::enable_if<!detail::is_singleton_query<T>::value, int>::type = 0>
    detail::component_query_t<T>& write() {
        return view_->template write<T>();
    }

    template <typename... ViewComponents>
    auto view() const {
        return view_->template view<ViewComponents...>();
    }

    template <
        typename T,
        typename... Args,
        typename std::enable_if<
            !detail::is_tag_query<T>::value && detail::contains_component<T, StructuralComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>* add(Entity entity, Args&&... args) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template add<detail::component_query_t<T>>(entity, std::forward<Args>(args)...);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_tag_query<T>::value && detail::contains_component<T, StructuralComponents...>::value,
            int>::type = 0>
    bool add(Entity entity) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template add<detail::component_query_t<T>>(entity);
    }

    template <
        typename T,
        typename std::enable_if<detail::contains_component<T, StructuralComponents...>::value, int>::type = 0>
    bool remove(Entity entity) {
#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
        detail::RegistryAccessAllowedScope scope;
#endif
        return registry_->template remove<detail::component_query_t<T>>(entity);
    }

private:
    Registry* registry_;
    ViewType* view_;
};

template <
    typename... IterComponents,
    typename... AccessComponents,
    typename... OptionalComponents,
    typename... WithTags,
    typename... WithoutTags>
class Registry::JobTagFilteredView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<OptionalComponents...>,
    detail::type_list<WithTags...>,
    detail::type_list<WithoutTags...>> {
public:
    JobTagFilteredView(Registry& registry, int order, JobThreadingOptions threading = {}, std::string name = {})
        : registry_(&registry), order_(order), threading_(threading), name_(std::move(name)) {}

    JobTagFilteredView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    JobTagFilteredView& max_threads(std::size_t count) {
        static_assert(sizeof...(AccessComponents) == 0, "ashiato access_other_entities jobs must be single threaded");
        threading_.max_threads = std::max<std::size_t>(count, 1);
        threading_.single_thread = false;
        return *this;
    }

    JobTagFilteredView& single_thread() {
        threading_.max_threads = 1;
        threading_.single_thread = true;
        return *this;
    }

    JobTagFilteredView& min_entities_per_thread(std::size_t count) {
        threading_.min_entities_per_thread = std::max<std::size_t>(count, 1);
        return *this;
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        JobAccessMetadata metadata =
            registry_->template make_job_access_metadata<IterComponents..., AccessComponents..., OptionalComponents...>();
        registry_->template append_job_filter_metadata<WithTags..., WithoutTags...>(metadata);
        return registry_->add_job(
            order_,
            name_,
            std::move(metadata),
            [callback](Registry& registry) mutable {
                auto view = make_view(registry);
                view.job_callback_scope().each(*callback);
            },
            [](Registry& registry) {
                auto view = make_view(registry);
                return view.matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = make_view(registry);
                    view.job_callback_scope().each_index_range(*callback, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<IterComponents...>(),
            threading_,
            false);
    }

    template <typename... Tags>
    auto with_tags() const
        -> JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type,
            detail::type_list<WithoutTags...>> {
        using NextWith =
            typename detail::type_list_concat<detail::type_list<WithTags...>, detail::type_list<Tags...>>::type;
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            NextWith,
            detail::type_list<WithoutTags...>>(*registry_, order_, threading_, name_);
    }

    template <typename... Tags>
    auto without_tags() const
        -> JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type> {
        using NextWithout =
            typename detail::type_list_concat<detail::type_list<WithoutTags...>, detail::type_list<Tags...>>::type;
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            NextWithout>(*registry_, order_, threading_, name_);
    }

    template <typename... MoreAccessComponents>
    auto access_other_entities() const
        -> JobTagFilteredView<
            detail::type_list<IterComponents...>,
            typename detail::type_list_concat<
                detail::type_list<AccessComponents...>,
                detail::type_list<MoreAccessComponents...>>::type,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        if (!threading_.single_thread || threading_.max_threads > 1) {
            throw std::logic_error("ashiato access_other_entities jobs must be single threaded");
        }
        using NextAccess = typename detail::type_list_concat<
            detail::type_list<AccessComponents...>,
            detail::type_list<MoreAccessComponents...>>::type;
        JobThreadingOptions threading = threading_;
        threading.single_thread = true;
        threading.max_threads = 1;
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            NextAccess,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>>(*registry_, order_, threading, name_);
    }

    template <typename... MoreOptionalComponents>
    auto optional() const
        -> JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            typename detail::type_list_concat<
                detail::type_list<OptionalComponents...>,
                detail::type_list<MoreOptionalComponents...>>::type,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>> {
        using NextOptional = typename detail::type_list_concat<
            detail::type_list<OptionalComponents...>,
            detail::type_list<MoreOptionalComponents...>>::type;
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            NextOptional,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>>(*registry_, order_, threading_, name_);
    }

    template <typename... StructuralComponents>
    JobTagFilteredStructuralView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<WithTags...>,
        detail::type_list<WithoutTags...>,
        detail::type_list<StructuralComponents...>>
    structural() const {
        static_assert(
            detail::unique_components<StructuralComponents...>::value,
            "ashiato structural components cannot repeat types");
        return JobTagFilteredStructuralView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<WithTags...>,
            detail::type_list<WithoutTags...>,
            detail::type_list<StructuralComponents...>>(*registry_, order_, threading_, name_);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        auto view = make_view(*registry_);
        return view.template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        auto view = make_view(*registry_);
        return view.template get<T>();
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        auto view = make_view(*registry_);
        return view.template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
                detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        auto view = make_view(*registry_);
        return view.template write<T>();
    }

private:
    static auto make_view(Registry& registry) {
        if constexpr (sizeof...(AccessComponents) == 0 && sizeof...(OptionalComponents) == 0) {
            return registry.template view<IterComponents...>()
                .template with_tags<WithTags...>()
                .template without_tags<WithoutTags...>();
        } else if constexpr (sizeof...(AccessComponents) == 0) {
            return registry.template view<IterComponents...>()
                .template optional<OptionalComponents...>()
                .template with_tags<WithTags...>()
                .template without_tags<WithoutTags...>();
        } else {
            return registry.template view<IterComponents...>()
                .template access<AccessComponents...>()
                .template optional<OptionalComponents...>()
                .template with_tags<WithTags...>()
                .template without_tags<WithoutTags...>();
        }
    }

    Registry* registry_;
    int order_ = 0;
    JobThreadingOptions threading_;
    std::string name_;
};

template <
    typename... IterComponents,
    typename... AccessComponents,
    typename... WithTags,
    typename... WithoutTags,
    typename... StructuralComponents>
class Registry::JobTagFilteredStructuralView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<>,
    detail::type_list<WithTags...>,
    detail::type_list<WithoutTags...>,
    detail::type_list<StructuralComponents...>> {
public:
    JobTagFilteredStructuralView(Registry& registry, int order, JobThreadingOptions threading, std::string name = {})
        : registry_(&registry), order_(order), threading_(threading), name_(std::move(name)) {}

    JobTagFilteredStructuralView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        JobAccessMetadata metadata = registry_->template make_job_access_metadata<IterComponents..., AccessComponents...>();
        registry_->template append_job_filter_metadata<WithTags..., WithoutTags...>(metadata);
        registry_->template append_job_structural_metadata<StructuralComponents...>(metadata);
        threading_.single_thread = true;
        threading_.max_threads = 1;
        return registry_->add_job(
            order_,
            name_,
            std::move(metadata),
            [callback](Registry& registry) mutable {
                auto view = make_view(registry);
                auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                    using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                    using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                    Context context(registry, active_view);
                    detail::invoke_job_context_callback(*callback, context, entity, components...);
                };
                view.each(adapter);
            },
            [](Registry& registry) {
                auto view = make_view(registry);
                return view.matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = make_view(registry);
                    auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                        using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                        using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                        Context context(registry, active_view);
                        detail::invoke_job_context_callback(*callback, context, entity, components...);
                    };
                    view.each_index_range(adapter, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<IterComponents...>(),
            threading_,
            true);
    }

private:
    static auto make_view(Registry& registry) {
        if constexpr (sizeof...(AccessComponents) == 0) {
            return registry.template view<IterComponents...>()
                .template with_tags<WithTags...>()
                .template without_tags<WithoutTags...>();
        } else {
            return registry.template view<IterComponents...>()
                .template access<AccessComponents...>()
                .template with_tags<WithTags...>()
                .template without_tags<WithoutTags...>();
        }
    }

    Registry* registry_;
    int order_ = 0;
    JobThreadingOptions threading_;
    std::string name_;
};

template <typename... Components>
class Registry::JobView {
    static_assert(sizeof...(Components) > 0, "ashiato jobs require at least one component");

public:
    JobView(Registry& registry, int order)
        : registry_(&registry), order_(order), view_(registry) {}

    JobView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    JobView& max_threads(std::size_t count) {
        threading_.max_threads = std::max<std::size_t>(count, 1);
        threading_.single_thread = false;
        return *this;
    }

    JobView& single_thread() {
        threading_.max_threads = 1;
        threading_.single_thread = true;
        return *this;
    }

    JobView& min_entities_per_thread(std::size_t count) {
        threading_.min_entities_per_thread = std::max<std::size_t>(count, 1);
        return *this;
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        return registry_->add_job(
            order_,
            name_,
            registry_->template make_job_access_metadata<Components...>(),
            [callback](Registry& registry) mutable {
                auto view = registry.template view<Components...>();
                view.job_callback_scope().each(*callback);
            },
            [](Registry& registry) {
                return registry.template view<Components...>().matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = registry.template view<Components...>();
                    view.job_callback_scope().each_index_range(*callback, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<Components...>(),
            threading_,
            false);
    }

    template <typename... AccessComponents>
    JobAccessView<detail::type_list<Components...>, detail::type_list<AccessComponents...>, detail::type_list<>>
    access_other_entities() const {
        if (!threading_.single_thread || threading_.max_threads > 1) {
            throw std::logic_error("ashiato access_other_entities jobs must be single threaded");
        }
        JobThreadingOptions threading = threading_;
        threading.single_thread = true;
        threading.max_threads = 1;
        return JobAccessView<
            detail::type_list<Components...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<>>(
            *registry_,
            order_,
            view_.template access<AccessComponents...>(),
            threading,
            name_);
    }

    template <typename... OptionalComponents>
    JobAccessView<detail::type_list<Components...>, detail::type_list<>, detail::type_list<OptionalComponents...>>
    optional() const {
        return JobAccessView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<OptionalComponents...>>(
            *registry_,
            order_,
            view_.template optional<OptionalComponents...>(),
            threading_,
            name_);
    }

    template <typename... Tags>
    JobTagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const {
        return JobTagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<Tags...>,
            detail::type_list<>>(*registry_, order_, threading_, name_);
    }

    template <typename... Tags>
    JobTagFilteredView<
        detail::type_list<Components...>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const {
        return JobTagFilteredView<
            detail::type_list<Components...>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<>,
            detail::type_list<Tags...>>(*registry_, order_, threading_, name_);
    }

    template <typename... StructuralComponents>
    JobStructuralView<detail::type_list<Components...>, detail::type_list<StructuralComponents...>> structural() const {
        static_assert(
            detail::unique_components<StructuralComponents...>::value,
            "ashiato structural components cannot repeat types");
        return JobStructuralView<detail::type_list<Components...>, detail::type_list<StructuralComponents...>>(
            *registry_,
            order_,
            threading_,
            name_);
    }

    template <typename T, typename std::enable_if<detail::contains_component<T, Components...>::value, int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        return view_.template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value && detail::contains_component<T, Components...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        return view_.template get<T>();
    }

    template <typename T, typename std::enable_if<
                              !std::is_const<typename std::remove_reference<T>::type>::value &&
                                  detail::contains_mutable_component<T, Components...>::value,
                              int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        return view_.template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, Components...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        return view_.template write<T>();
    }

private:
    Registry* registry_;
    int order_ = 0;
    View<Components...> view_;
    JobThreadingOptions threading_;
    std::string name_;
};

template <typename... IterComponents, typename... AccessComponents, typename... OptionalComponents>
class Registry::JobAccessView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<OptionalComponents...>> {
public:
    using ActiveView = AccessView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>>;

    JobAccessView(
        Registry& registry,
        int order,
        ActiveView view,
        JobThreadingOptions threading,
        std::string name = {})
        : registry_(&registry), order_(order), view_(std::move(view)), threading_(threading), name_(std::move(name)) {}

    JobAccessView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    JobAccessView& max_threads(std::size_t count) {
        static_assert(sizeof...(AccessComponents) == 0, "ashiato access_other_entities jobs must be single threaded");
        threading_.max_threads = std::max<std::size_t>(count, 1);
        threading_.single_thread = false;
        return *this;
    }

    JobAccessView& single_thread() {
        threading_.max_threads = 1;
        threading_.single_thread = true;
        return *this;
    }

    JobAccessView& min_entities_per_thread(std::size_t count) {
        threading_.min_entities_per_thread = std::max<std::size_t>(count, 1);
        return *this;
    }

    template <typename... MoreAccessComponents>
    auto access_other_entities() const
        -> JobAccessView<
            detail::type_list<IterComponents...>,
            typename detail::type_list_concat<
                detail::type_list<AccessComponents...>,
                detail::type_list<MoreAccessComponents...>>::type,
            detail::type_list<OptionalComponents...>> {
        if (!threading_.single_thread || threading_.max_threads > 1) {
            throw std::logic_error("ashiato access_other_entities jobs must be single threaded");
        }
        using NextAccess = typename detail::type_list_concat<
            detail::type_list<AccessComponents...>,
            detail::type_list<MoreAccessComponents...>>::type;
        JobThreadingOptions threading = threading_;
        threading.single_thread = true;
        threading.max_threads = 1;
        return JobAccessView<
            detail::type_list<IterComponents...>,
            NextAccess,
            detail::type_list<OptionalComponents...>>(
            *registry_,
            order_,
            view_.template access<MoreAccessComponents...>(),
            threading,
            name_);
    }

    template <typename... MoreOptionalComponents>
    auto optional() const
        -> JobAccessView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            typename detail::type_list_concat<
                detail::type_list<OptionalComponents...>,
                detail::type_list<MoreOptionalComponents...>>::type> {
        using NextOptional = typename detail::type_list_concat<
            detail::type_list<OptionalComponents...>,
            detail::type_list<MoreOptionalComponents...>>::type;
        return JobAccessView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            NextOptional>(
            *registry_,
            order_,
            view_.template optional<MoreOptionalComponents...>(),
            threading_,
            name_);
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        return registry_->add_job(
            order_,
            name_,
            registry_->template make_job_access_metadata<IterComponents..., AccessComponents..., OptionalComponents...>(),
            [callback](Registry& registry) mutable {
                auto view = make_view(registry);
                view.job_callback_scope().each(*callback);
            },
            [](Registry& registry) {
                auto view = make_view(registry);
                return view.matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = make_view(registry);
                    view.job_callback_scope().each_index_range(*callback, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<IterComponents...>(),
            threading_,
            false);
    }

    template <typename... StructuralComponents>
    JobStructuralAccessView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<StructuralComponents...>>
    structural() const {
        static_assert(
            detail::unique_components<StructuralComponents...>::value,
            "ashiato structural components cannot repeat types");
        return JobStructuralAccessView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<StructuralComponents...>>(*registry_, order_, threading_, name_);
    }

    template <typename... Tags>
    JobTagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<Tags...>,
        detail::type_list<>>
    with_tags() const {
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<Tags...>,
            detail::type_list<>>(*registry_, order_, threading_, name_);
    }

    template <typename... Tags>
    JobTagFilteredView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>,
        detail::type_list<>,
        detail::type_list<Tags...>>
    without_tags() const {
        return JobTagFilteredView<
            detail::type_list<IterComponents...>,
            detail::type_list<AccessComponents...>,
            detail::type_list<OptionalComponents...>,
            detail::type_list<>,
            detail::type_list<Tags...>>(*registry_, order_, threading_, name_);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::contains_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get(Entity entity) const {
        return view_.template get<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                detail::contains_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    const detail::component_query_t<T>& get() const {
        return view_.template get<T>();
    }

    template <
        typename T,
        typename std::enable_if<
            !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write(Entity entity) {
        return view_.template write<T>(entity);
    }

    template <
        typename T,
        typename std::enable_if<
            detail::is_singleton_query<T>::value &&
                !std::is_const<typename std::remove_reference<T>::type>::value &&
                detail::contains_mutable_component<T, IterComponents..., AccessComponents..., OptionalComponents...>::value,
            int>::type = 0>
    detail::component_query_t<T>& write() {
        return view_.template write<T>();
    }

private:
    static auto make_view(Registry& registry) {
        if constexpr (sizeof...(AccessComponents) == 0 && sizeof...(OptionalComponents) == 0) {
            return registry.template view<IterComponents...>();
        } else if constexpr (sizeof...(AccessComponents) == 0) {
            return registry.template view<IterComponents...>().template optional<OptionalComponents...>();
        } else if constexpr (sizeof...(OptionalComponents) == 0) {
            return registry.template view<IterComponents...>().template access<AccessComponents...>();
        } else {
            return registry.template view<IterComponents...>()
                .template access<AccessComponents...>()
                .template optional<OptionalComponents...>();
        }
    }

    Registry* registry_;
    int order_ = 0;
    AccessView<
        detail::type_list<IterComponents...>,
        detail::type_list<AccessComponents...>,
        detail::type_list<OptionalComponents...>> view_;
    JobThreadingOptions threading_;
    std::string name_;
};

template <typename... IterComponents, typename... StructuralComponents>
class Registry::JobStructuralView<detail::type_list<IterComponents...>, detail::type_list<StructuralComponents...>> {
public:
    JobStructuralView(Registry& registry, int order, JobThreadingOptions threading, std::string name = {})
        : registry_(&registry), order_(order), threading_(threading), name_(std::move(name)) {}

    JobStructuralView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        JobAccessMetadata metadata = registry_->template make_job_access_metadata<IterComponents...>();
        registry_->template append_job_structural_metadata<StructuralComponents...>(metadata);
        threading_.single_thread = true;
        threading_.max_threads = 1;
        return registry_->add_job(
            order_,
            name_,
            std::move(metadata),
            [callback](Registry& registry) mutable {
                auto view = registry.template view<IterComponents...>();
                auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                    using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                    using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                    Context context(registry, active_view);
                    detail::invoke_job_context_callback(*callback, context, entity, components...);
                };
                view.each(adapter);
            },
            [](Registry& registry) {
                return registry.template view<IterComponents...>().matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = registry.template view<IterComponents...>();
                    auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                        using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                        using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                        Context context(registry, active_view);
                        detail::invoke_job_context_callback(*callback, context, entity, components...);
                    };
                    view.each_index_range(adapter, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<IterComponents...>(),
            threading_,
            true);
    }

private:
    Registry* registry_;
    int order_ = 0;
    JobThreadingOptions threading_;
    std::string name_;
};

template <typename... IterComponents, typename... AccessComponents, typename... OptionalComponents, typename... StructuralComponents>
class Registry::JobStructuralAccessView<
    detail::type_list<IterComponents...>,
    detail::type_list<AccessComponents...>,
    detail::type_list<OptionalComponents...>,
    detail::type_list<StructuralComponents...>> {
public:
    JobStructuralAccessView(Registry& registry, int order, JobThreadingOptions threading, std::string name = {})
        : registry_(&registry), order_(order), threading_(threading), name_(std::move(name)) {}

    JobStructuralAccessView& name(std::string value) {
        name_ = std::move(value);
        return *this;
    }

    template <typename Fn>
    Entity each(Fn&& fn) {
        using Callback = typename std::decay<Fn>::type;
        auto callback = std::make_shared<Callback>(std::forward<Fn>(fn));
        JobAccessMetadata metadata =
            registry_->template make_job_access_metadata<IterComponents..., AccessComponents..., OptionalComponents...>();
        registry_->template append_job_structural_metadata<StructuralComponents...>(metadata);
        threading_.single_thread = true;
        threading_.max_threads = 1;
        return registry_->add_job(
            order_,
            name_,
            std::move(metadata),
            [callback](Registry& registry) mutable {
                auto view = registry.template view<IterComponents...>().template access<AccessComponents...>();
                auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                    using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                    using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                    Context context(registry, active_view);
                    detail::invoke_job_context_callback(*callback, context, entity, components...);
                };
                view.each(adapter);
            },
            [](Registry& registry) {
                return registry.template view<IterComponents...>()
                    .template access<AccessComponents...>()
                    .matching_indices();
            },
            [callback](Registry& registry, const std::vector<std::uint32_t>& indices, std::size_t begin, std::size_t end)
                mutable {
                    auto view = registry.template view<IterComponents...>().template access<AccessComponents...>();
                    auto adapter = [callback, &registry](auto& active_view, Entity entity, auto&... components) mutable {
                        using ActiveView = typename std::remove_reference<decltype(active_view)>::type;
                        using Context = JobStructuralContext<ActiveView, StructuralComponents...>;
                        Context context(registry, active_view);
                        detail::invoke_job_context_callback(*callback, context, entity, components...);
                    };
                    view.each_index_range(adapter, indices, begin, end);
                },
            registry_->template make_job_range_dirty_writes<IterComponents...>(),
            threading_,
            true);
    }

private:
    Registry* registry_;
    int order_ = 0;
    JobThreadingOptions threading_;
    std::string name_;
};

template <typename... Components>
typename std::enable_if<!detail::contains_tag_query<Components...>::value, Registry::View<Components...>>::type Registry::view() {
    require_runtime_registry_access_allowed("view");
    return View<Components...>(*this);
}

template <typename... Components>
Registry::JobView<Components...> Registry::job(int order) {
    require_runtime_registry_access_allowed("job");
    return JobView<Components...>(*this, order);
}

class Orchestrator {
public:
    explicit Orchestrator(const Registry& registry);

    JobSchedule schedule() const;
    JobSchedule schedule_for_jobs(const std::vector<Entity>& jobs) const;

private:
    JobSchedule build_schedule(const std::vector<std::size_t>& ordered_indices) const;
    static void apply_read_dependencies(
        const std::vector<std::uint32_t>& reads,
        const std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
        std::size_t& stage_index);
    static void apply_write_dependencies(
        const std::vector<std::uint32_t>& writes,
        const std::unordered_map<std::uint32_t, std::size_t>& last_reader_stage,
        const std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
        std::size_t& stage_index);
    static void record_read_stage(
        const std::vector<std::uint32_t>& reads,
        std::unordered_map<std::uint32_t, std::size_t>& last_reader_stage,
        std::size_t stage_index);
    static void record_write_stage(
        const std::vector<std::uint32_t>& writes,
        std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
        std::size_t stage_index);

    const Registry* registry_;
};

template <typename... Owned>
void Registry::declare_owned_group() {
    require_runtime_registry_access_allowed("declare_owned_group");
    static_assert(sizeof...(Owned) > 0, "ashiato owned groups require at least one component");
    static_assert(detail::unique_components<Owned...>::value, "ashiato owned groups cannot repeat component types");
    static_assert((!detail::is_singleton_query<Owned>::value && ...), "ashiato owned groups cannot own singleton components");
    static_assert(
        (!std::is_const<typename std::remove_reference<Owned>::type>::value && ...),
        "ashiato owned groups cannot own const component types");

    (void)std::initializer_list<int>{
        (static_cast<void>(storage_for(registered_component<detail::component_query_t<Owned>>())), 0)...};
    std::vector<std::uint32_t> key = make_group_key<Owned...>(*this);
    (void)group_for_key(key);
}

}  // namespace ashiato
