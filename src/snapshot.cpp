#include "ashiato/ashiato.hpp"

namespace ashiato {

namespace detail {

constexpr std::uint32_t persistent_magic = 0x50534345U;  // ECSP
constexpr std::uint32_t persistent_version = 1U;
constexpr std::uint32_t persistent_full_kind = 1U;
constexpr std::uint32_t persistent_delta_kind = 2U;

void write_exact(std::ostream& out, const void* data, std::size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error("ashiato snapshot write failed");
    }
}

void read_exact(std::istream& in, void* data, std::size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("ashiato snapshot read failed");
    }
}

template <typename T>
void write_pod(std::ostream& out, T value) {
    static_assert(std::is_trivially_copyable<T>::value, "snapshot POD I/O requires trivially copyable values");
    write_exact(out, &value, sizeof(T));
}

template <typename T>
T read_pod(std::istream& in) {
    static_assert(std::is_trivially_copyable<T>::value, "snapshot POD I/O requires trivially copyable values");
    T value{};
    read_exact(in, &value, sizeof(T));
    return value;
}

void write_string(std::ostream& out, const std::string& value) {
    write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(value.size()));
    if (!value.empty()) {
        write_exact(out, value.data(), value.size());
    }
}

std::string read_string(std::istream& in) {
    const auto size = read_pod<std::uint64_t>(in);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("ashiato snapshot string is too large");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!value.empty()) {
        read_exact(in, &value[0], value.size());
    }
    return value;
}

void write_u32_le(std::ostream& out, std::uint32_t value) {
    unsigned char bytes[4]{
        static_cast<unsigned char>(value & 0xffU),
        static_cast<unsigned char>((value >> 8U) & 0xffU),
        static_cast<unsigned char>((value >> 16U) & 0xffU),
        static_cast<unsigned char>((value >> 24U) & 0xffU),
    };
    write_exact(out, bytes, sizeof(bytes));
}

void write_u64_le(std::ostream& out, std::uint64_t value) {
    unsigned char bytes[8]{};
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
    write_exact(out, bytes, sizeof(bytes));
}

std::uint32_t read_u32_le(std::istream& in) {
    unsigned char bytes[4]{};
    read_exact(in, bytes, sizeof(bytes));
    return std::uint32_t{bytes[0]} |
        (std::uint32_t{bytes[1]} << 8U) |
        (std::uint32_t{bytes[2]} << 16U) |
        (std::uint32_t{bytes[3]} << 24U);
}

std::uint64_t read_u64_le(std::istream& in) {
    unsigned char bytes[8]{};
    read_exact(in, bytes, sizeof(bytes));
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= std::uint64_t{bytes[index]} << (index * 8U);
    }
    return value;
}

void write_bits(BitBuffer& out, std::uint64_t value, std::size_t bits) {
    out.push_unsigned_bits(value, bits);
}

std::uint64_t read_bits(BitBuffer& in, std::size_t bits) {
    return in.read_unsigned_bits(bits);
}

void write_buffer_string(BitBuffer& out, const std::string& value) {
    write_bits(out, static_cast<std::uint64_t>(value.size()), 32U);
    if (!value.empty()) {
        out.push_bytes(value.data(), value.size());
    }
}

std::string read_buffer_string(BitBuffer& in) {
    const auto size = read_bits(in, 32U);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("persistent snapshot string is too large");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!value.empty()) {
        in.read_bytes(&value[0], value.size());
    }
    return value;
}

void write_frame(std::ostream& out, std::uint32_t kind, const BitBuffer& body) {
    write_u32_le(out, persistent_magic);
    write_u32_le(out, persistent_version);
    write_u32_le(out, kind);
    write_u64_le(out, static_cast<std::uint64_t>(body.bit_size()));
    if (!body.bytes().empty()) {
        write_exact(out, body.bytes().data(), body.byte_size());
    }
}

BitBuffer read_frame(std::istream& in, std::uint32_t expected_kind) {
    if (read_u32_le(in) != persistent_magic ||
        read_u32_le(in) != persistent_version ||
        read_u32_le(in) != expected_kind) {
        throw std::runtime_error("persistent snapshot header is invalid");
    }
    const auto bit_size = read_u64_le(in);
    if (bit_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("persistent snapshot frame is too large");
    }
    const auto byte_size = static_cast<std::size_t>((bit_size + 7U) / 8U);
    std::vector<std::uint8_t> bytes(byte_size);
    if (!bytes.empty()) {
        read_exact(in, bytes.data(), bytes.size());
    }
    BitBuffer body;
    body.assign_bytes(std::move(bytes), static_cast<std::size_t>(bit_size));
    return body;
}

}  // namespace detail

bool snapshot_component_selected(ashiato::Entity component, const SnapshotComponentOptions& options) {
    const bool included = options.include_components.empty() ||
        std::find(options.include_components.begin(), options.include_components.end(), component) !=
            options.include_components.end();
    if (!included) {
        return false;
    }
    return std::find(options.exclude_components.begin(), options.exclude_components.end(), component) ==
        options.exclude_components.end();
}

void write_component_record(std::ostream& out, const Registry::ComponentRecord& record) {
    detail::write_pod<std::uint64_t>(out, record.entity.value);
    detail::write_string(out, record.name);
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(record.info.size));
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(record.info.alignment));
    detail::write_pod<std::uint8_t>(out, record.info.trivially_copyable ? 1U : 0U);
    detail::write_pod<std::uint8_t>(out, record.info.tag ? 1U : 0U);
    detail::write_pod<std::uint8_t>(out, record.singleton ? 1U : 0U);
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(record.type_id));
    detail::write_pod<std::uint32_t>(out, static_cast<std::uint32_t>(record.primitive));
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(record.fields.size()));
    for (const ComponentField& field : record.fields) {
        detail::write_string(out, field.name);
        detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(field.offset));
        detail::write_pod<std::uint64_t>(out, field.type.value);
        detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(field.count));
    }
}

Registry::ComponentRecord read_component_record(std::istream& in) {
    Registry::ComponentRecord record;
    record.entity = Entity{detail::read_pod<std::uint64_t>(in)};
    record.name = detail::read_string(in);
    const auto size = detail::read_pod<std::uint64_t>(in);
    const auto alignment = detail::read_pod<std::uint64_t>(in);
    record.info.size = static_cast<std::size_t>(size);
    record.info.alignment = static_cast<std::size_t>(alignment);
    record.info.trivially_copyable = detail::read_pod<std::uint8_t>(in) != 0U;
    record.info.tag = detail::read_pod<std::uint8_t>(in) != 0U;
    record.singleton = detail::read_pod<std::uint8_t>(in) != 0U;
    const auto type_id = detail::read_pod<std::uint64_t>(in);
    record.type_id = type_id > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        ? Registry::npos_type_id
        : static_cast<std::size_t>(type_id);
    record.primitive = static_cast<Registry::PrimitiveKind>(detail::read_pod<std::uint32_t>(in));
    record.lifecycle.trivially_copyable = record.info.trivially_copyable;
    const auto field_count = detail::read_pod<std::uint64_t>(in);
    record.fields.reserve(static_cast<std::size_t>(field_count));
    for (std::uint64_t index = 0; index < field_count; ++index) {
        ComponentField field;
        field.name = detail::read_string(in);
        field.offset = static_cast<std::size_t>(detail::read_pod<std::uint64_t>(in));
        field.type = Entity{detail::read_pod<std::uint64_t>(in)};
        field.count = static_cast<std::size_t>(detail::read_pod<std::uint64_t>(in));
        record.fields.push_back(std::move(field));
    }
    return record;
}

void write_storage(std::ostream& out, const Registry::TypeErasedStorage& storage) {
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(storage.dense_size()));
    for (std::size_t dense = 0; dense < storage.dense_size(); ++dense) {
        detail::write_pod<std::uint32_t>(out, storage.dense_index_at(dense));
        detail::write_pod<std::uint8_t>(out, storage.dirty_[dense]);
        detail::write_pod<std::uint8_t>(out, storage.added_[dense]);
        if (!storage.info_.tag) {
            detail::write_exact(out, storage.get_dense(dense), storage.info_.size);
        }
    }
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(storage.tombstone_entry_count()));
    for (std::size_t position = 0; position < storage.tombstone_entry_count(); ++position) {
        detail::write_pod<std::uint32_t>(out, storage.tombstone_index_at(position));
        detail::write_pod<unsigned char>(out, storage.tombstone_flags_at(position));
    }
}

std::unique_ptr<Registry::TypeErasedStorage> read_storage(
    std::istream& in,
    const Registry::ComponentRecord& record) {
    auto storage = std::make_unique<Registry::TypeErasedStorage>(record);
    const auto dense_count = detail::read_pod<std::uint64_t>(in);
    for (std::uint64_t dense = 0; dense < dense_count; ++dense) {
        const auto entity_index = detail::read_pod<std::uint32_t>(in);
        const auto dirty = detail::read_pod<std::uint8_t>(in);
        const auto added = detail::read_pod<std::uint8_t>(in);
        if (record.info.tag) {
            storage->emplace_or_replace_tag(entity_index);
        } else {
            std::vector<unsigned char> bytes(record.info.size);
            if (!bytes.empty()) {
                detail::read_exact(in, bytes.data(), bytes.size());
            }
            storage->emplace_or_replace_bytes(entity_index, bytes.empty() ? nullptr : bytes.data());
        }
        if (dirty == 0U) {
            storage->clear_dirty(entity_index);
        } else if (added == 0U) {
            storage->clear_added_dense(storage->dense_position(entity_index));
        }
    }
    const auto tombstone_count = detail::read_pod<std::uint64_t>(in);
    for (std::uint64_t position = 0; position < tombstone_count; ++position) {
        const auto entity_index = detail::read_pod<std::uint32_t>(in);
        const auto flags = detail::read_pod<unsigned char>(in);
        storage->mark_tombstone(entity_index, flags);
    }
    storage->rebuild_lookup();
    return storage;
}

void validate_serializable_component(const Registry::ComponentRecord& record) {
    if (!record.info.tag && !record.info.trivially_copyable) {
        throw std::logic_error("ashiato snapshot selected component is not disk serializable");
    }
}

void write_snapshot_common(
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
    const SnapshotComponentOptions& options) {
    constexpr std::uint32_t magic = 0x53534345U;  // ECSS
    constexpr std::uint32_t version = 1U;
    detail::write_pod<std::uint32_t>(out, magic);
    detail::write_pod<std::uint32_t>(out, version);
    detail::write_pod<std::uint32_t>(out, kind);
    detail::write_pod<std::uint8_t>(out, has_entities ? 1U : 0U);
    detail::write_pod<std::uint64_t>(out, baseline_token);
    detail::write_pod<std::uint64_t>(out, state_token);
    detail::write_pod<std::uint32_t>(out, free_head);
    detail::write_pod<std::uint64_t>(out, singleton_entity.value);
    detail::write_pod<std::uint64_t>(out, system_tag.value);
    for (std::size_t index = 0; index < 7U; ++index) {
        detail::write_pod<std::uint64_t>(out, primitive_types[index].value);
    }

    auto selected = [&](Entity component) {
        if (component == system_tag) {
            return true;
        }
        for (std::size_t index = 0; index < 7U; ++index) {
            if (component == primitive_types[index]) {
                return true;
            }
        }
        return snapshot_component_selected(component, options);
    };

    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(entities.size()));
    for (std::uint64_t slot : entities) {
        detail::write_pod<std::uint64_t>(out, slot);
    }

    std::vector<std::uint32_t> component_indices;
    component_indices.reserve(components.size());
    for (const auto& component : components) {
        if (selected(component.second.entity)) {
            validate_serializable_component(component.second);
            component_indices.push_back(component.first);
        }
    }
    std::sort(component_indices.begin(), component_indices.end());
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(component_indices.size()));
    for (std::uint32_t index : component_indices) {
        detail::write_pod<std::uint32_t>(out, index);
        write_component_record(out, components.at(index));
    }

    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(typed_components.size()));
    for (Entity component : typed_components) {
        detail::write_pod<std::uint64_t>(
            out,
            selected(component) ? component.value : std::uint64_t{0});
    }

    if (groups == nullptr) {
        detail::write_pod<std::uint64_t>(out, 0U);
    } else {
        detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(groups->size()));
        for (const auto& group : *groups) {
            detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(group->owned.size()));
            for (std::uint32_t owned : group->owned) {
                detail::write_pod<std::uint32_t>(out, owned);
            }
            detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(group->size));
        }
    }

    std::vector<std::uint32_t> storage_indices;
    storage_indices.reserve(storages.size());
    for (const auto& storage : storages) {
        const auto found_component = components.find(storage.first);
        if (found_component == components.end() ||
            !selected(found_component->second.entity)) {
            continue;
        }
        validate_serializable_component(found_component->second);
        storage_indices.push_back(storage.first);
    }
    std::sort(storage_indices.begin(), storage_indices.end());
    detail::write_pod<std::uint64_t>(out, static_cast<std::uint64_t>(storage_indices.size()));
    for (std::uint32_t index : storage_indices) {
        detail::write_pod<std::uint32_t>(out, index);
        write_storage(out, *storages.at(index));
    }
}

void read_snapshot_header(
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
    std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages) {
    constexpr std::uint32_t magic = 0x53534345U;
    constexpr std::uint32_t version = 1U;
    if (detail::read_pod<std::uint32_t>(in) != magic ||
        detail::read_pod<std::uint32_t>(in) != version ||
        detail::read_pod<std::uint32_t>(in) != expected_kind) {
        throw std::runtime_error("ashiato snapshot header is invalid");
    }
    has_entities = detail::read_pod<std::uint8_t>(in) != 0U;
    baseline_token = detail::read_pod<std::uint64_t>(in);
    state_token = detail::read_pod<std::uint64_t>(in);
    free_head = detail::read_pod<std::uint32_t>(in);
    singleton_entity = Entity{detail::read_pod<std::uint64_t>(in)};
    system_tag = Entity{detail::read_pod<std::uint64_t>(in)};
    for (std::size_t index = 0; index < 7U; ++index) {
        primitive_types[index] = Entity{detail::read_pod<std::uint64_t>(in)};
    }

    const auto entity_count = detail::read_pod<std::uint64_t>(in);
    entities.resize(static_cast<std::size_t>(entity_count));
    for (std::uint64_t& slot : entities) {
        slot = detail::read_pod<std::uint64_t>(in);
    }

    const auto component_count = detail::read_pod<std::uint64_t>(in);
    for (std::uint64_t offset = 0; offset < component_count; ++offset) {
        const auto index = detail::read_pod<std::uint32_t>(in);
        Registry::ComponentRecord record = read_component_record(in);
        components[index] = std::move(record);
    }

    const auto typed_count = detail::read_pod<std::uint64_t>(in);
    typed_components.resize(static_cast<std::size_t>(typed_count));
    for (Entity& component : typed_components) {
        component = Entity{detail::read_pod<std::uint64_t>(in)};
    }

    const auto group_count = detail::read_pod<std::uint64_t>(in);
    groups.reserve(static_cast<std::size_t>(group_count));
    for (std::uint64_t group_index = 0; group_index < group_count; ++group_index) {
        auto group = std::make_unique<Registry::GroupRecord>();
        const auto owned_count = detail::read_pod<std::uint64_t>(in);
        group->owned.reserve(static_cast<std::size_t>(owned_count));
        for (std::uint64_t owned_index = 0; owned_index < owned_count; ++owned_index) {
            group->owned.push_back(detail::read_pod<std::uint32_t>(in));
        }
        group->size = static_cast<std::size_t>(detail::read_pod<std::uint64_t>(in));
        groups.push_back(std::move(group));
    }

    const auto storage_count = detail::read_pod<std::uint64_t>(in);
    for (std::uint64_t offset = 0; offset < storage_count; ++offset) {
        const auto index = detail::read_pod<std::uint32_t>(in);
        const auto found = components.find(index);
        if (found == components.end()) {
            throw std::runtime_error("ashiato snapshot storage references missing component metadata");
        }
        storages[index] = read_storage(in, found->second);
    }
}

void Registry::Snapshot::write(std::ostream& out, const SnapshotComponentOptions& options) const {
    write_native(out, options);
}

void Registry::Snapshot::write_native(std::ostream& out, const SnapshotComponentOptions& options) const {
    write_snapshot_common(
        out,
        1U,
        entities_,
        free_head_,
        components_,
        storages_,
        typed_components_,
        &groups_,
        singleton_entity_,
        primitive_types_,
        system_tag_,
        true,
        0U,
        state_token_,
        options);
}

Registry::Snapshot Registry::Snapshot::read(std::istream& in) {
    return read_native(in);
}

Registry::Snapshot Registry::Snapshot::read_native(std::istream& in) {
    Snapshot snapshot;
    bool has_entities = false;
    std::uint64_t baseline_token = 0;
    read_snapshot_header(
        in,
        1U,
        has_entities,
        baseline_token,
        snapshot.state_token_,
        snapshot.free_head_,
        snapshot.singleton_entity_,
        snapshot.primitive_types_,
        snapshot.system_tag_,
        snapshot.entities_,
        snapshot.components_,
        snapshot.typed_components_,
        snapshot.groups_,
        snapshot.storages_);
    if (!has_entities || baseline_token != 0U) {
        throw std::runtime_error("ashiato full snapshot payload is invalid");
    }
    for (const auto& component : snapshot.components_) {
        if (!component.second.name.empty()) {
            snapshot.component_names_[component.second.name] = component.first;
        }
    }
    return snapshot;
}

void Registry::DeltaSnapshot::write(std::ostream& out, const SnapshotComponentOptions& options) const {
    write_native(out, options);
}

void Registry::DeltaSnapshot::write_native(std::ostream& out, const SnapshotComponentOptions& options) const {
    std::vector<Entity> empty_typed_components;
    std::array<Entity, 7> empty_primitives{};
    write_snapshot_common(
        out,
        2U,
        entities_,
        free_head_,
        components_,
        storages_,
        empty_typed_components,
        nullptr,
        Entity{},
        empty_primitives.data(),
        Entity{},
        has_entities_,
        baseline_token_,
        state_token_,
        options);
}

Registry::DeltaSnapshot Registry::DeltaSnapshot::read(std::istream& in) {
    return read_native(in);
}

Registry::DeltaSnapshot Registry::DeltaSnapshot::read_native(std::istream& in) {
    DeltaSnapshot snapshot;
    Entity singleton_entity;
    Entity system_tag;
    std::array<Entity, 8> primitive_types{};
    std::vector<Entity> typed_components;
    std::vector<std::unique_ptr<Registry::GroupRecord>> groups;
    read_snapshot_header(
        in,
        2U,
        snapshot.has_entities_,
        snapshot.baseline_token_,
        snapshot.state_token_,
        snapshot.free_head_,
        singleton_entity,
        primitive_types.data(),
        system_tag,
        snapshot.entities_,
        snapshot.components_,
        typed_components,
        groups,
        snapshot.storages_);
    return snapshot;
}

class PersistentSnapshotAccess {
public:

struct PersistentComponentSelection {
    std::vector<std::uint32_t> storage_indices;
    std::vector<std::string> names;
    std::unordered_map<std::string, std::uint32_t> name_indices;
};

static bool is_persistent_internal_component(const Registry::ComponentRecord& record, Entity system_tag) {
    return record.primitive != Registry::PrimitiveKind::None || (system_tag && record.entity == system_tag);
}

static PersistentComponentSelection select_persistent_components(
    const std::unordered_map<std::uint32_t, Registry::ComponentRecord>& components,
    const std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages,
    Entity system_tag,
    const SnapshotComponentOptions& options) {
    PersistentComponentSelection selection;
    for (const auto& storage : storages) {
        const auto found = components.find(storage.first);
        if (found == components.end()) {
            continue;
        }
        const Registry::ComponentRecord& record = found->second;
        if (is_persistent_internal_component(record, system_tag) ||
            !snapshot_component_selected(record.entity, options)) {
            continue;
        }
        if (record.name.empty()) {
            throw std::logic_error("persistent snapshot components require non-empty names");
        }
        selection.storage_indices.push_back(storage.first);
    }
    std::sort(selection.storage_indices.begin(), selection.storage_indices.end());
    for (std::uint32_t component_index : selection.storage_indices) {
        const Registry::ComponentRecord& record = components.at(component_index);
        const auto inserted = selection.name_indices.emplace(
            record.name,
            static_cast<std::uint32_t>(selection.names.size()));
        if (!inserted.second) {
            throw std::logic_error("persistent snapshot component names must be unique");
        }
        selection.names.push_back(record.name);
    }
    return selection;
}

static const ComponentSerializationRegistry::Entry* persistent_serialization_for(
    const ComponentSerializationRegistry& serialization,
    const Registry::ComponentRecord& record) {
    if (record.info.tag) {
        return nullptr;
    }
    const auto found = serialization.entries_.find(record.name);
    if (found == serialization.entries_.end()) {
        throw std::logic_error("persistent snapshot component is missing serialization: " + record.name);
    }
    return &found->second;
}

static std::vector<unsigned char> quantized_value(
    const ComponentSerializationOps& ops,
    const Registry::TypeErasedStorage& storage,
    std::uint32_t entity_index) {
    const void* value = storage.get(entity_index);
    if (value == nullptr) {
        throw std::logic_error("persistent snapshot baseline value is missing");
    }
    std::vector<unsigned char> quantized(ops.quantized_size);
    ops.quantize(value, quantized.data());
    return quantized;
}

static std::vector<unsigned char> baseline_quantized(
    const Registry::Snapshot* baseline,
    const Registry::ComponentRecord& record,
    const ComponentSerializationOps& ops,
    std::uint32_t entity_index) {
    if (baseline == nullptr) {
        return {};
    }
    const auto found_storage = baseline->storages_.find(Registry::entity_index(record.entity));
    if (found_storage == baseline->storages_.end() ||
        !found_storage->second->contains_index(entity_index)) {
        return {};
    }
    return quantized_value(ops, *found_storage->second, entity_index);
}

static void write_entities(BitBuffer& body, const std::vector<std::uint64_t>& entities) {
    detail::write_bits(body, static_cast<std::uint64_t>(entities.size()), 32U);
    for (std::uint64_t slot : entities) {
        detail::write_bits(body, slot, 64U);
    }
}

static std::vector<std::uint64_t> read_entities(BitBuffer& body) {
    const auto count = detail::read_bits(body, 32U);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("persistent snapshot entity count is too large");
    }
    std::vector<std::uint64_t> entities(static_cast<std::size_t>(count));
    for (std::uint64_t& slot : entities) {
        slot = detail::read_bits(body, 64U);
    }
    return entities;
}

static void write_name_table(BitBuffer& body, const std::vector<std::string>& names) {
    detail::write_bits(body, static_cast<std::uint64_t>(names.size()), 32U);
    for (const std::string& name : names) {
        detail::write_buffer_string(body, name);
    }
}

static std::vector<std::string> read_name_table(BitBuffer& body) {
    const auto count = detail::read_bits(body, 32U);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("persistent snapshot component name table is too large");
    }
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(count));
    std::unordered_map<std::string, bool> seen;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string name = detail::read_buffer_string(body);
        if (name.empty() || !seen.emplace(name, true).second) {
            throw std::runtime_error("persistent snapshot component names must be unique and non-empty");
        }
        names.push_back(std::move(name));
    }
    return names;
}

static void write_payload(BitBuffer& body, const BitBuffer& payload) {
    detail::write_bits(body, static_cast<std::uint64_t>(payload.bit_size()), 32U);
    body.push_buffer_bits(payload);
}

static BitBuffer read_payload(BitBuffer& body) {
    const auto bit_count = detail::read_bits(body, 32U);
    if (bit_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("persistent snapshot payload is too large");
    }
    BitBuffer payload;
    body.read_buffer_bits(payload, static_cast<std::size_t>(bit_count));
    payload.reset_read();
    return payload;
}

static void write_persistent_storages(
    BitBuffer& body,
    const std::unordered_map<std::uint32_t, Registry::ComponentRecord>& components,
    const std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages,
    const Registry::Snapshot* baseline,
    const ComponentSerializationRegistry& serialization,
    const PersistentComponentSelection& selection) {
    detail::write_bits(body, static_cast<std::uint64_t>(selection.storage_indices.size()), 32U);
    for (std::uint32_t component_index : selection.storage_indices) {
        const Registry::ComponentRecord& record = components.at(component_index);
        const Registry::TypeErasedStorage& storage = *storages.at(component_index);
        const auto found_name = selection.name_indices.find(record.name);
        if (found_name == selection.name_indices.end()) {
            throw std::logic_error("persistent snapshot component name table is incomplete");
        }
        const ComponentSerializationRegistry::Entry* serialization_entry = persistent_serialization_for(serialization, record);

        detail::write_bits(body, found_name->second, 32U);
        detail::write_bits(body, record.info.tag ? 1U : 0U, 1U);
        detail::write_bits(body, record.singleton ? 1U : 0U, 1U);
        detail::write_bits(body, static_cast<std::uint64_t>(storage.dense_size()), 32U);
        for (std::size_t dense = 0; dense < storage.dense_size(); ++dense) {
            const std::uint32_t entity_index = storage.dense_index_at(dense);
            detail::write_bits(body, entity_index, 32U);
            detail::write_bits(body, storage.is_dirty(entity_index) ? 1U : 0U, 1U);
            if (!record.info.tag) {
                const ComponentSerializationOps& ops = serialization_entry->ops;
                std::vector<unsigned char> current(ops.quantized_size);
                ops.quantize(storage.get_dense(dense), current.data());
                const std::vector<unsigned char> previous =
                    baseline_quantized(baseline, record, ops, entity_index);
                BitBuffer payload;
                ops.serialize(
                    previous.empty() ? nullptr : previous.data(),
                    current.data(),
                    payload,
                    nullptr);
                write_payload(body, payload);
            }
        }

        detail::write_bits(body, static_cast<std::uint64_t>(storage.tombstone_entry_count()), 32U);
        for (std::size_t position = 0; position < storage.tombstone_entry_count(); ++position) {
            detail::write_bits(body, storage.tombstone_index_at(position), 32U);
            detail::write_bits(body, storage.tombstone_flags_at(position), 8U);
        }
    }
}

static std::unique_ptr<Registry::TypeErasedStorage> read_persistent_storage(
    BitBuffer& body,
    const Registry::ComponentRecord& record,
    std::uint32_t singleton_index,
    const Registry::Snapshot* baseline,
    const ComponentSerializationRegistry& serialization,
    bool encoded_tag,
    bool encoded_singleton) {
    if (record.info.tag != encoded_tag || record.singleton != encoded_singleton) {
        throw std::runtime_error("persistent snapshot component metadata does not match schema");
    }
    const ComponentSerializationRegistry::Entry* serialization_entry = persistent_serialization_for(serialization, record);
    auto storage = std::make_unique<Registry::TypeErasedStorage>(record);
    const auto dense_count = detail::read_bits(body, 32U);
    for (std::uint64_t dense = 0; dense < dense_count; ++dense) {
        std::uint32_t entity_index = static_cast<std::uint32_t>(detail::read_bits(body, 32U));
        const auto dirty = static_cast<std::uint8_t>(detail::read_bits(body, 1U));
        if (record.singleton) {
            entity_index = singleton_index;
        }
        if (record.info.tag) {
            storage->emplace_or_replace_tag(entity_index);
        } else {
            BitBuffer payload = read_payload(body);
            const ComponentSerializationOps& ops = serialization_entry->ops;
            const std::vector<unsigned char> previous =
                baseline_quantized(baseline, record, ops, entity_index);
            std::vector<unsigned char> quantized;
            quantized.resize(ops.quantized_size);
            if (!ops.deserialize(payload, previous.empty() ? nullptr : previous.data(), quantized.data(), nullptr)) {
                throw std::runtime_error("persistent snapshot component deserializer failed: " + record.name);
            }
            if (payload.remaining_bits() != 0U) {
                throw std::runtime_error("persistent snapshot component payload was not fully consumed: " + record.name);
            }
            serialization_entry->emplace(*storage, entity_index, quantized.data());
        }
        if (dirty == 0U) {
            storage->clear_dirty(entity_index);
        }
    }

    const auto tombstone_count = detail::read_bits(body, 32U);
    for (std::uint64_t position = 0; position < tombstone_count; ++position) {
        const auto entity_index = static_cast<std::uint32_t>(detail::read_bits(body, 32U));
        const auto flags = static_cast<unsigned char>(detail::read_bits(body, 8U));
        storage->mark_tombstone(entity_index, flags);
    }
    return storage;
}

static void read_persistent_storages(
    BitBuffer& body,
    const std::vector<std::string>& names,
    const Registry& schema,
    const Registry::Snapshot* baseline,
    const ComponentSerializationRegistry& serialization,
    std::unordered_map<std::uint32_t, Registry::ComponentRecord>& components,
    std::unordered_map<std::uint32_t, std::unique_ptr<Registry::TypeErasedStorage>>& storages) {
    const auto storage_count = detail::read_bits(body, 32U);
    for (std::uint64_t offset = 0; offset < storage_count; ++offset) {
        const auto name_index = detail::read_bits(body, 32U);
        if (name_index >= names.size()) {
            throw std::runtime_error("persistent snapshot storage references invalid component name");
        }
        const std::string& name = names[static_cast<std::size_t>(name_index)];
        const auto found_name = schema.component_catalog_.names.find(name);
        if (found_name == schema.component_catalog_.names.end()) {
            throw std::runtime_error("persistent snapshot component is missing from schema: " + name);
        }
        const std::uint32_t component_index = found_name->second;
        const Registry::ComponentRecord& record = schema.component_catalog_.records.at(component_index);
        const bool encoded_tag = detail::read_bits(body, 1U) != 0U;
        const bool encoded_singleton = detail::read_bits(body, 1U) != 0U;
        components[component_index] = record;
        storages[component_index] = read_persistent_storage(
            body,
            record,
            Registry::entity_index(schema.component_catalog_.singleton_entity),
            baseline,
            serialization,
            encoded_tag,
            encoded_singleton);
    }
}

};

void write_persistent_snapshot(
    std::ostream& out,
    const Registry::Snapshot& snapshot,
    const ComponentSerializationRegistry& serialization,
    const SnapshotComponentOptions& options) {
    const auto selection =
        PersistentSnapshotAccess::select_persistent_components(
            snapshot.components_,
            snapshot.storages_,
            snapshot.system_tag_,
            options);
    BitBuffer body;
    detail::write_bits(body, snapshot.state_token_, 64U);
    detail::write_bits(body, 0U, 64U);
    detail::write_bits(body, 1U, 1U);
    detail::write_bits(body, snapshot.free_head_, 32U);
    PersistentSnapshotAccess::write_entities(body, snapshot.entities_);
    PersistentSnapshotAccess::write_name_table(body, selection.names);
    PersistentSnapshotAccess::write_persistent_storages(
        body,
        snapshot.components_,
        snapshot.storages_,
        nullptr,
        serialization,
        selection);
    detail::write_frame(out, detail::persistent_full_kind, body);
}

Registry::Snapshot read_persistent_snapshot(
    std::istream& in,
    const Registry& schema,
    const ComponentSerializationRegistry& serialization) {
    BitBuffer body = detail::read_frame(in, detail::persistent_full_kind);
    Registry::Snapshot result = schema.create_snapshot();
    result.state_token_ = detail::read_bits(body, 64U);
    const std::uint64_t baseline_token = detail::read_bits(body, 64U);
    const bool has_entities = detail::read_bits(body, 1U) != 0U;
    result.free_head_ = static_cast<std::uint32_t>(detail::read_bits(body, 32U));
    if (!has_entities || baseline_token != 0U) {
        throw std::runtime_error("persistent full snapshot payload is invalid");
    }
    result.entities_ = PersistentSnapshotAccess::read_entities(body);
    const Registry::Snapshot schema_snapshot = schema.create_snapshot();
    if (result.entities_.size() < schema_snapshot.entities_.size()) {
        result.entities_.resize(schema_snapshot.entities_.size(), 0U);
    }
    for (std::size_t index = 0; index < schema_snapshot.entities_.size(); ++index) {
        const std::uint64_t slot = schema_snapshot.entities_[index];
        if (Registry::slot_index(slot) == index) {
            result.entities_[index] = slot;
        }
    }
    const std::vector<std::string> names = PersistentSnapshotAccess::read_name_table(body);
    result.storages_.clear();
    PersistentSnapshotAccess::read_persistent_storages(
        body,
        names,
        schema,
        nullptr,
        serialization,
        result.components_,
        result.storages_);
    if (body.remaining_bits() != 0U) {
        throw std::runtime_error("persistent full snapshot frame was not fully consumed");
    }
    return result;
}

void write_persistent_delta_snapshot(
    std::ostream& out,
    const Registry::DeltaSnapshot& snapshot,
    const Registry::Snapshot& baseline,
    const ComponentSerializationRegistry& serialization,
    const SnapshotComponentOptions& options) {
    const auto selection =
        PersistentSnapshotAccess::select_persistent_components(
            snapshot.components_,
            snapshot.storages_,
            Entity{},
            options);
    BitBuffer body;
    detail::write_bits(body, snapshot.state_token_, 64U);
    detail::write_bits(body, snapshot.baseline_token_, 64U);
    detail::write_bits(body, snapshot.has_entities_ ? 1U : 0U, 1U);
    detail::write_bits(body, snapshot.free_head_, 32U);
    PersistentSnapshotAccess::write_entities(body, snapshot.entities_);
    PersistentSnapshotAccess::write_name_table(body, selection.names);
    PersistentSnapshotAccess::write_persistent_storages(
        body,
        snapshot.components_,
        snapshot.storages_,
        &baseline,
        serialization,
        selection);
    detail::write_frame(out, detail::persistent_delta_kind, body);
}

Registry::DeltaSnapshot read_persistent_delta_snapshot(
    std::istream& in,
    const Registry& schema,
    const Registry::Snapshot& baseline,
    const ComponentSerializationRegistry& serialization) {
    BitBuffer body = detail::read_frame(in, detail::persistent_delta_kind);
    Registry::DeltaSnapshot result;
    result.state_token_ = detail::read_bits(body, 64U);
    result.baseline_token_ = detail::read_bits(body, 64U);
    result.has_entities_ = detail::read_bits(body, 1U) != 0U;
    result.free_head_ = static_cast<std::uint32_t>(detail::read_bits(body, 32U));
    result.entities_ = PersistentSnapshotAccess::read_entities(body);
    const std::vector<std::string> names = PersistentSnapshotAccess::read_name_table(body);
    PersistentSnapshotAccess::read_persistent_storages(
        body,
        names,
        schema,
        &baseline,
        serialization,
        result.components_,
        result.storages_);
    if (body.remaining_bits() != 0U) {
        throw std::runtime_error("persistent delta snapshot frame was not fully consumed");
    }
    return result;
}

RegistryDirtyFrameBroadcastSubscription::RegistryDirtyFrameBroadcastSubscription(
    std::shared_ptr<State> state,
    std::uint64_t id)
    : state_(std::move(state)),
      id_(id) {}

RegistryDirtyFrameBroadcastSubscription::RegistryDirtyFrameBroadcastSubscription(
    RegistryDirtyFrameBroadcastSubscription&& other) noexcept
    : state_(std::move(other.state_)),
      id_(other.id_) {
    other.id_ = 0;
}

RegistryDirtyFrameBroadcastSubscription& RegistryDirtyFrameBroadcastSubscription::operator=(
    RegistryDirtyFrameBroadcastSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

RegistryDirtyFrameBroadcastSubscription::~RegistryDirtyFrameBroadcastSubscription() {
    reset();
}

void RegistryDirtyFrameBroadcastSubscription::reset() {
    auto state = state_.lock();
    if (state == nullptr || id_ == 0) {
        return;
    }
    for (Entry& entry : state->consumers) {
        if (entry.id == id_) {
            entry.listener = nullptr;
            break;
        }
    }
    id_ = 0;
    state_.reset();
}

bool RegistryDirtyFrameBroadcastSubscription::active() const noexcept {
    return id_ != 0 && !state_.expired();
}

RegistryDirtyFrameBroadcaster::RegistryDirtyFrameBroadcaster()
    : listeners_(std::make_shared<RegistryDirtyFrameBroadcastSubscription::State>()) {}

RegistryDirtyFrameBroadcastSubscription RegistryDirtyFrameBroadcaster::subscribe(
    RegistryDirtyFrameBroadcastListener& listener) {
    const std::uint64_t id = listeners_->next_id++;
    listeners_->consumers.push_back(RegistryDirtyFrameBroadcastSubscription::Entry{id, &listener});
    return RegistryDirtyFrameBroadcastSubscription(listeners_, id);
}

void RegistryDirtyFrameBroadcaster::broadcast(Registry& registry) {
    const auto dirty_scope = registry.dirty_scope();
    const RegistryDirtyFrame dirty_frame{registry, dirty_scope.view()};
    for (const RegistryDirtyFrameBroadcastSubscription::Entry& entry : listeners_->consumers) {
        if (entry.listener != nullptr) {
            entry.listener->on_registry_dirty_frame(dirty_frame);
        }
    }
    remove_unsubscribed();
}

void RegistryDirtyFrameBroadcaster::remove_unsubscribed() {
    listeners_->consumers.erase(
        std::remove_if(
            listeners_->consumers.begin(),
            listeners_->consumers.end(),
            [](const RegistryDirtyFrameBroadcastSubscription::Entry& entry) {
                return entry.listener == nullptr;
            }),
        listeners_->consumers.end());
}

DirtySnapshotTracker::DirtySnapshotTracker(DirtySnapshotTrackerOptions options)
    : options_(std::move(options)) {}

DirtySnapshotTracker::~DirtySnapshotTracker() = default;

DirtySnapshotTracker::DirtySnapshotTracker(DirtySnapshotTracker&&) noexcept = default;

DirtySnapshotTracker& DirtySnapshotTracker::operator=(DirtySnapshotTracker&&) noexcept = default;

void DirtySnapshotTracker::on_registry_dirty_frame(const RegistryDirtyFrame& frame) {
    if (!options_.write) {
        return;
    }

    ++dirty_frame_count_;
    const std::uint64_t full_interval = options_.full_snapshot_interval_dirty_frames;
    const bool write_full =
        last_full_ == nullptr ||
        full_interval == 0U ||
        dirty_frame_count_ % full_interval == 0U;
    if (write_full) {
        auto snapshot = std::make_unique<Registry::Snapshot>(frame.registry.create_snapshot());
        options_.write(DirtySnapshotFrame{
            &frame.registry,
            DirtySnapshotFrameKind::Full,
            snapshot.get(),
            nullptr});
        last_full_ = std::move(snapshot);
        last_delta_.reset();
        return;
    }

    auto delta = last_delta_ != nullptr
        ? std::make_unique<Registry::DeltaSnapshot>(frame.registry.create_delta_snapshot(*last_delta_))
        : std::make_unique<Registry::DeltaSnapshot>(frame.registry.create_delta_snapshot(*last_full_));
    options_.write(DirtySnapshotFrame{
        &frame.registry,
        DirtySnapshotFrameKind::Delta,
        nullptr,
        delta.get()});
    last_delta_ = std::move(delta);
}

Registry::Snapshot Registry::create_snapshot() const {
    require_runtime_registry_access_allowed("create_snapshot");
    Snapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    result.entities_ = make_snapshot_entities(excluded);
    result.free_head_ = rebuild_free_list(result.entities_);
    result.components_ = component_catalog_.records;
    result.component_names_ = component_catalog_.names;
    result.typed_components_ = component_catalog_.typed_components;
    result.singleton_entity_ = component_catalog_.singleton_entity;
    result.system_tag_ = component_catalog_.system_tag;
    result.state_token_ = state_token_;
    std::copy(std::begin(component_catalog_.primitive_types), std::end(component_catalog_.primitive_types), std::begin(result.primitive_types_));

    result.storages_.reserve(storage_registry_.storages.size());
    for (const auto& storage : storage_registry_.storages) {
        if (component_catalog_.system_tag && storage.first == entity_index(component_catalog_.system_tag)) {
            continue;
        }
        result.storages_.emplace(storage.first, storage.second->clone_excluding(excluded));
    }

    result.groups_.reserve(group_index_.groups.size());
    for (const auto& group : group_index_.groups) {
        result.groups_.push_back(std::make_unique<GroupRecord>(*group));
    }

    return result;
}

Registry::DeltaSnapshot Registry::create_delta_snapshot(const Snapshot& baseline) const {
    require_runtime_registry_access_allowed("create_delta_snapshot");
    DeltaSnapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    if ((excluded.empty() && entity_store_.slots == baseline.entities_) ||
        (!excluded.empty() && make_snapshot_entities(excluded) == baseline.entities_)) {
        result.has_entities_ = false;
        result.free_head_ = baseline.free_head_;
    } else {
        result.entities_ = make_snapshot_entities(excluded);
        result.free_head_ = rebuild_free_list(result.entities_);
    }
    result.baseline_token_ = baseline.state_token_;
    result.state_token_ = next_state_token();

    for (const auto& storage : storage_registry_.storages) {
        if (component_catalog_.system_tag && storage.first == entity_index(component_catalog_.system_tag)) {
            continue;
        }
        if (!storage.second->has_dirty_entries()) {
            continue;
        }
        auto found_component = component_catalog_.records.find(storage.first);
        if (found_component == component_catalog_.records.end()) {
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

Registry::DeltaSnapshot Registry::create_delta_snapshot(const DeltaSnapshot& baseline) const {
    require_runtime_registry_access_allowed("create_delta_snapshot");
    DeltaSnapshot result;
    const std::vector<bool> excluded = make_snapshot_exclusion_mask();
    if (baseline.has_entities_ &&
        ((excluded.empty() && entity_store_.slots == baseline.entities_) ||
         (!excluded.empty() && make_snapshot_entities(excluded) == baseline.entities_))) {
        result.has_entities_ = false;
        result.free_head_ = baseline.free_head_;
    } else {
        result.entities_ = make_snapshot_entities(excluded);
        result.free_head_ = rebuild_free_list(result.entities_);
    }
    result.baseline_token_ = baseline.state_token_;
    result.state_token_ = next_state_token();

    for (const auto& storage : storage_registry_.storages) {
        if (component_catalog_.system_tag && storage.first == entity_index(component_catalog_.system_tag)) {
            continue;
        }
        if (!storage.second->has_dirty_entries()) {
            continue;
        }
        auto found_component = component_catalog_.records.find(storage.first);
        if (found_component == component_catalog_.records.end()) {
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

void Registry::restore_snapshot(const Snapshot& snapshot) {
    require_runtime_registry_access_allowed("restore_snapshot");
    std::unordered_map<std::uint32_t, std::unique_ptr<TypeErasedStorage>> storages;
    storages.reserve(snapshot.storages_.size());
    for (const auto& storage : snapshot.storages_) {
        storages.emplace(storage.first, storage.second->clone_for_restore());
    }

    std::vector<std::unique_ptr<GroupRecord>> groups;
    groups.reserve(snapshot.groups_.size());
    for (const auto& group : snapshot.groups_) {
        groups.push_back(std::make_unique<GroupRecord>(*group));
    }

    std::vector<std::uint64_t> entities = snapshot.entities_;
    merge_current_system_entities(entities);
    const std::uint32_t free_head = rebuild_free_list(entities);

    entity_store_.slots = std::move(entities);
    entity_store_.free_head = free_head;
    component_catalog_.records = snapshot.components_;
    component_catalog_.names = snapshot.component_names_;
    storage_registry_.storages = std::move(storages);
    rebind_typed_components_by_registered_names();
    group_index_.groups = std::move(groups);
    rebuild_group_ownership();
    component_catalog_.singleton_entity = snapshot.singleton_entity_;
    component_catalog_.system_tag = snapshot.system_tag_;
    state_token_ = snapshot.state_token_;
    bump_view_topology_token();
    std::copy(std::begin(snapshot.primitive_types_), std::end(snapshot.primitive_types_), std::begin(component_catalog_.primitive_types));
    restore_internal_bookkeeping_tags();
}

void Registry::restore_delta_snapshot(const DeltaSnapshot& snapshot) {
    require_runtime_registry_access_allowed("restore_delta_snapshot");
    if (state_token_ != snapshot.baseline_token_) {
        throw std::logic_error("ashiato delta snapshot baseline does not match registry state");
    }

    for (const auto& component : snapshot.components_) {
        const auto found = component_catalog_.records.find(component.first);
        if (found == component_catalog_.records.end()) {
            throw std::logic_error("ashiato delta snapshot component is not registered");
        }

        const ComponentRecord& current = found->second;
        const ComponentRecord& captured = component.second;
        if (current.entity != captured.entity ||
            current.info.size != captured.info.size ||
            current.info.alignment != captured.info.alignment ||
            current.info.trivially_copyable != captured.info.trivially_copyable ||
            current.info.tag != captured.info.tag ||
            current.singleton != captured.singleton) {
            throw std::logic_error("ashiato delta snapshot component metadata does not match registry");
        }
    }

    if (snapshot.has_entities_) {
        std::vector<std::uint64_t> entities = snapshot.entities_;
        merge_current_system_entities(entities);
        entity_store_.slots = std::move(entities);
        entity_store_.free_head = rebuild_free_list(entity_store_.slots);
    }

    std::vector<std::uint32_t> destroyed_indices;
    for (const auto& storage : snapshot.storages_) {
        const TypeErasedStorage& delta_storage = *storage.second;
        for (std::size_t tombstone = 0; tombstone < delta_storage.tombstone_entry_count(); ++tombstone) {
            const std::uint32_t entity_index = delta_storage.tombstone_index_at(tombstone);
            if (delta_storage.has_destroy_tombstone_at_position(tombstone) &&
                std::find(destroyed_indices.begin(), destroyed_indices.end(), entity_index) == destroyed_indices.end()) {
                destroyed_indices.push_back(entity_index);
            }
        }
    }

    for (std::uint32_t index : destroyed_indices) {
        for (auto& storage : storage_registry_.storages) {
            if (storage.second->contains_index(index)) {
                remove_from_groups_before_component_removal(index, storage.first);
                storage.second->remove_index(index);
            }
        }
    }

    for (const auto& storage : snapshot.storages_) {
        const std::uint32_t component_index = storage.first;
        const auto found_component = component_catalog_.records.find(component_index);
        if (found_component == component_catalog_.records.end()) {
            throw std::logic_error("ashiato delta snapshot component is not registered");
        }
        const Entity component = found_component->second.entity;
        const TypeErasedStorage& delta_storage = *storage.second;
        TypeErasedStorage* target_storage = find_storage(component);

        for (std::size_t tombstone = 0; tombstone < delta_storage.tombstone_entry_count(); ++tombstone) {
            const std::uint32_t entity_index = delta_storage.tombstone_index_at(tombstone);
            if (!delta_storage.has_dirty_tombstone_at_position(tombstone) ||
                delta_storage.has_destroy_tombstone_at_position(tombstone)) {
                continue;
            }
            if (target_storage == nullptr || !target_storage->contains_index(entity_index)) {
                throw std::logic_error("ashiato delta snapshot component removal does not match registry state");
            }
            remove_from_groups_before_component_removal(entity_index, component_index);
            target_storage->remove_index(entity_index);
        }

        for (std::size_t dense = 0; dense < delta_storage.dense_size(); ++dense) {
            const std::uint32_t entity_index = delta_storage.dense_index_at(dense);
            if (entity_index >= entity_store_.slots.size() || slot_index(entity_store_.slots[entity_index]) != entity_index) {
                throw std::logic_error("ashiato delta snapshot entity is not alive");
            }
            storage_for(component).emplace_or_replace_copy(entity_index, delta_storage.get_dense(dense));
            refresh_group_after_add(entity_index, component_index);
        }
    }

    state_token_ = snapshot.state_token_;
    bump_view_topology_token();
    restore_internal_bookkeeping_tags();
}

}  // namespace ashiato
