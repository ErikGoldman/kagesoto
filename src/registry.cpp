#include "registry.hpp"

#include <algorithm>

namespace ecs {

namespace detail {

ComponentId next_component_id() {
    static ComponentId next_id = 0;
    if (next_id == null_component) {
        throw std::overflow_error("component id space exhausted");
    }
    return next_id++;
}

}  // namespace detail

Registry::Registry(std::size_t page_size)
    : page_size_(page_size) {
    if (page_size_ == 0) {
        throw std::invalid_argument("page size must be greater than zero");
    }
}

Registry::~Registry() {
    clear();
}

Registry::Registry(Registry&& other) noexcept = default;

Registry& Registry::operator=(Registry&& other) noexcept = default;

Entity Registry::create() {
    require_no_readers();

    Entity entity = null_entity;
    if (!free_entities_.empty()) {
        entity = free_entities_.back();
        free_entities_.pop_back();
    } else {
        if (next_entity_index_ >= entity_index_mask) {
            throw std::overflow_error("entity index space exhausted");
        }

        const Entity index = next_entity_index_++;
        ensure_entity_slot(index);
        entity = make_entity(index, current_versions_[index]);
    }

    const Entity index = entity_index(entity);
    ensure_entity_slot(index);
    alive_entities_.push_back(entity);
    entity_dense_indices_[index] = alive_entities_.size() - 1;
    return entity;
}

bool Registry::destroy(Entity entity) {
    require_no_readers();

    if (!alive(entity)) {
        return false;
    }

    const Entity index = entity_index(entity);
    const std::size_t dense_index = entity_dense_indices_[index];
    const std::size_t last_index = alive_entities_.size() - 1;

    if (dense_index != last_index) {
        const Entity moved = alive_entities_.back();
        alive_entities_[dense_index] = moved;
        entity_dense_indices_[entity_index(moved)] = dense_index;
    }

    alive_entities_.pop_back();
    entity_dense_indices_[index] = npos;

    for (auto& slot : components_) {
        if (slot != nullptr) {
            slot->erase(entity);
        }
    }

    current_versions_[index] = static_cast<EntityVersion>((current_versions_[index] + 1) & entity_version_mask);
    free_entities_.push_back(make_entity(index, current_versions_[index]));
    return true;
}

void Registry::clear() {
    require_no_readers();

    for (auto& slot : components_) {
        delete slot;
        slot = nullptr;
    }

    components_.clear();
    current_versions_.clear();
    entity_dense_indices_.clear();
    alive_entities_.clear();
    free_entities_.clear();
    active_transactions_.clear();
    active_readers_ = 0;
    next_entity_index_ = 0;
    next_tsn_ = 1;
}

bool Registry::alive(Entity entity) const {
    const Entity index = entity_index(entity);
    return index < current_versions_.size() &&
           entity_version(entity) == current_versions_[index] &&
           entity_dense_indices_[index] != npos;
}

std::size_t Registry::page_size() const {
    return page_size_;
}

std::size_t Registry::entity_count() const {
    return alive_entities_.size();
}

const std::vector<Entity>& Registry::entities() const {
    return alive_entities_;
}

bool Registry::has(Entity entity, ComponentId component) const {
    return try_get(entity, component) != nullptr;
}

const void* Registry::try_get(Entity entity, ComponentId component) const {
    if (!alive(entity) || component >= components_.size()) {
        return nullptr;
    }

    const RawPagedSparseArray* slot = components_[component];
    return slot != nullptr ? slot->try_get_raw(entity) : nullptr;
}

bool Registry::remove(Entity entity, ComponentId component) {
    require_no_readers();

    if (component >= components_.size()) {
        return false;
    }

    RawPagedSparseArray* slot = components_[component];
    return slot != nullptr && slot->erase(entity);
}

RawPagedSparseArray* Registry::storage(ComponentId component) {
    if (component >= components_.size()) {
        return nullptr;
    }
    return components_[component];
}

const RawPagedSparseArray* Registry::storage(ComponentId component) const {
    if (component >= components_.size()) {
        return nullptr;
    }
    return components_[component];
}

RawPagedSparseArray& Registry::assure_storage(
    ComponentId componentId,
    std::size_t component_size,
    std::size_t component_alignment) {

    if (componentId >= components_.size()) {
        components_.resize(static_cast<std::size_t>(componentId) + 1);
    }

    RawPagedSparseArray* component = components_[componentId];
    if (component == nullptr) {
        component = new RawPagedSparseArray(component_size, component_alignment, page_size_);
        components_[componentId] = component;
    }
    return *component;
}

void Registry::require_no_readers() const {
    if (active_readers_ != 0) {
        throw std::logic_error("cannot mutate registry structure while snapshots or transactions are open");
    }
}

void Registry::require_alive(Entity entity) const {
    if (!alive(entity)) {
        throw std::out_of_range("entity is not alive");
    }
}

void Registry::ensure_entity_slot(Entity index) {
    const std::size_t required = static_cast<std::size_t>(index) + 1;
    if (required > current_versions_.size()) {
        current_versions_.resize(required, 0);
        entity_dense_indices_.resize(required, npos);
    }
}

std::uint64_t Registry::acquire_tsn() {
    return next_tsn_++;
}

std::vector<std::uint64_t> Registry::active_transactions_snapshot() const {
    return active_transactions_;
}

void Registry::register_transaction(std::uint64_t tsn) {
    active_transactions_.push_back(tsn);
}

void Registry::unregister_transaction(std::uint64_t tsn) {
    const auto it = std::find(active_transactions_.begin(), active_transactions_.end(), tsn);
    if (it != active_transactions_.end()) {
        active_transactions_.erase(it);
    }
}

void Registry::register_reader() {
    ++active_readers_;
}

void Registry::unregister_reader() {
    if (active_readers_ != 0) {
        --active_readers_;
    }
}

}  // namespace ecs
