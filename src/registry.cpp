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
        if (slot.storage != nullptr && !slot.singleton) {
            slot.storage->erase(entity, trace_commit_context_);
        }
    }

    current_versions_[index] = static_cast<EntityVersion>((current_versions_[index] + 1) & entity_version_mask);
    free_entities_.push_back(make_entity(index, current_versions_[index]));
    return true;
}

void Registry::clear() {
    require_no_readers();

    for (auto& slot : components_) {
        delete slot.storage;
        slot.storage = nullptr;
    }

    current_versions_.clear();
    entity_dense_indices_.clear();
    alive_entities_.clear();
    free_entities_.clear();
    active_transactions_.clear();
    active_readers_ = 0;
    classic_access_.clear();
    snapshot_classic_readers_ = 0;
    recompute_classic_mode_flag();
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

    if (components_[component].singleton) {
        return nullptr;
    }

    const RawPagedSparseArray* slot = components_[component].storage;
    return slot != nullptr ? slot->try_get_raw(entity) : nullptr;
}

const void* Registry::try_get_singleton(ComponentId component) const {
    if (component >= components_.size() || !components_[component].singleton) {
        return nullptr;
    }

    const RawPagedSparseArray* slot = components_[component].storage;
    return slot != nullptr ? slot->try_get_raw(detail::singleton_entity) : nullptr;
}

bool Registry::remove(Entity entity, ComponentId component) {
    require_no_readers();

    if (component >= components_.size() || components_[component].singleton) {
        return false;
    }

    RawPagedSparseArray* slot = components_[component].storage;
    return slot != nullptr && slot->erase(entity, trace_commit_context_);
}

RawPagedSparseArray* Registry::storage(ComponentId component) {
    if (component >= components_.size()) {
        return nullptr;
    }
    return components_[component].storage;
}

const RawPagedSparseArray* Registry::storage(ComponentId component) const {
    if (component >= components_.size()) {
        return nullptr;
    }
    return components_[component].storage;
}

RawPagedSparseArray& Registry::assure_storage(
    ComponentId componentId,
    std::size_t component_size,
    std::size_t component_alignment) {

    ensure_component_slot(componentId);

    ComponentSlot& slot = components_[componentId];
    if (!slot.mode_configured) {
        slot.mode = resolved_storage_mode(componentId);
        slot.mode_configured = true;
        slot.singleton = false;
        recompute_classic_mode_flag();
    }

    RawPagedSparseArray* component = slot.storage;
    if (component == nullptr) {
        if (slot.mode == ComponentStorageMode::trace_preallocate && !trace_max_history_configured_) {
            throw std::logic_error("trace_preallocate storage requires set_trace_max_history before storage creation");
        }
        component = new RawPagedSparseArray(
            component_size,
            component_alignment,
            slot.mode,
            page_size_,
            trace_max_history_);
        slot.storage = component;
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

void Registry::ensure_component_slot(ComponentId componentId) {
    if (componentId >= components_.size()) {
        components_.resize(static_cast<std::size_t>(componentId) + 1);
    }
}

ComponentStorageMode Registry::resolved_storage_mode(ComponentId componentId) const {
    if (componentId >= components_.size() || !components_[componentId].mode_configured) {
        return ComponentStorageMode::mvcc;
    }
    return components_[componentId].mode;
}

void Registry::recompute_classic_mode_flag() {
    has_classic_storage_mode_ = false;
    for (const ComponentSlot& slot : components_) {
        if (slot.mode_configured && is_direct_write_storage_mode(slot.mode)) {
            has_classic_storage_mode_ = true;
            return;
        }
    }
}

void Registry::compact_trace_history() {
    for (ComponentSlot& slot : components_) {
        if (slot.storage == nullptr || !is_trace_storage_mode(slot.mode)) {
            continue;
        }
        if (slot.mode == ComponentStorageMode::trace_ondemand) {
            slot.storage->compact_trace_history(trace_commit_context_.timestamp, trace_max_history_);
        } else {
            slot.storage->compact_preallocated_trace_history(trace_commit_context_.timestamp, trace_max_history_);
        }
    }
}

void Registry::capture_preallocated_trace_frames() {
    for (ComponentSlot& slot : components_) {
        if (slot.storage == nullptr || slot.mode != ComponentStorageMode::trace_preallocate) {
            continue;
        }
        slot.storage->capture_preallocated_trace_frame(trace_commit_context_.timestamp);
    }
}

Timestamp Registry::acquire_tsn() {
    if (next_tsn_ == std::numeric_limits<Timestamp>::max()) {
        throw std::overflow_error("transaction timestamp space exhausted");
    }
    return next_tsn_++;
}

std::vector<Timestamp> Registry::active_transactions_snapshot() const {
    return active_transactions_;
}

void Registry::register_transaction(Timestamp tsn) {
    active_transactions_.push_back(tsn);
}

void Registry::unregister_transaction(Timestamp tsn) {
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

void Registry::register_snapshot_classic_access() {
    if (!has_classic_storage_mode_) {
        return;
    }

    if (classic_access_.size() < components_.size()) {
        classic_access_.resize(components_.size());
    }

    for (std::size_t i = 0; i < components_.size(); ++i) {
        const ComponentSlot& slot = components_[i];
        if (!slot.mode_configured || !is_direct_write_storage_mode(slot.mode)) {
            continue;
        }

        ClassicAccessState& state = classic_access_[i];
        if (state.writers != 0) {
            throw std::logic_error("direct-write component storage mode does not allow snapshots while a writer is active");
        }
    }

    for (std::size_t i = 0; i < components_.size(); ++i) {
        const ComponentSlot& slot = components_[i];
        if (!slot.mode_configured || !is_direct_write_storage_mode(slot.mode)) {
            continue;
        }
        ++classic_access_[i].readers;
    }

    ++snapshot_classic_readers_;
}

void Registry::unregister_snapshot_classic_access() {
    if (snapshot_classic_readers_ == 0) {
        return;
    }

    for (std::size_t i = 0; i < components_.size() && i < classic_access_.size(); ++i) {
        const ComponentSlot& slot = components_[i];
        if (!slot.mode_configured || !is_direct_write_storage_mode(slot.mode)) {
            continue;
        }
        if (classic_access_[i].readers != 0) {
            --classic_access_[i].readers;
        }
    }

    --snapshot_classic_readers_;
}

void Registry::unregister_classic_access(const std::vector<ClassicAccessRegistration>& registrations) {
    for (const ClassicAccessRegistration& registration : registrations) {
        if (registration.component >= classic_access_.size()) {
            continue;
        }

        ClassicAccessState& state = classic_access_[registration.component];
        if (registration.writer) {
            if (state.writers != 0) {
                --state.writers;
            }
        } else if (state.readers != 0) {
            --state.readers;
        }
    }
}

}  // namespace ecs
