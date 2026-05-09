#include "ecs/ecs.hpp"

namespace ecs {

Registry::Registry() {
    register_system_tag();
    register_job_tag();
    register_primitive_types();
}

Entity Registry::register_component(ComponentDesc desc) {
    require_runtime_registry_access_allowed("register_component");
    ComponentLifecycle lifecycle;
    lifecycle.trivially_copyable = true;
    return register_component_impl(std::move(desc), lifecycle, true, false, false, npos_type_id);
}

Entity Registry::register_tag(std::string name) {
    require_runtime_registry_access_allowed("register_tag");
    ComponentDesc desc;
    desc.name = std::move(name);
    desc.size = 0;
    desc.alignment = 1;

    ComponentLifecycle lifecycle;
    lifecycle.trivially_copyable = true;
    return register_component_impl(std::move(desc), lifecycle, true, false, true, npos_type_id);
}

Entity Registry::primitive_type(PrimitiveType type) const {
    return component_catalog_.primitive_types[static_cast<std::size_t>(type)];
}

Entity Registry::system_tag() const {
    return component_catalog_.system_tag;
}

Entity Registry::job_tag() const {
    return component_catalog_.job_tag;
}

const ComponentInfo* Registry::component_info(Entity component) const {
    require_runtime_registry_access_allowed("component_info");
    const ComponentRecord* record = find_component_record(component);
    return record != nullptr ? &record->info : nullptr;
}

std::string Registry::component_name(Entity component) const {
    require_runtime_registry_access_allowed("component_name");
    const ComponentRecord* record = find_component_record(component);
    return record != nullptr ? record->name : std::string{};
}

const std::vector<ComponentField>* Registry::component_fields(Entity component) const {
    require_runtime_registry_access_allowed("component_fields");
    const ComponentRecord* record = find_component_record(component);
    return record != nullptr ? &record->fields : nullptr;
}

bool Registry::set_component_fields(Entity component, std::vector<ComponentField> fields) {
    require_runtime_registry_access_allowed("set_component_fields");
    ComponentRecord* record = find_component_record(component);
    if (record == nullptr || !valid_component_fields(*record, fields)) {
        return false;
    }

    record->fields = std::move(fields);
    return true;
}

bool Registry::add_component_field(Entity component, ComponentField field) {
    require_runtime_registry_access_allowed("add_component_field");
    ComponentRecord* record = find_component_record(component);
    if (record == nullptr || !valid_component_field(*record, field)) {
        return false;
    }

    record->fields.push_back(std::move(field));
    return true;
}

bool Registry::add_tag(Entity entity, Entity tag) {
    require_runtime_registry_access_allowed("add_tag");
    const ComponentRecord& record = require_component_record(tag);
    if (!record.info.tag) {
        throw std::logic_error("ecs component entity is not a tag");
    }
    if (!alive(entity)) {
        return false;
    }

    storage_for(tag).emplace_or_replace_tag(entity_index(entity));
    refresh_group_after_add(entity_index(entity), entity_index(tag));
    return true;
}

bool Registry::remove_tag(Entity entity, Entity tag) {
    require_runtime_registry_access_allowed("remove_tag");
    return remove(entity, tag);
}

void* Registry::add(Entity entity, Entity component, const void* value) {
    require_runtime_registry_access_allowed("add");
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
    refresh_group_after_add(entity_index(entity), entity_index(component));
    return added;
}

void* Registry::write(Entity entity, Entity component) {
    require_runtime_registry_access_allowed("write");
    const ComponentRecord& record = require_component_record(component);
    if (record.info.tag) {
        throw std::logic_error("ecs tags cannot be written");
    }
    if (record.singleton) {
        entity = component_catalog_.singleton_entity;
    }
    if (!alive(entity)) {
        return nullptr;
    }

    auto* found = find_storage(component);
    return found != nullptr ? found->write(entity_index(entity)) : nullptr;
}

bool Registry::has(Entity entity, Entity component) const {
    require_runtime_registry_access_allowed("has");
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

bool Registry::dirty_clear(Entity entity, Entity component) {
    const ComponentRecord& record = require_component_record(component);
    if (record.singleton) {
        entity = component_catalog_.singleton_entity;
    }
    if (!alive(entity)) {
        return false;
    }

    auto* found = find_storage(component);
    return found != nullptr && found->clear_dirty(entity_index(entity));
}

bool Registry::dirty_is(Entity entity, Entity component) const {
    const ComponentRecord& record = require_component_record(component);
    if (record.singleton) {
        entity = component_catalog_.singleton_entity;
    }
    if (!alive(entity)) {
        return false;
    }

    const auto* found = find_storage(component);
    return found != nullptr && found->is_dirty(entity_index(entity));
}

void Registry::dirty_clear_all(Entity component) {
    require_component_record(component);
    if (auto* found = find_storage(component)) {
        found->clear_all_dirty();
    }
}

void Registry::clear_all_dirty_entries() {
    for (auto& storage : storage_registry_.storages) {
        storage.second->clear_all_dirty();
    }
}

void Registry::set_job_thread_executor(JobThreadExecutor executor) {
    require_runtime_registry_access_allowed("set_job_thread_executor");
    job_registry_.thread_executor = std::move(executor);
}

std::string Registry::debug_print(Entity entity, Entity component) const {
    require_runtime_registry_access_allowed("debug_print");
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

std::vector<EntityComponentInfo> Registry::components(Entity entity) const {
    require_runtime_registry_access_allowed("components");
    std::vector<EntityComponentInfo> result;
    if (!alive(entity)) {
        return result;
    }

    for (const auto& entry : component_catalog_.records) {
        const ComponentRecord& record = entry.second;
        if (record.singleton) {
            continue;
        }
        const Entity storage_entity = entity;
        if (!alive(storage_entity)) {
            continue;
        }

        const TypeErasedStorage* storage = find_storage(record.entity);
        const std::uint32_t index = entity_index(storage_entity);
        if (storage == nullptr || !storage->contains_index(index)) {
            continue;
        }

        EntityComponentInfo info;
        info.component = record.entity;
        info.name = record.name;
        info.info = record.info;
        info.singleton = record.singleton;
        info.dirty = storage->is_dirty(index);
        info.debug_value = debug_print(entity, record.entity);
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const EntityComponentInfo& lhs, const EntityComponentInfo& rhs) {
        return lhs.component.value < rhs.component.value;
    });
    return result;
}

std::vector<EntityComponentInfo> Registry::singleton_components() const {
    require_runtime_registry_access_allowed("singleton_components");
    std::vector<EntityComponentInfo> result;
    const Entity singleton = component_catalog_.singleton_entity;
    if (!singleton || !alive(singleton)) {
        return result;
    }

    for (const auto& entry : component_catalog_.records) {
        const ComponentRecord& record = entry.second;
        if (!record.singleton) {
            continue;
        }

        const TypeErasedStorage* storage = find_storage(record.entity);
        const std::uint32_t index = entity_index(singleton);
        if (storage == nullptr || !storage->contains_index(index)) {
            continue;
        }

        EntityComponentInfo info;
        info.component = record.entity;
        info.name = record.name;
        info.info = record.info;
        info.singleton = true;
        info.dirty = storage->is_dirty(index);
        info.debug_value = debug_print(singleton, record.entity);
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const EntityComponentInfo& lhs, const EntityComponentInfo& rhs) {
        return lhs.component.value < rhs.component.value;
    });
    return result;
}

Entity Registry::singleton_storage_entity() const {
    require_runtime_registry_access_allowed("singleton_storage_entity");
    return component_catalog_.singleton_entity;
}

std::optional<JobInfo> Registry::job_info(Entity job) const {
    require_runtime_registry_access_allowed("job_info");
    const auto found = job_registry_.index_by_entity.find(job.value);
    if (found == job_registry_.index_by_entity.end() || found->second >= job_registry_.jobs.size()) {
        return std::nullopt;
    }

    const JobRecord& record = job_registry_.jobs[found->second];
    JobInfo info;
    info.entity = record.entity;
    info.name = record.name;
    info.order = record.order;
    info.structural = record.structural;
    info.single_thread = record.single_thread;
    info.max_threads = record.max_threads;
    info.min_entities_per_thread = record.min_entities_per_thread;
    info.reads.reserve(record.reads.size());
    for (std::uint32_t component : record.reads) {
        const auto component_record = component_catalog_.records.find(component);
        if (component_record != component_catalog_.records.end()) {
            info.reads.push_back(component_record->second.entity);
        }
    }
    info.writes.reserve(record.writes.size());
    for (std::uint32_t component : record.writes) {
        const auto component_record = component_catalog_.records.find(component);
        if (component_record != component_catalog_.records.end()) {
            info.writes.push_back(component_record->second.entity);
        }
    }
    return info;
}

std::vector<Entity> Registry::job_matching_entities(Entity job) const {
    require_runtime_registry_access_allowed("job_matching_entities");
    const auto found = job_registry_.index_by_entity.find(job.value);
    if (found == job_registry_.index_by_entity.end() || found->second >= job_registry_.jobs.size()) {
        return {};
    }

    std::vector<Entity> result;
    const std::vector<std::uint32_t> indices = job_registry_.jobs[found->second].collect_indices(
        const_cast<Registry&>(*this));
    result.reserve(indices.size());
    for (std::uint32_t index : indices) {
        if (index < entity_store_.slots.size()) {
            Entity entity{entity_store_.slots[index]};
            if (alive(entity)) {
                result.push_back(entity);
            }
        }
    }
    return result;
}

std::size_t Registry::next_type_id() {
    static std::atomic<std::size_t> next{0};
    return next++;
}

std::uint64_t Registry::next_state_token() {
    static std::atomic<std::uint64_t> next{1};
    return next++;
}

std::vector<Registry::TypedComponentBinding>& Registry::typed_component_bindings() {
    static std::vector<TypedComponentBinding> bindings;
    return bindings;
}

std::mutex& Registry::typed_component_bindings_mutex() {
    static std::mutex mutex;
    return mutex;
}

void Registry::record_typed_component_binding(
    std::size_t id,
    std::string name,
    ComponentInfo info,
    bool singleton) {
    std::lock_guard<std::mutex> lock(typed_component_bindings_mutex());
    auto& bindings = typed_component_bindings();
    if (id >= bindings.size()) {
        bindings.resize(id + 1);
    }
    bindings[id] = TypedComponentBinding{std::move(name), info, singleton, true};
}

void Registry::rebind_typed_components_by_registered_names() {
    std::vector<TypedComponentBinding> bindings;
    {
        std::lock_guard<std::mutex> lock(typed_component_bindings_mutex());
        bindings = typed_component_bindings();
    }

    component_catalog_.typed_components.assign(bindings.size(), Entity{});
    storage_registry_.typed_storages.assign(bindings.size(), nullptr);
    for (std::size_t id = 0; id < bindings.size(); ++id) {
        const TypedComponentBinding& binding = bindings[id];
        if (!binding.registered || binding.name.empty()) {
            continue;
        }

        const auto found_name = component_catalog_.names.find(binding.name);
        if (found_name == component_catalog_.names.end()) {
            continue;
        }

        const auto found_record = component_catalog_.records.find(found_name->second);
        if (found_record == component_catalog_.records.end()) {
            continue;
        }

        const ComponentRecord& record = found_record->second;
        if (record.info.size != binding.info.size ||
            record.info.alignment != binding.info.alignment ||
            record.info.trivially_copyable != binding.info.trivially_copyable ||
            record.info.tag != binding.info.tag ||
            record.singleton != binding.singleton) {
            throw std::logic_error("restored component metadata does not match registered component type");
        }

        component_catalog_.typed_components[id] = record.entity;
        const auto found_storage = storage_registry_.storages.find(found_record->first);
        storage_registry_.typed_storages[id] =
            found_storage != storage_registry_.storages.end() ? found_storage->second.get() : nullptr;
    }
}

void Registry::ensure_typed_capacity(std::size_t id) {
    if (id >= component_catalog_.typed_components.size()) {
        component_catalog_.typed_components.resize(id + 1);
        storage_registry_.typed_storages.resize(id + 1);
    }
}

Entity Registry::singleton_entity() {
    if (!component_catalog_.singleton_entity) {
        component_catalog_.singleton_entity = create();
    }
    return component_catalog_.singleton_entity;
}

Entity Registry::register_component_impl(
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
        const auto by_name = component_catalog_.names.find(desc.name);
        if (by_name != component_catalog_.names.end()) {
            const Entity existing = component_catalog_.records.at(by_name->second).entity;
            ComponentRecord& record = component_catalog_.records.at(by_name->second);
            if (record.info.size != desc.size ||
                record.info.alignment != desc.alignment ||
                record.info.trivially_copyable != trivially_copyable ||
                record.info.tag != tag ||
                record.singleton != singleton) {
                throw std::logic_error("component name is already registered with different metadata");
            }

            if (typed_id != npos_type_id) {
                if (record.type_id != npos_type_id && record.type_id != typed_id) {
                    throw std::logic_error("component name is already registered for a different component type");
                }
                ensure_typed_capacity(typed_id);
                component_catalog_.typed_components[typed_id] = existing;
                storage_registry_.typed_storages[typed_id] = find_storage(existing);
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
    if (!valid_component_fields(record, desc.fields)) {
        throw std::invalid_argument("component field metadata is invalid");
    }
    record.fields = std::move(desc.fields);
    record.lifecycle = lifecycle;
    record.type_id = typed_id;
    record.singleton = singleton;

    const std::uint32_t index = entity_index(component);
    if (!record.name.empty()) {
        component_catalog_.names[record.name] = index;
    }
    component_catalog_.records[index] = std::move(record);
    return component;
}

Registry::ComponentRecord* Registry::find_component_record(Entity component) {
    if (!alive(component)) {
        return nullptr;
    }

    auto found = component_catalog_.records.find(entity_index(component));
    if (found == component_catalog_.records.end() || found->second.entity != component) {
        return nullptr;
    }

    return &found->second;
}

const Registry::ComponentRecord* Registry::find_component_record(Entity component) const {
    if (!alive(component)) {
        return nullptr;
    }

    auto found = component_catalog_.records.find(entity_index(component));
    if (found == component_catalog_.records.end() || found->second.entity != component) {
        return nullptr;
    }

    return &found->second;
}

Registry::ComponentRecord& Registry::require_component_record(Entity component) {
    ComponentRecord* record = find_component_record(component);
    if (record == nullptr) {
        throw std::logic_error("ecs component entity is not registered");
    }

    return *record;
}

const Registry::ComponentRecord& Registry::require_component_record(Entity component) const {
    const ComponentRecord* record = find_component_record(component);
    if (record == nullptr) {
        throw std::logic_error("ecs component entity is not registered");
    }

    return *record;
}

bool Registry::valid_component_field(const ComponentRecord& component, const ComponentField& field) const {
    const ComponentRecord* field_type = find_component_record(field.type);
    if (field_type == nullptr || field_type->info.tag || field.count == 0) {
        return false;
    }

    const std::size_t field_size = field_type->info.size;
    const std::size_t field_alignment = field_type->info.alignment;
    if (field_size == 0 || field_alignment == 0 || field.offset % field_alignment != 0) {
        return false;
    }

    if (field.count > std::numeric_limits<std::size_t>::max() / field_size) {
        return false;
    }

    const std::size_t byte_size = field.count * field_size;
    return field.offset <= component.info.size && byte_size <= component.info.size - field.offset;
}

bool Registry::valid_component_fields(
    const ComponentRecord& component,
    const std::vector<ComponentField>& fields) const {
    return std::all_of(fields.begin(), fields.end(), [&](const ComponentField& field) {
        return valid_component_field(component, field);
    });
}

void Registry::rebuild_typed_storages() {
    storage_registry_.typed_storages.assign(component_catalog_.typed_components.size(), nullptr);
    for (const auto& component : component_catalog_.records) {
        const ComponentRecord& record = component.second;
        if (record.type_id != npos_type_id && record.type_id < storage_registry_.typed_storages.size()) {
            const auto found = storage_registry_.storages.find(component.first);
            storage_registry_.typed_storages[record.type_id] =
                found != storage_registry_.storages.end() ? found->second.get() : nullptr;
        }
    }
}

void Registry::require_tag_component(Entity component) const {
    const ComponentRecord& record = require_component_record(component);
    if (!record.info.tag) {
        throw std::logic_error("ecs component entity is not a tag");
    }
}

void Registry::unregister_component_entity(Entity component) {
    ComponentRecord* record = find_component_record(component);
    if (record == nullptr) {
        return;
    }

    const std::uint32_t component_index = entity_index(component);
    group_index_.groups.erase(
        std::remove_if(group_index_.groups.begin(), group_index_.groups.end(), [&](const auto& group) {
            return group_contains_component(*group, component_index);
        }),
        group_index_.groups.end());
    rebuild_group_ownership();
    storage_registry_.storages.erase(component_index);

    if (!record->name.empty()) {
        component_catalog_.names.erase(record->name);
    }

    if (record->type_id != npos_type_id && record->type_id < component_catalog_.typed_components.size()) {
        component_catalog_.typed_components[record->type_id] = Entity{};
        storage_registry_.typed_storages[record->type_id] = nullptr;
    }

    for (std::size_t index = 0; index < component_catalog_.typed_components.size(); ++index) {
        if (component_catalog_.typed_components[index] == component) {
            component_catalog_.typed_components[index] = Entity{};
            storage_registry_.typed_storages[index] = nullptr;
        }
    }

    for (Entity& primitive : component_catalog_.primitive_types) {
        if (primitive == component) {
            primitive = Entity{};
        }
    }
    if (component_catalog_.job_tag == component) {
        component_catalog_.job_tag = Entity{};
    }

    component_catalog_.records.erase(component_index);
}

void Registry::register_primitive_types() {
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::Bool)] =
        register_primitive("bool", sizeof(bool), alignof(bool), PrimitiveKind::Bool);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::I32)] =
        register_primitive("i32", sizeof(std::int32_t), alignof(std::int32_t), PrimitiveKind::I32);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::U32)] =
        register_primitive("u32", sizeof(std::uint32_t), alignof(std::uint32_t), PrimitiveKind::U32);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::I64)] =
        register_primitive("i64", sizeof(std::int64_t), alignof(std::int64_t), PrimitiveKind::I64);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::U64)] =
        register_primitive("u64", sizeof(std::uint64_t), alignof(std::uint64_t), PrimitiveKind::U64);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::F32)] =
        register_primitive("f32", sizeof(float), alignof(float), PrimitiveKind::F32);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::F64)] =
        register_primitive("f64", sizeof(double), alignof(double), PrimitiveKind::F64);
    component_catalog_.primitive_types[static_cast<std::size_t>(PrimitiveType::String)] =
        register_primitive("string", sizeof(char), alignof(char), PrimitiveKind::String);
}

Entity Registry::register_primitive(std::string name, std::size_t size, std::size_t alignment, PrimitiveKind kind) {
    ComponentDesc desc;
    desc.name = std::move(name);
    desc.size = size;
    desc.alignment = alignment;

    ComponentLifecycle lifecycle;
    lifecycle.trivially_copyable = true;

    const Entity type = register_component_impl(std::move(desc), lifecycle, true, false, false, npos_type_id);
    component_catalog_.records[entity_index(type)].primitive = kind;
    add_system_tag(type);
    return type;
}

void Registry::register_system_tag() {
    ComponentDesc desc;
    desc.name = "ecs.system";
    desc.size = 0;
    desc.alignment = 1;

    ComponentLifecycle lifecycle;
    lifecycle.trivially_copyable = true;

    component_catalog_.system_tag = register_component_impl(std::move(desc), lifecycle, true, false, true, npos_type_id);
    add_system_tag(component_catalog_.system_tag);
}

void Registry::register_job_tag() {
    ComponentDesc desc;
    desc.name = "ecs.job";
    desc.size = 0;
    desc.alignment = 1;

    ComponentLifecycle lifecycle;
    lifecycle.trivially_copyable = true;

    component_catalog_.job_tag = register_component_impl(std::move(desc), lifecycle, true, false, true, npos_type_id);
    add_system_tag(component_catalog_.job_tag);
}

void Registry::add_system_tag(Entity entity) {
    if (!component_catalog_.system_tag || !alive(entity)) {
        return;
    }
    storage_for(component_catalog_.system_tag).emplace_or_replace_tag(entity_index(entity));
}

void Registry::add_job_tag(Entity entity) {
    if (!component_catalog_.job_tag || !alive(entity)) {
        return;
    }
    storage_for(component_catalog_.job_tag).emplace_or_replace_tag(entity_index(entity));
}

void Registry::canonicalize_components(std::vector<std::uint32_t>& components) {
    std::sort(components.begin(), components.end());
    components.erase(std::unique(components.begin(), components.end()), components.end());
}

void Registry::canonicalize_job_metadata(JobAccessMetadata& metadata) {
    canonicalize_components(metadata.reads);
    canonicalize_components(metadata.writes);
    std::vector<std::uint32_t> reads;
    reads.reserve(metadata.reads.size());
    std::set_difference(
        metadata.reads.begin(),
        metadata.reads.end(),
        metadata.writes.begin(),
        metadata.writes.end(),
        std::back_inserter(reads));
    metadata.reads = std::move(reads);
}

void Registry::append_unique_component(std::vector<std::uint32_t>& components, std::uint32_t component) {
    if (std::find(components.begin(), components.end(), component) == components.end()) {
        components.push_back(component);
    }
}

Entity Registry::add_job(
    int order,
    std::string name,
    JobAccessMetadata metadata,
    std::function<void(Registry&)> run,
    std::function<std::vector<std::uint32_t>(Registry&)> collect_indices,
    std::function<void(Registry&, const std::vector<std::uint32_t>&, std::size_t, std::size_t)> run_range,
    std::vector<std::uint32_t> range_dirty_writes,
    JobThreadingOptions threading,
    bool structural) {
    canonicalize_job_metadata(metadata);
    for (std::uint32_t component : metadata.writes) {
        const auto found = component_catalog_.records.find(component);
        if (found != component_catalog_.records.end() && found->second.singleton) {
            threading.single_thread = true;
            threading.max_threads = 1;
            break;
        }
    }
    const Entity entity = create();
    add_system_tag(entity);
    add_job_tag(entity);
    job_registry_.schedule_cache_valid = false;
    job_registry_.graph_cache_valid = false;
    job_registry_.ordered_indices_cache_valid = false;
    const std::size_t job_index = job_registry_.jobs.size();
    job_registry_.index_by_entity[entity.value] = job_index;
    job_registry_.jobs.push_back(JobRecord{
        entity,
        std::move(name),
        order,
        job_registry_.next_sequence++,
        std::move(metadata.reads),
        std::move(metadata.writes),
        std::move(run),
        std::move(collect_indices),
        std::move(run_range),
        std::move(range_dirty_writes),
        std::max<std::size_t>(threading.max_threads, 1),
        std::max<std::size_t>(threading.min_entities_per_thread, 1),
        threading.single_thread,
        structural});
    return entity;
}

bool Registry::is_snapshot_excluded_index(std::uint32_t index) const {
    if (!component_catalog_.system_tag || component_catalog_.records.find(index) != component_catalog_.records.end()) {
        return false;
    }

    const TypeErasedStorage* system_storage = find_storage(component_catalog_.system_tag);
    return system_storage != nullptr && system_storage->contains_index(index);
}

std::vector<bool> Registry::make_snapshot_exclusion_mask() const {
    std::vector<bool> excluded;
    const TypeErasedStorage* system_storage = component_catalog_.system_tag ? find_storage(component_catalog_.system_tag) : nullptr;
    if (system_storage == nullptr) {
        return excluded;
    }

    for (std::size_t dense = 0; dense < system_storage->dense_size(); ++dense) {
        const std::uint32_t index = system_storage->dense_index_at(dense);
        if (index >= entity_store_.slots.size() || component_catalog_.records.find(index) != component_catalog_.records.end()) {
            continue;
        }
        if (excluded.empty()) {
            excluded.resize(entity_store_.slots.size(), false);
        }
        excluded[index] = true;
    }
    return excluded;
}

std::vector<std::uint64_t> Registry::make_snapshot_entities(const std::vector<bool>& excluded) const {
    std::vector<std::uint64_t> entities = entity_store_.slots;
    if (excluded.empty()) {
        return entities;
    }
    for (std::size_t index = 0; index < entities.size() && index < excluded.size(); ++index) {
        if (excluded[index]) {
            entities[index] = 0;
        }
    }
    return entities;
}

std::vector<Entity> Registry::current_snapshot_excluded_entities() const {
    std::vector<Entity> entities;
    const TypeErasedStorage* system_storage = component_catalog_.system_tag ? find_storage(component_catalog_.system_tag) : nullptr;
    if (system_storage != nullptr) {
        for (std::size_t dense = 0; dense < system_storage->dense_size(); ++dense) {
            const std::uint32_t index = system_storage->dense_index_at(dense);
            if (is_snapshot_excluded_index(index) && index < entity_store_.slots.size()) {
                entities.push_back(Entity{entity_store_.slots[index]});
            }
        }
    }

    for (const JobRecord& job : job_registry_.jobs) {
        if (alive(job.entity) && std::find(entities.begin(), entities.end(), job.entity) == entities.end()) {
            entities.push_back(job.entity);
        }
    }
    return entities;
}

void Registry::merge_current_system_entities(std::vector<std::uint64_t>& entities) const {
    for (Entity entity : current_snapshot_excluded_entities()) {
        const std::uint32_t index = entity_index(entity);
        if (index >= entities.size()) {
            entities.resize(static_cast<std::size_t>(index) + 1, 0);
        }
        entities[index] = entity.value;
    }
}

void Registry::restore_internal_bookkeeping_tags() {
    add_system_tag(component_catalog_.system_tag);
    add_system_tag(component_catalog_.job_tag);
    for (Entity primitive : component_catalog_.primitive_types) {
        add_system_tag(primitive);
    }
    for (const JobRecord& job : job_registry_.jobs) {
        add_system_tag(job.entity);
        add_job_tag(job.entity);
    }
}

void Registry::print_field(std::ostringstream& out, const unsigned char* data, const ComponentField& field) const {
    const ComponentRecord* type = find_component_record(field.type);
    if (type == nullptr || (field.count != 1 && type->primitive != PrimitiveKind::String)) {
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
    case PrimitiveKind::String:
        out << "\"";
        for (std::size_t i = 0; i < field.count && data[i] != '\0'; ++i) {
            const unsigned char ch = data[i];
            out << (ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?');
        }
        out << "\"";
        break;
    case PrimitiveKind::None:
        out << "<unprintable>";
        break;
    }
}

const std::vector<std::size_t>& Registry::ordered_job_indices() const {
    if (job_registry_.ordered_indices_cache_valid) {
        return job_registry_.ordered_indices_cache;
    }

    job_registry_.ordered_indices_cache.resize(job_registry_.jobs.size());
    std::iota(job_registry_.ordered_indices_cache.begin(), job_registry_.ordered_indices_cache.end(), std::size_t{0});
    std::sort(
        job_registry_.ordered_indices_cache.begin(),
        job_registry_.ordered_indices_cache.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            const JobRecord& left = job_registry_.jobs[lhs];
            const JobRecord& right = job_registry_.jobs[rhs];
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.sequence < right.sequence;
        });
    job_registry_.ordered_indices_cache_valid = true;
    return job_registry_.ordered_indices_cache;
}

std::size_t Registry::job_index(Entity job) const {
    const auto found = job_registry_.index_by_entity.find(job.value);
    if (found == job_registry_.index_by_entity.end()) {
        throw std::logic_error("ecs job is not registered");
    }
    return found->second;
}

bool Registry::job_excluded_by_options(const JobRecord& job, const RunJobsOptions& options) const {
    for (Entity tag : options.excluded_job_tags) {
        if (tag && has(job.entity, tag)) {
            return true;
        }
    }
    return false;
}

}  // namespace ecs
