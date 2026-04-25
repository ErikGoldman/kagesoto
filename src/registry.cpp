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

    std::vector<OwningGroupBase*> erased_groups;
    for (auto& slot : components_) {
        if (slot.owner != nullptr &&
            std::find(erased_groups.begin(), erased_groups.end(), slot.owner) == erased_groups.end()) {
            if (slot.owner->erase_entity(*this, entity, trace_commit_context_)) {
                erased_groups.push_back(slot.owner);
            }
        }
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
        slot.owner = nullptr;
    }
    groups_.clear();

    current_versions_.clear();
    entity_dense_indices_.clear();
    alive_entities_.clear();
    free_entities_.clear();
    active_readers_ = 0;
    direct_access_.clear();
    snapshot_direct_readers_ = 0;
    next_entity_index_ = 0;
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

void Registry::set_tracing_enabled(bool enabled) {
    require_no_readers();
    if (trace_commit_context_.enabled == enabled) {
        return;
    }

    trace_commit_context_.enabled = enabled;
    if (enabled) {
        initialize_trace_history();
        compact_trace_history();
    }
}

bool Registry::has(Entity entity, ComponentId component) const {
    return try_get(entity, component) != nullptr;
}

const void* Registry::try_get(Entity entity, ComponentId component) const {
    if (!alive(entity) || component >= components_.size()) {
        return nullptr;
    }

    if (components_[component].singleton) {
        return try_get_singleton(component);
    }

    if (const OwningGroupBase* owner = components_[component].owner; owner != nullptr && owner->contains(entity)) {
        return owner->try_get(component, entity);
    }

    return try_get_standalone(entity, component);
}

const void* Registry::try_get_standalone(Entity entity, ComponentId component) const {
    if (!alive(entity) || component >= components_.size()) {
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

    if (OwningGroupBase* owner = components_[component].owner; owner != nullptr && owner->contains(entity)) {
        return owner->remove_component(*this, entity, component, trace_commit_context_);
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

RawPagedSparseArray* Registry::storage_for_entity(Entity entity, ComponentId component) {
    if (component >= components_.size()) {
        return nullptr;
    }
    if (OwningGroupBase* owner = components_[component].owner; owner != nullptr && owner->contains(entity)) {
        return owner->storage(component);
    }
    return components_[component].storage;
}

const RawPagedSparseArray* Registry::storage_for_entity(Entity entity, ComponentId component) const {
    if (component >= components_.size()) {
        return nullptr;
    }
    if (const OwningGroupBase* owner = components_[component].owner; owner != nullptr && owner->contains(entity)) {
        return owner->storage(component);
    }
    return components_[component].storage;
}

bool Registry::component_belongs_to_group(ComponentId component) const {
    return component < components_.size() && components_[component].owner != nullptr;
}

void Registry::refresh_groups_for_entities(const std::vector<Entity>& entities) {
    for (const Entity entity : entities) {
        for (const auto& group : groups_) {
            (void)group->promote_if_complete(*this, entity);
        }
    }
}

RawPagedSparseArray& Registry::assure_storage(
    ComponentId componentId,
    std::size_t component_size,
    std::size_t component_alignment) {

    ensure_component_slot(componentId);

    ComponentSlot& slot = components_[componentId];
    if (!slot.trace_storage_configured) {
        slot.trace_storage = ComponentTraceStorage::copy_on_write;
        slot.trace_storage_configured = true;
        slot.singleton = false;
    }

    RawPagedSparseArray* component = slot.storage;
    if (component == nullptr) {
        component = new RawPagedSparseArray(
            component_size,
            component_alignment,
            slot.trace_storage,
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

void Registry::compact_trace_history() {
    for (ComponentSlot& slot : components_) {
        RawPagedSparseArray* target = slot.storage;
        if (slot.owner != nullptr) {
            target = slot.owner->storage(&slot - components_.data());
        }
        if (target == nullptr) {
            continue;
        }

        target->compact_trace_history(trace_commit_context_.timestamp, trace_max_history_);
        target->compact_preallocated_trace_history(trace_commit_context_.timestamp, trace_max_history_);
    }
}

void Registry::capture_preallocated_trace_frames() {
    for (ComponentSlot& slot : components_) {
        RawPagedSparseArray* target = slot.storage;
        if (slot.owner != nullptr) {
            target = slot.owner->storage(&slot - components_.data());
        }
        if (target != nullptr) {
            target->capture_preallocated_trace_frame(trace_commit_context_.timestamp);
        }
    }
}

void Registry::initialize_trace_history() {
    for (ComponentSlot& slot : components_) {
        RawPagedSparseArray* target = slot.storage;
        if (slot.owner != nullptr) {
            target = slot.owner->storage(&slot - components_.data());
        }
        if (target != nullptr) {
            target->initialize_trace(trace_commit_context_);
        }
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

void Registry::register_snapshot_direct_access() {
    if (direct_access_.size() < components_.size()) {
        direct_access_.resize(components_.size());
    }

    for (std::size_t i = 0; i < components_.size(); ++i) {
        DirectAccessState& state = direct_access_[i];
        if (state.writers != 0) {
            throw std::logic_error("component storage does not allow snapshots while a writer is active");
        }
    }

    for (std::size_t i = 0; i < components_.size(); ++i) {
        ++direct_access_[i].readers;
    }

    ++snapshot_direct_readers_;
}

void Registry::unregister_snapshot_direct_access() {
    if (snapshot_direct_readers_ == 0) {
        return;
    }

    for (std::size_t i = 0; i < components_.size() && i < direct_access_.size(); ++i) {
        if (direct_access_[i].readers != 0) {
            --direct_access_[i].readers;
        }
    }

    --snapshot_direct_readers_;
}

void Registry::unregister_direct_access(const std::vector<DirectAccessRegistration>& registrations) {
    for (const DirectAccessRegistration& registration : registrations) {
        if (registration.component >= direct_access_.size()) {
            continue;
        }

        DirectAccessState& state = direct_access_[registration.component];
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
