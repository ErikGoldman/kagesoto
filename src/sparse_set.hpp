#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "ecs/export.hpp"

namespace ecs {

using Entity = std::uint64_t;
using EntityVersion = std::uint8_t;

inline constexpr Entity null_entity = std::numeric_limits<Entity>::max();
inline constexpr std::size_t entity_index_bits = 59;
inline constexpr std::size_t entity_version_bits = 64 - entity_index_bits;
inline constexpr std::size_t entity_version_shift = entity_index_bits;
inline constexpr Entity entity_index_mask = (Entity{1} << entity_index_bits) - 1;
inline constexpr Entity entity_version_mask = (Entity{1} << entity_version_bits) - 1;

constexpr Entity entity_index(Entity entity) {
    return entity & entity_index_mask;
}

constexpr EntityVersion entity_version(Entity entity) {
    return static_cast<EntityVersion>((entity >> entity_version_shift) & entity_version_mask);
}

constexpr Entity make_entity(Entity index, EntityVersion version) {
    return (static_cast<Entity>(version & entity_version_mask) << entity_version_shift) |
           (index & entity_index_mask);
}

class ECS_API PagedSparseSet {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    explicit PagedSparseSet(std::size_t page_size = 1024);
    ~PagedSparseSet();

    PagedSparseSet(PagedSparseSet&& other) noexcept;
    PagedSparseSet& operator=(PagedSparseSet&& other) noexcept;

    PagedSparseSet(const PagedSparseSet&) = delete;
    PagedSparseSet& operator=(const PagedSparseSet&) = delete;

    bool contains(Entity entity) const;
    bool insert(Entity entity);
    bool erase(Entity entity);
    void clear();

    std::size_t sparse_index(Entity entity) const;
    void set_index(Entity entity, std::size_t value);
    void clear_index(Entity entity);
    std::size_t index_of(Entity entity) const;
    std::size_t page_size() const;
    std::size_t size() const;
    bool empty() const;

    const std::vector<Entity>& entities() const;

private:
    using Page = std::unique_ptr<std::size_t[]>;

    std::size_t page_index(Entity entity) const;
    std::size_t page_offset(Entity entity) const;
    const std::size_t* try_page(Entity entity) const;
    Page& ensure_page(Entity entity);
    void set_sparse_index(Entity entity, std::size_t value);
    std::size_t find_slot(Entity entity) const;
    std::size_t find_index(Entity entity) const;

    std::size_t page_size_;
    std::size_t page_shift_;
    Entity page_mask_;
    std::vector<Page> sparse_pages_;
    std::vector<Entity> dense_entities_;
};

}  // namespace ecs
