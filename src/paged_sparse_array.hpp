#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "component_index.hpp"
#include "sparse_set.hpp"

namespace ecs {

class Registry;
template <typename T, typename TransactionType>
class TransactionStorageView;

enum class ComponentStorageMode : std::uint8_t {
    mvcc,
    classic,
    trace,
};

struct TraceCommitContext {
    std::uint32_t timestamp = 0;
    std::uint16_t writer_id = 0;
};

struct TraceChangeInfo {
    std::uint32_t timestamp = 0;
    std::uint16_t writer_id = 0;
    bool tombstone = false;
};

class RawPagedSparseArray {
public:
    static constexpr std::size_t inline_revision_capacity = 3;
    static constexpr std::size_t overflow_revision_capacity = 5;
    static constexpr std::uint32_t npos32 = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t trace_timestamp_bits = 22;
    static constexpr std::uint32_t trace_writer_id_bits = 10;
    static constexpr std::uint32_t max_trace_timestamp = (1u << trace_timestamp_bits) - 1u;
    static constexpr std::uint16_t max_trace_writer_id = static_cast<std::uint16_t>((1u << trace_writer_id_bits) - 1u);

    struct RevisionRef {
        std::uint64_t tsn = 0;
        std::uint32_t value_index = npos32;
        std::uint32_t trace_tag = 0;
        bool isolated = false;
        bool tombstone = false;
        bool voided = true;
    };

    struct RevisionInfo {
        std::uint32_t item_count = 0;
        std::uint32_t overflow_index = npos32;
        std::array<RevisionRef, inline_revision_capacity> revisions{};
    };

    struct RevisionOverflow {
        std::uint32_t next_overflow = npos32;
        std::uint32_t item_count = 0;
        std::array<RevisionRef, overflow_revision_capacity> revisions{};
    };

    struct PendingWrite {
        std::size_t dense_index = npos;
        std::uint32_t value_index = npos32;
        bool had_value_before = false;
    };

    struct VisibleRef {
        const RevisionRef* ref = nullptr;
        bool found = false;
        bool tombstone = false;
    };

    RawPagedSparseArray(std::size_t component_size,
                        std::size_t component_alignment,
                        ComponentStorageMode mode = ComponentStorageMode::mvcc,
                        std::size_t page_size = 1024)
        : component_size_(component_size),
          component_alignment_(component_alignment),
          mode_(mode),
          index_(page_size) {
        if (component_size_ == 0) {
            throw std::invalid_argument("component size must be greater than zero");
        }

        if (component_alignment_ == 0 || (component_alignment_ & (component_alignment_ - 1)) != 0) {
            throw std::invalid_argument("component alignment must be a non-zero power of two");
        }
    }

    virtual ~RawPagedSparseArray() {
        release_storage();
    }

    RawPagedSparseArray(RawPagedSparseArray&& other) noexcept
        : component_size_(other.component_size_),
          component_alignment_(other.component_alignment_),
          mode_(other.mode_),
          index_(std::move(other.index_)),
          dense_entities_(std::move(other.dense_entities_)),
          dense_data_(other.dense_data_),
          dense_capacity_(other.dense_capacity_),
          committed_count_(other.committed_count_),
          revision_headers_(std::move(other.revision_headers_)),
          revision_overflow_(std::move(other.revision_overflow_)),
          revision_values_(std::move(other.revision_values_)),
          free_rows_(std::move(other.free_rows_)) {
        other.dense_data_ = nullptr;
        other.dense_capacity_ = 0;
        other.committed_count_ = 0;
    }

    RawPagedSparseArray& operator=(RawPagedSparseArray&& other) noexcept {
        if (this != &other) {
            release_storage();
            component_size_ = other.component_size_;
            component_alignment_ = other.component_alignment_;
            mode_ = other.mode_;
            index_ = std::move(other.index_);
            dense_entities_ = std::move(other.dense_entities_);
            dense_data_ = other.dense_data_;
            dense_capacity_ = other.dense_capacity_;
            committed_count_ = other.committed_count_;
            revision_headers_ = std::move(other.revision_headers_);
            revision_overflow_ = std::move(other.revision_overflow_);
            revision_values_ = std::move(other.revision_values_);
            free_rows_ = std::move(other.free_rows_);
            other.dense_data_ = nullptr;
            other.dense_capacity_ = 0;
            other.committed_count_ = 0;
        }
        return *this;
    }

    RawPagedSparseArray(const RawPagedSparseArray&) = delete;
    RawPagedSparseArray& operator=(const RawPagedSparseArray&) = delete;

    bool contains(Entity entity) const {
        return try_get_raw(entity) != nullptr;
    }

    ComponentStorageMode storage_mode() const {
        return mode_;
    }

    static constexpr std::uint32_t max_trace_time() {
        return max_trace_timestamp;
    }

    static constexpr std::uint16_t max_trace_writer() {
        return max_trace_writer_id;
    }

    const void* try_get_raw(Entity entity) const {
        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return nullptr;
        }

        if (mode_ == ComponentStorageMode::classic) {
            return element_ptr(dense_index);
        }

        const RevisionRef* committed = last_committed_ref(dense_index);
        return committed == nullptr || committed->tombstone ? nullptr : revision_value_ptr(committed->value_index);
    }

    const void* get_raw(Entity entity) const {
        const void* value = try_get_raw(entity);
        if (value == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return value;
    }

    const void* try_get_visible_raw(
        Entity entity,
        std::uint64_t max_visible_tsn,
        const std::vector<std::uint64_t>& active_at_open,
        std::uint64_t own_tsn) const {

        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return nullptr;
        }

        if (mode_ == ComponentStorageMode::classic) {
            return element_ptr(dense_index);
        }

        const VisibleRef visible = visible_ref(dense_index, max_visible_tsn, active_at_open, own_tsn);
        return !visible.found || visible.tombstone ? nullptr : revision_value_ptr(visible.ref->value_index);
    }

    virtual bool erase(Entity entity, TraceCommitContext trace_context = {}) {
        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return false;
        }

        if (mode_ == ComponentStorageMode::classic) {
            clear_row(dense_index);
            --committed_count_;
            return true;
        }

        const RevisionRef* committed = last_committed_ref(dense_index);
        if (committed == nullptr || committed->tombstone) {
            return false;
        }

        if (mode_ == ComponentStorageMode::trace) {
            append_ref(
                dense_index,
                RevisionRef{0, npos32, pack_trace_tag(trace_context), false, true, false});
            std::memset(element_ptr(dense_index), 0, component_size_);
            --committed_count_;
            return true;
        }

        --committed_count_;
        clear_row(dense_index);
        return true;
    }

    virtual void clear() {
        release_storage();
        index_.clear();
        dense_entities_.clear();
        revision_headers_.clear();
        revision_overflow_.clear();
        revision_values_.clear();
        free_rows_.clear();
        committed_count_ = 0;
    }

    std::size_t size() const {
        return committed_count_;
    }

    bool empty() const {
        return committed_count_ == 0;
    }

    std::size_t page_size() const {
        return index_.page_size();
    }

    std::size_t component_size() const {
        return component_size_;
    }

    std::size_t component_alignment() const {
        return component_alignment_;
    }

    const std::vector<Entity>& entities() const {
        return dense_entities_;
    }

    const void* dense_at(std::size_t index) const {
        return index < dense_entities_.size() && dense_entities_[index] != null_entity ? element_ptr(index) : nullptr;
    }

    PendingWrite stage_write(Entity entity,
                             std::uint64_t tsn,
                             const void* initial_value,
                             bool has_initial_value,
                             bool preserve_previous_value = false) {
        if (mode_ == ComponentStorageMode::classic) {
            const std::size_t existing = dense_index_of(entity);
            const bool had_value_before = existing != npos;
            const std::size_t dense_index = had_value_before ? existing : assure_row(entity);
            if (!had_value_before) {
                ++committed_count_;
            }

            const std::uint32_t value_index =
                had_value_before && preserve_previous_value ? append_value(element_ptr(dense_index)) : npos32;
            if (has_initial_value) {
                std::memcpy(element_ptr(dense_index), initial_value, component_size_);
            } else if (!had_value_before) {
                std::memset(element_ptr(dense_index), 0, component_size_);
            }
            return PendingWrite{dense_index, value_index, had_value_before};
        }

        const std::size_t dense_index = assure_row(entity);
        if (find_isolated_ref(dense_index, tsn) != nullptr) {
            return PendingWrite{dense_index, npos32, false};
        }

        const std::uint32_t value_index = append_value(has_initial_value ? initial_value : nullptr);
        append_ref(dense_index, RevisionRef{tsn, value_index, 0, true, false, false});
        std::memcpy(element_ptr(dense_index), revision_value_ptr(value_index), component_size_);
        return PendingWrite{dense_index, npos32, false};
    }

    const void* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) const {
        if (mode_ == ComponentStorageMode::classic) {
            return pending.dense_index == npos ? nullptr : element_ptr(pending.dense_index);
        }

        const RevisionRef* isolated = find_isolated_ref(pending.dense_index, tsn);
        return isolated == nullptr || isolated->tombstone ? nullptr : revision_value_ptr(isolated->value_index);
    }

    void rollback_staged(Entity entity, const PendingWrite& pending, std::uint64_t tsn) {
        if (mode_ == ComponentStorageMode::classic) {
            (void)entity;
            (void)pending;
            (void)tsn;
            return;
        }

        RevisionRef* isolated = find_isolated_ref(pending.dense_index, tsn);
        if (isolated == nullptr) {
            return;
        }

        isolated->voided = true;
        recompute_dense_head(entity, pending.dense_index);
    }

    VisibleRef visible_ref(
        std::size_t dense_index,
        std::uint64_t max_visible_tsn,
        const std::vector<std::uint64_t>& active_at_open,
        std::uint64_t own_tsn) const {

        VisibleRef result{};
        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (ref.voided) {
                return false;
            }

            if (ref.isolated) {
                if (own_tsn != 0 && ref.tsn == own_tsn) {
                    result.ref = &ref;
                    result.found = true;
                    result.tombstone = ref.tombstone;
                }
                return false;
            }

            if (ref.tsn <= max_visible_tsn && !contains_tsn(active_at_open, ref.tsn)) {
                result.ref = &ref;
                result.found = true;
                result.tombstone = ref.tombstone;
            }
            return false;
        });
        return result;
    }

protected:
    std::size_t dense_index_of(Entity entity) const {
        const std::size_t sparse = index_.sparse_index(entity);
        if (sparse == PagedSparseSet::npos || sparse >= dense_entities_.size()) {
            return npos;
        }
        return dense_entities_[sparse] == entity ? sparse : npos;
    }

    const RevisionRef* last_committed_ref(std::size_t dense_index) const {
        const RevisionRef* result = nullptr;
        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (!ref.voided && !ref.isolated) {
                result = &ref;
            }
            return false;
        });
        return result;
    }

    const RevisionRef* find_isolated_ref(std::size_t dense_index, std::uint64_t tsn) const {
        const RevisionRef* result = nullptr;
        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (!ref.voided && ref.isolated && ref.tsn == tsn) {
                result = &ref;
                return true;
            }
            return false;
        });
        return result;
    }

    RevisionRef* find_isolated_ref(std::size_t dense_index, std::uint64_t tsn) {
        RevisionRef* result = nullptr;
        for_each_ref_mut(dense_index, [&](RevisionRef& ref) {
            if (!ref.voided && ref.isolated && ref.tsn == tsn) {
                result = &ref;
                return true;
            }
            return false;
        });
        return result;
    }

    bool commit_staged_ref(
        Entity entity,
        const PendingWrite& pending,
        std::uint64_t tsn,
        TraceCommitContext trace_context = {}) {

        if (mode_ == ComponentStorageMode::classic) {
            (void)entity;
            (void)pending;
            (void)tsn;
            (void)trace_context;
            return true;
        }

        RevisionRef* isolated = find_isolated_ref(pending.dense_index, tsn);
        if (isolated == nullptr) {
            return false;
        }

        const RevisionRef isolated_copy = *isolated;
        isolated->voided = true;
        append_ref(
            pending.dense_index,
            RevisionRef{
                tsn,
                isolated_copy.value_index,
                mode_ == ComponentStorageMode::trace ? pack_trace_tag(trace_context) : 0,
                false,
                isolated_copy.tombstone,
                false});
        recompute_dense_head(entity, pending.dense_index);
        return true;
    }

    template <typename Func>
    void for_each_trace_change(Entity entity, Func&& func) const {
        if (mode_ != ComponentStorageMode::trace) {
            return;
        }

        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return;
        }

        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (ref.voided || ref.isolated) {
                return false;
            }

            const TraceChangeInfo info = unpack_trace_change(ref);
            func(info, ref.tombstone ? nullptr : revision_value_ptr(ref.value_index));
            return false;
        });
    }

    bool rollback_to_trace_timestamp(Entity entity, std::uint32_t timestamp, TraceCommitContext trace_context) {
        if (mode_ != ComponentStorageMode::trace) {
            throw std::logic_error("trace rollback is only supported for trace component storage");
        }

        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return false;
        }

        const RevisionRef* target = nullptr;
        const RevisionRef* latest = nullptr;
        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (ref.voided || ref.isolated) {
                return false;
            }

            latest = &ref;
            const TraceCommitContext candidate = unpack_trace_context(ref.trace_tag);
            if (candidate.timestamp <= timestamp) {
                target = &ref;
            }
            return false;
        });

        if (latest == nullptr) {
            return false;
        }

        const bool current_present = !latest->tombstone;
        const bool target_present = target != nullptr && !target->tombstone;
        const bool same_state =
            current_present == target_present &&
            (!current_present ||
             std::memcmp(
                 revision_value_ptr(latest->value_index),
                 revision_value_ptr(target->value_index),
                 component_size_) == 0);
        if (same_state) {
            return false;
        }

        append_ref(
            dense_index,
            RevisionRef{
                0,
                target_present ? append_value(revision_value_ptr(target->value_index)) : npos32,
                pack_trace_tag(trace_context),
                false,
                !target_present,
                false});

        if (target_present) {
            std::memcpy(element_ptr(dense_index), revision_value_ptr(last_committed_ref(dense_index)->value_index), component_size_);
            if (!current_present) {
                ++committed_count_;
            }
        } else {
            std::memset(element_ptr(dense_index), 0, component_size_);
            if (current_present) {
                --committed_count_;
            }
        }
        return true;
    }

    void compact_trace_history(std::uint32_t current_time, std::uint32_t max_history) {
        if (mode_ != ComponentStorageMode::trace) {
            return;
        }

        const std::uint32_t cutoff = current_time > max_history ? current_time - max_history : 0;
        std::vector<RevisionOverflow> new_overflow;
        std::vector<unsigned char> new_values;

        for (std::size_t dense_index = 0; dense_index < revision_headers_.size(); ++dense_index) {
            const Entity entity = dense_entities_[dense_index];
            if (entity == null_entity) {
                revision_headers_[dense_index] = RevisionInfo{};
                continue;
            }

            std::vector<RevisionRef> refs;
            for_each_ref(dense_index, [&](const RevisionRef& ref) {
                if (!ref.voided && !ref.isolated) {
                    refs.push_back(ref);
                }
                return false;
            });

            if (refs.empty()) {
                clear_row(dense_index);
                continue;
            }

            std::size_t keep_from = 0;
            bool found_baseline = false;
            for (std::size_t i = 0; i < refs.size(); ++i) {
                const TraceCommitContext context = unpack_trace_context(refs[i].trace_tag);
                if (context.timestamp >= cutoff) {
                    keep_from = i;
                    if (i > 0 &&
                        context.timestamp > cutoff &&
                        unpack_trace_context(refs[i - 1].trace_tag).timestamp < cutoff) {
                        keep_from = i - 1;
                    }
                    found_baseline = true;
                    break;
                }
            }

            if (!found_baseline && refs.size() > 1) {
                keep_from = refs.size() - 1;
            }

            RevisionInfo rebuilt{};
            for (std::size_t i = keep_from; i < refs.size(); ++i) {
                RevisionRef kept = refs[i];
                if (!kept.tombstone) {
                    kept.value_index = append_compacted_value(new_values, revision_value_ptr(kept.value_index));
                }
                append_ref_to(rebuilt, new_overflow, kept);
            }

            revision_headers_[dense_index] = rebuilt;
            recompute_dense_head(entity, dense_index);
        }

        revision_overflow_ = std::move(new_overflow);
        revision_values_ = std::move(new_values);
    }

    const void* revision_value_ptr(std::uint32_t value_index) const {
        return revision_values_.data() + static_cast<std::size_t>(value_index) * component_size_;
    }

    void* revision_value_ptr(std::uint32_t value_index) {
        return revision_values_.data() + static_cast<std::size_t>(value_index) * component_size_;
    }

    std::size_t committed_size() const {
        return committed_count_;
    }

    void reset_indexes_after_clear() {
        index_.clear();
    }

    void increment_committed_count() {
        ++committed_count_;
    }

    const void* pending_previous_ptr(const PendingWrite& pending) const {
        if (mode_ != ComponentStorageMode::classic || !pending.had_value_before || pending.value_index == npos32) {
            return nullptr;
        }
        return revision_value_ptr(pending.value_index);
    }

private:
    friend class Registry;

    static bool contains_tsn(const std::vector<std::uint64_t>& tsns, std::uint64_t tsn) {
        for (const std::uint64_t candidate : tsns) {
            if (candidate == tsn) {
                return true;
            }
        }
        return false;
    }

    template <typename Func>
    void for_each_ref(std::size_t dense_index, Func&& func) const {
        const RevisionInfo& header = revision_headers_[dense_index];
        std::uint32_t remaining = header.item_count;
        const std::uint32_t inline_count = remaining > inline_revision_capacity
            ? static_cast<std::uint32_t>(inline_revision_capacity)
            : remaining;
        for (std::uint32_t i = 0; i < inline_count; ++i) {
            if (func(header.revisions[i])) {
                return;
            }
        }

        remaining -= inline_count;
        std::uint32_t overflow_index = header.overflow_index;
        while (remaining > 0 && overflow_index != npos32) {
            const RevisionOverflow& overflow = revision_overflow_[overflow_index];
            const std::uint32_t take = remaining > overflow.item_count ? overflow.item_count : remaining;
            for (std::uint32_t i = 0; i < take; ++i) {
                if (func(overflow.revisions[i])) {
                    return;
                }
            }
            remaining -= take;
            overflow_index = overflow.next_overflow;
        }
    }

    static std::uint32_t pack_trace_tag(TraceCommitContext context) {
        if (context.timestamp > max_trace_timestamp) {
            throw std::out_of_range("trace timestamp exceeds 22-bit range");
        }
        if (context.writer_id > max_trace_writer_id) {
            throw std::out_of_range("trace writer id exceeds 10-bit range");
        }
        return (context.timestamp << trace_writer_id_bits) | static_cast<std::uint32_t>(context.writer_id);
    }

    static TraceCommitContext unpack_trace_context(std::uint32_t trace_tag) {
        return TraceCommitContext{
            trace_tag >> trace_writer_id_bits,
            static_cast<std::uint16_t>(trace_tag & max_trace_writer_id),
        };
    }

    static TraceChangeInfo unpack_trace_change(const RevisionRef& ref) {
        const TraceCommitContext context = unpack_trace_context(ref.trace_tag);
        return TraceChangeInfo{context.timestamp, context.writer_id, ref.tombstone};
    }

    template <typename Func>
    void for_each_ref_mut(std::size_t dense_index, Func&& func) {
        RevisionInfo& header = revision_headers_[dense_index];
        std::uint32_t remaining = header.item_count;
        const std::uint32_t inline_count = remaining > inline_revision_capacity
            ? static_cast<std::uint32_t>(inline_revision_capacity)
            : remaining;
        for (std::uint32_t i = 0; i < inline_count; ++i) {
            if (func(header.revisions[i])) {
                return;
            }
        }

        remaining -= inline_count;
        std::uint32_t overflow_index = header.overflow_index;
        while (remaining > 0 && overflow_index != npos32) {
            RevisionOverflow& overflow = revision_overflow_[overflow_index];
            const std::uint32_t take = remaining > overflow.item_count ? overflow.item_count : remaining;
            for (std::uint32_t i = 0; i < take; ++i) {
                if (func(overflow.revisions[i])) {
                    return;
                }
            }
            remaining -= take;
            overflow_index = overflow.next_overflow;
        }
    }

    std::uint32_t append_value(const void* value) {
        const std::size_t offset = revision_values_.size();
        const unsigned char* source = static_cast<const unsigned char*>(value);
        std::vector<unsigned char> stable_copy;

        if (source != nullptr) {
            const unsigned char* begin = revision_values_.data();
            const unsigned char* end = begin + revision_values_.size();
            if (source >= begin && source < end) {
                stable_copy.resize(component_size_);
                std::memcpy(stable_copy.data(), source, component_size_);
                source = stable_copy.data();
            }
        }

        revision_values_.resize(offset + component_size_);
        if (source == nullptr) {
            std::memset(revision_values_.data() + offset, 0, component_size_);
        } else {
            std::memcpy(revision_values_.data() + offset, source, component_size_);
        }
        return static_cast<std::uint32_t>(offset / component_size_);
    }

    std::uint32_t append_compacted_value(std::vector<unsigned char>& values, const void* value) const {
        const std::size_t offset = values.size();
        values.resize(offset + component_size_);
        std::memcpy(values.data() + offset, value, component_size_);
        return static_cast<std::uint32_t>(offset / component_size_);
    }

    void append_ref(std::size_t dense_index, const RevisionRef& ref) {
        RevisionInfo& header = revision_headers_[dense_index];
        append_ref_to(header, revision_overflow_, ref);
    }

    static void append_ref_to(
        RevisionInfo& header,
        std::vector<RevisionOverflow>& overflow_storage,
        const RevisionRef& ref) {

        if (header.item_count < inline_revision_capacity) {
            header.revisions[header.item_count] = ref;
            ++header.item_count;
            return;
        }

        std::uint32_t overflow_index = header.overflow_index;
        if (overflow_index == npos32) {
            overflow_index = static_cast<std::uint32_t>(overflow_storage.size());
            overflow_storage.push_back(RevisionOverflow{});
            header.overflow_index = overflow_index;
        }

        while (true) {
            RevisionOverflow& overflow = overflow_storage[overflow_index];
            if (overflow.item_count < overflow_revision_capacity) {
                overflow.revisions[overflow.item_count] = ref;
                ++overflow.item_count;
                ++header.item_count;
                return;
            }
            if (overflow.next_overflow == npos32) {
                const std::uint32_t next_index = static_cast<std::uint32_t>(overflow_storage.size());
                overflow.next_overflow = next_index;
                overflow_storage.push_back(RevisionOverflow{});
                overflow_index = next_index;
                continue;
            }
            overflow_index = overflow.next_overflow;
        }
    }

    std::size_t assure_row(Entity entity) {
        const std::size_t existing = dense_index_of(entity);
        if (existing != npos) {
            return existing;
        }

        std::size_t dense_index = npos;
        if (!free_rows_.empty()) {
            dense_index = free_rows_.back();
            free_rows_.pop_back();
            dense_entities_[dense_index] = entity;
            revision_headers_[dense_index] = RevisionInfo{};
            std::memset(element_ptr(dense_index), 0, component_size_);
        } else {
            dense_index = dense_entities_.size();
            ensure_dense_capacity(dense_index + 1);
            dense_entities_.push_back(entity);
            revision_headers_.push_back(RevisionInfo{});
            std::memset(element_ptr(dense_index), 0, component_size_);
        }

        index_.set_index(entity, dense_index);
        return dense_index;
    }

    void clear_row(std::size_t dense_index) {
        const Entity entity = dense_entities_[dense_index];
        if (entity != null_entity) {
            index_.clear_index(entity);
        }
        dense_entities_[dense_index] = null_entity;
        revision_headers_[dense_index] = RevisionInfo{};
        std::memset(element_ptr(dense_index), 0, component_size_);
        free_rows_.push_back(dense_index);
    }

    void recompute_dense_head(Entity entity, std::size_t dense_index) {
        const RevisionRef* newest_non_void = nullptr;
        for_each_ref(dense_index, [&](const RevisionRef& ref) {
            if (!ref.voided) {
                newest_non_void = &ref;
            }
            return false;
        });

        if (newest_non_void == nullptr) {
            clear_row(dense_index);
            return;
        }

        if (!newest_non_void->tombstone) {
            std::memcpy(element_ptr(dense_index), revision_value_ptr(newest_non_void->value_index), component_size_);
        } else {
            std::memset(element_ptr(dense_index), 0, component_size_);
            if (mode_ != ComponentStorageMode::trace) {
                clear_row(dense_index);
                return;
            }
        }

        if (dense_entities_[dense_index] == null_entity) {
            dense_entities_[dense_index] = entity;
            index_.set_index(entity, dense_index);
        }
    }

    unsigned char* element_ptr(std::size_t index) {
        return dense_data_ + (index * component_size_);
    }

    const unsigned char* element_ptr(std::size_t index) const {
        return dense_data_ + (index * component_size_);
    }

    void ensure_dense_capacity(std::size_t required) {
        if (required <= dense_capacity_) {
            return;
        }

        std::size_t new_capacity = dense_capacity_ == 0 ? 1 : dense_capacity_;
        while (new_capacity < required) {
            new_capacity *= 2;
        }

        unsigned char* new_data =
            static_cast<unsigned char*>(::operator new[](new_capacity * component_size_, std::align_val_t(component_alignment_)));

        if (dense_data_ != nullptr) {
            std::memcpy(new_data, dense_data_, dense_entities_.size() * component_size_);
            ::operator delete[](dense_data_, std::align_val_t(component_alignment_));
        }

        dense_data_ = new_data;
        dense_capacity_ = new_capacity;
    }

    void release_storage() {
        if (dense_data_ != nullptr) {
            ::operator delete[](dense_data_, std::align_val_t(component_alignment_));
            dense_data_ = nullptr;
        }
        dense_capacity_ = 0;
    }

    std::size_t component_size_;
    std::size_t component_alignment_;
    ComponentStorageMode mode_;
    PagedSparseSet index_;
    std::vector<Entity> dense_entities_;
    unsigned char* dense_data_ = nullptr;
    std::size_t dense_capacity_ = 0;
    std::size_t committed_count_ = 0;
    std::vector<RevisionInfo> revision_headers_;
    std::vector<RevisionOverflow> revision_overflow_;
    std::vector<unsigned char> revision_values_;
    std::vector<std::size_t> free_rows_;
};

template <typename T>
class ComponentStorage final : public RawPagedSparseArray {
public:
    explicit ComponentStorage(ComponentStorageMode mode = ComponentStorageMode::mvcc, std::size_t page_size = 1024)
        : RawPagedSparseArray(sizeof(T), alignof(T), mode, page_size) {}

    using PendingWrite = RawPagedSparseArray::PendingWrite;

    const T* try_get(Entity entity) const {
        return static_cast<const T*>(this->try_get_raw(entity));
    }

    const T* try_get_visible(
        Entity entity,
        std::uint64_t max_visible_tsn,
        const std::vector<std::uint64_t>& active_at_open,
        std::uint64_t own_tsn) const {

        return static_cast<const T*>(this->try_get_visible_raw(entity, max_visible_tsn, active_at_open, own_tsn));
    }

    PendingWrite stage_write(
        Entity entity,
        std::uint64_t tsn,
        const std::vector<std::uint64_t>& active_at_open,
        std::uint64_t max_visible_tsn) {

        constexpr bool preserve_previous_value = std::tuple_size_v<typename ComponentIndices<T>::type> != 0;
        if (const T* existing = static_cast<const T*>(this->try_get_visible_raw(entity, max_visible_tsn, active_at_open, tsn))) {
            return RawPagedSparseArray::stage_write(entity, tsn, existing, true, preserve_previous_value);
        }
        return RawPagedSparseArray::stage_write(entity, tsn, nullptr, false, preserve_previous_value);
    }

    PendingWrite stage_value(Entity entity, std::uint64_t tsn, const T& value) {
        constexpr bool preserve_previous_value = std::tuple_size_v<typename ComponentIndices<T>::type> != 0;
        return RawPagedSparseArray::stage_write(entity, tsn, &value, true, preserve_previous_value);
    }

    T* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) {
        return static_cast<T*>(const_cast<void*>(RawPagedSparseArray::staged_ptr(pending, tsn)));
    }

    const T* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) const {
        return static_cast<const T*>(RawPagedSparseArray::staged_ptr(pending, tsn));
    }

    bool commit_staged(
        Entity entity,
        const PendingWrite& pending,
        std::uint64_t tsn,
        TraceCommitContext trace_context = {}) {

        if (this->storage_mode() == ComponentStorageMode::classic) {
            const T* staged = staged_ptr(pending, tsn);
            if (staged == nullptr) {
                return false;
            }

            if (pending.had_value_before) {
                const T* previous = static_cast<const T*>(this->pending_previous_ptr(pending));
                indexes_.replace(entity, *previous, *staged);
                return false;
            }

            indexes_.insert(entity, *staged);
            return true;
        }

        const T* previous = try_get(entity);
        const bool had_committed = previous != nullptr;
        const T previous_value = had_committed ? *previous : T{};
        const T* staged = staged_ptr(pending, tsn);
        if (staged == nullptr) {
            return false;
        }

        if (!RawPagedSparseArray::commit_staged_ref(entity, pending, tsn, trace_context)) {
            return false;
        }

        if (had_committed) {
            indexes_.replace(entity, previous_value, *staged);
        } else {
            indexes_.insert(entity, *staged);
            this->increment_committed_count();
        }
        return !had_committed;
    }

    void rollback_staged(Entity entity, const PendingWrite& pending, std::uint64_t tsn) {
        RawPagedSparseArray::rollback_staged(entity, pending, tsn);
    }

    bool erase(Entity entity, TraceCommitContext trace_context = {}) override {
        const T* existing = try_get(entity);
        if (existing == nullptr) {
            return false;
        }

        indexes_.erase(entity, *existing);
        return RawPagedSparseArray::erase(entity, trace_context);
    }

    void clear() override {
        indexes_.clear();
        RawPagedSparseArray::clear();
    }

    void rebuild_indexes() {
        indexes_.clear();
        const auto& dense_entities = this->entities();
        for (std::size_t i = 0; i < dense_entities.size(); ++i) {
            if (dense_entities[i] == null_entity) {
                continue;
            }
            if (const T* committed = try_get(dense_entities[i])) {
                indexes_.insert(dense_entities[i], *committed);
            }
        }
    }

    template <auto... Members, typename... KeyParts>
    std::vector<Entity> find_all(KeyParts&&... key_parts) const {
        using query_spec = detail::ComponentIndexSpec<false, Members...>;
        using index_spec = typename detail::tuple_member_pack_index_spec<typename ComponentIndices<T>::type, Members...>::type;
        static_assert(std::is_same_v<typename query_spec::component_type, T>, "find field must belong to the storage component");

        const auto key = detail::make_index_key<query_spec>(std::forward<KeyParts>(key_parts)...);
        if constexpr (!std::is_void_v<index_spec>) {
            return find_index<index_spec>(key);
        } else {
            std::vector<Entity> matches;
            for (const Entity entity : this->entities()) {
                if (entity == null_entity) {
                    continue;
                }
                if (const T* component = try_get(entity); component != nullptr && query_spec::key(*component) == key) {
                    matches.push_back(entity);
                }
            }
            return matches;
        }
    }

    template <auto Member, typename Value>
    std::vector<Entity> find_compare(PredicateOperator op, Value&& value) const {
        using index_spec = typename detail::tuple_member_index_spec<Member, typename ComponentIndices<T>::type>::type;
        using member_traits = detail::member_pointer_traits<decltype(Member)>;
        using stored_type = typename member_traits::member_type;
        static_assert(std::is_same_v<typename member_traits::component_type, T>, "find field must belong to the storage component");

        const stored_type key(std::forward<Value>(value));
        if constexpr (!std::is_void_v<index_spec>) {
            return find_compare_index<index_spec>(op, key);
        } else {
            std::vector<Entity> matches;
            for (const Entity entity : this->entities()) {
                if (entity == null_entity) {
                    continue;
                }
                if (const T* component = try_get(entity)) {
                    const auto& field = component->*Member;
                    bool matched = false;
                    switch (op) {
                        case PredicateOperator::eq:
                            matched = field == key;
                            break;
                        case PredicateOperator::ne:
                            matched = field != key;
                            break;
                        case PredicateOperator::gt:
                            matched = field > key;
                            break;
                        case PredicateOperator::gte:
                            matched = field >= key;
                            break;
                        case PredicateOperator::lt:
                            matched = field < key;
                            break;
                        case PredicateOperator::lte:
                            matched = field <= key;
                            break;
                    }
                    if (matched) {
                        matches.push_back(entity);
                    }
                }
            }
            return matches;
        }
    }

    template <auto... Members, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        const std::vector<Entity> matches = find_all<Members...>(std::forward<KeyParts>(key_parts)...);
        return matches.empty() ? null_entity : matches.front();
    }

    template <typename Func>
    void for_each_trace_change(Entity entity, Func&& func) const {
        this->RawPagedSparseArray::for_each_trace_change(entity, [&](const TraceChangeInfo& info, const void* value) {
            func(info, static_cast<const T*>(value));
        });
    }

    bool rollback_to_trace_timestamp(Entity entity, std::uint32_t timestamp, TraceCommitContext trace_context) {
        const T* previous = try_get(entity);
        const bool had_previous = previous != nullptr;
        const T previous_value = had_previous ? *previous : T{};

        if (!RawPagedSparseArray::rollback_to_trace_timestamp(entity, timestamp, trace_context)) {
            return false;
        }

        const T* current = try_get(entity);
        if (had_previous && current != nullptr) {
            indexes_.replace(entity, previous_value, *current);
        } else if (had_previous) {
            indexes_.erase(entity, previous_value);
        } else if (current != nullptr) {
            indexes_.insert(entity, *current);
        }
        return true;
    }

    void compact_trace_history(std::uint32_t current_time, std::uint32_t max_history) {
        RawPagedSparseArray::compact_trace_history(current_time, max_history);
    }

private:
    template <typename, typename>
    friend class TransactionStorageView;

    template <typename IndexSpec>
    std::vector<Entity> find_index(const typename IndexSpec::key_type& key) const {
        return indexes_.template find<IndexSpec>(key);
    }

    template <typename IndexSpec>
    std::vector<Entity> find_compare_index(PredicateOperator op, const typename IndexSpec::key_type& key) const {
        return indexes_.template find_compare<IndexSpec>(op, key);
    }

    template <typename IndexSpec>
    Entity find_one_index(const typename IndexSpec::key_type& key) const {
        return indexes_.template find_one<IndexSpec>(key);
    }

    detail::IndexSet<T, typename ComponentIndices<T>::type> indexes_;
};

template <typename T>
class PagedSparseArrayView {
public:
    explicit PagedSparseArrayView(RawPagedSparseArray* raw = nullptr)
        : raw_(raw) {}

    const T* try_get(Entity entity) const {
        return raw_ == nullptr ? nullptr : static_cast<const T*>(raw_->try_get_raw(entity));
    }

    const T& get(Entity entity) const {
        return *static_cast<const T*>(raw_->get_raw(entity));
    }

    bool contains(Entity entity) const {
        return raw_ != nullptr && raw_->contains(entity);
    }

    std::size_t size() const {
        return raw_ == nullptr ? 0 : raw_->size();
    }

    void rebuild_indexes() {
        if (raw_ != nullptr) {
            typed_raw()->rebuild_indexes();
        }
    }

    template <auto... Members, typename... KeyParts>
    std::vector<Entity> find_all(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find_all<Members...>(std::forward<KeyParts>(key_parts)...);
    }

    template <auto Member, typename Value>
    std::vector<Entity> find_compare(PredicateOperator op, Value&& value) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find_compare<Member>(op, std::forward<Value>(value));
    }

    template <auto... Members, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return null_entity;
        }

        return typed_raw()->template find_one<Members...>(std::forward<KeyParts>(key_parts)...);
    }

    const std::vector<Entity>& entities() const {
        static const std::vector<Entity> empty;
        return raw_ == nullptr ? empty : raw_->entities();
    }

    template <typename Func>
    void each(Func&& func) const {
        if (raw_ == nullptr) {
            return;
        }

        for (const Entity entity : raw_->entities()) {
            if (entity == null_entity) {
                continue;
            }
            if (const T* component = try_get(entity)) {
                func(entity, *component);
            }
        }
    }

private:
    ComponentStorage<T>* typed_raw() const {
        return static_cast<ComponentStorage<T>*>(raw_);
    }

    RawPagedSparseArray* raw_;
};

template <typename T>
class ConstPagedSparseArrayView {
public:
    explicit ConstPagedSparseArrayView(const RawPagedSparseArray* raw = nullptr)
        : raw_(raw) {}

    const T* try_get(Entity entity) const {
        return raw_ == nullptr ? nullptr : static_cast<const T*>(raw_->try_get_raw(entity));
    }

    const T& get(Entity entity) const {
        return *static_cast<const T*>(raw_->get_raw(entity));
    }

    bool contains(Entity entity) const {
        return raw_ != nullptr && raw_->contains(entity);
    }

    std::size_t size() const {
        return raw_ == nullptr ? 0 : raw_->size();
    }

    template <auto... Members, typename... KeyParts>
    std::vector<Entity> find_all(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find_all<Members...>(std::forward<KeyParts>(key_parts)...);
    }

    template <auto Member, typename Value>
    std::vector<Entity> find_compare(PredicateOperator op, Value&& value) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find_compare<Member>(op, std::forward<Value>(value));
    }

    template <auto... Members, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return null_entity;
        }

        return typed_raw()->template find_one<Members...>(std::forward<KeyParts>(key_parts)...);
    }

    const std::vector<Entity>& entities() const {
        static const std::vector<Entity> empty;
        return raw_ == nullptr ? empty : raw_->entities();
    }

    template <typename Func>
    void each(Func&& func) const {
        if (raw_ == nullptr) {
            return;
        }

        for (const Entity entity : raw_->entities()) {
            if (entity == null_entity) {
                continue;
            }
            if (const T* component = try_get(entity)) {
                func(entity, *component);
            }
        }
    }

private:
    const ComponentStorage<T>* typed_raw() const {
        return static_cast<const ComponentStorage<T>*>(raw_);
    }

    const RawPagedSparseArray* raw_;
};

}  // namespace ecs
