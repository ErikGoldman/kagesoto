#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

    bool has(Entity entity, ComponentId component) const;
    void* try_get(Entity entity, ComponentId component);
    const void* try_get(Entity entity, ComponentId component) const;
    bool remove(Entity entity, ComponentId component);
    RawPagedSparseArray* storage(ComponentId component);
    const RawPagedSparseArray* storage(ComponentId component) const;

    template <typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args) {
        static_assert(std::is_trivially_copyable_v<T>, "Registry components must be trivially copyable");

        require_alive(entity);
        T value{std::forward<Args>(args)...};
        return *static_cast<T*>(assure_storage(component_id<T>(), sizeof(T), alignof(T)).emplace_copy(entity, &value));
    }

    template <typename T>
    bool has(Entity entity) const {
        return has(entity, component_id<T>());
    }

    template <typename T>
    T* try_get(Entity entity) {
        return static_cast<T*>(try_get(entity, component_id<T>()));
    }

    template <typename T>
    const T* try_get(Entity entity) const {
        return static_cast<const T*>(try_get(entity, component_id<T>()));
    }

    template <typename T>
    T& get(Entity entity) {
        T* component = try_get<T>(entity);
        if (component == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return *component;
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
    bool remove(Entity entity) {
        return remove(entity, component_id<T>());
    }

    template <typename T>
    PagedSparseArrayView<T> storage() {
        return PagedSparseArrayView<T>(storage(component_id<T>()));
    }

    template <typename T>
    ConstPagedSparseArrayView<T> storage() const {
        return ConstPagedSparseArrayView<T>(storage(component_id<T>()));
    }

private:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max() - 1;

    void require_alive(Entity entity) const;
    void ensure_entity_slot(Entity index);
    RawPagedSparseArray& assure_storage(ComponentId componentId, std::size_t component_size, std::size_t component_alignment);

    std::size_t page_size_;
    Entity next_entity_index_ = 0;
    std::vector<EntityVersion> current_versions_;
    std::vector<std::size_t> entity_dense_indices_;
    std::vector<Entity> alive_entities_;
    std::vector<Entity> free_entities_;
    std::vector<RawPagedSparseArray*> components_;
};

}  // namespace ecs
