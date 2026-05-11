#include "ashiato/ashiato.hpp"

namespace ashiato {

bool Registry::includes_all(const std::vector<std::uint32_t>& lhs, const std::vector<std::uint32_t>& rhs) {
    return std::includes(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

Registry::GroupRecord& Registry::group_for_key(const std::vector<std::uint32_t>& key) {
    for (const auto& group : group_index_.groups) {
        if (group->owned == key) {
            return *group;
        }
    }

    validate_group_key(key);
    auto group = std::make_unique<GroupRecord>();
    group->owned = key;
    group_index_.groups.push_back(std::move(group));
    bump_view_topology_token();
    GroupRecord& created = *group_index_.groups.back();
    build_group(created);
    register_group_ownership(created);
    return created;
}

void Registry::validate_group_key(const std::vector<std::uint32_t>& key) const {
    for (std::uint32_t component : key) {
        auto found = group_index_.owned_component_groups.find(component);
        if (found != group_index_.owned_component_groups.end() && found->second->owned != key) {
            throw std::logic_error("ashiato owned components cannot be shared by distinct owned groups");
        }
    }
}

void Registry::register_group_ownership(GroupRecord& group) {
    for (std::uint32_t component : group.owned) {
        group_index_.owned_component_groups[component] = &group;
    }
}

void Registry::rebuild_group_ownership() {
    group_index_.owned_component_groups.clear();
    for (const auto& group : group_index_.groups) {
        register_group_ownership(*group);
    }
}

void Registry::build_group(GroupRecord& group) {
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

Registry::TypeErasedStorage* Registry::smallest_storage(const GroupRecord& group) {
    TypeErasedStorage* driver = nullptr;
    for (std::uint32_t component : group.owned) {
        TypeErasedStorage* storage = find_storage(Entity{entity_store_.slots[component]});
        if (storage == nullptr) {
            return nullptr;
        }
        if (driver == nullptr || storage->dense_size() < driver->dense_size()) {
            driver = storage;
        }
    }
    return driver;
}

bool Registry::group_contains_all(const GroupRecord& group, std::uint32_t index) const {
    for (std::uint32_t component : group.owned) {
        const TypeErasedStorage* storage = find_storage(Entity{entity_store_.slots[component]});
        if (storage == nullptr || !storage->contains_index(index)) {
            return false;
        }
    }
    return true;
}

bool Registry::group_contains_component(const GroupRecord& group, std::uint32_t component) const {
    return std::binary_search(group.owned.begin(), group.owned.end(), component);
}

bool Registry::group_contains_index(const GroupRecord& group, std::uint32_t index) const {
    if (group.owned.empty() || group.size == 0) {
        return false;
    }
    const TypeErasedStorage* storage = find_storage(Entity{entity_store_.slots[group.owned.front()]});
    if (storage == nullptr) {
        return false;
    }
    const std::uint32_t dense = storage->dense_position(index);
    return dense != TypeErasedStorage::npos && dense < group.size;
}

void Registry::enter_group(GroupRecord& group, std::uint32_t index) {
    if (group_contains_index(group, index)) {
        return;
    }
    const std::uint32_t target = static_cast<std::uint32_t>(group.size);
    for (std::uint32_t component : group.owned) {
        storage_for(Entity{entity_store_.slots[component]}).move_index_to_dense(index, target);
    }
    ++group.size;
}

void Registry::leave_group(GroupRecord& group, std::uint32_t index) {
    if (!group_contains_index(group, index)) {
        return;
    }
    --group.size;
    const std::uint32_t target = static_cast<std::uint32_t>(group.size);
    for (std::uint32_t component : group.owned) {
        storage_for(Entity{entity_store_.slots[component]}).move_index_to_dense(index, target);
    }
}

void Registry::refresh_group_after_add(std::uint32_t index, std::uint32_t component) {
    auto found = group_index_.owned_component_groups.find(component);
    if (found == group_index_.owned_component_groups.end()) {
        return;
    }
    GroupRecord& group = *found->second;
    if (group_contains_all(group, index)) {
        enter_group(group, index);
    }
}

void Registry::remove_from_groups_before_component_removal(std::uint32_t index, std::uint32_t component) {
    auto found = group_index_.owned_component_groups.find(component);
    if (found == group_index_.owned_component_groups.end()) {
        return;
    }
    leave_group(*found->second, index);
}

}  // namespace ashiato
