#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "component_index.hpp"
#include "profiler.hpp"
#include "sparse_set.hpp"

namespace ecs {

enum class ComponentTraceStorage : std::uint8_t {
    copy_on_write,
    preallocated,
};

struct TraceCommitContext {
    Timestamp timestamp = 0;
    std::uint16_t writer_id = 0;
    bool enabled = false;
};

struct TraceChangeInfo {
    Timestamp timestamp = 0;
    std::uint16_t writer_id = 0;
    bool tombstone = false;
};

class RawPagedSparseArray {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t npos32 = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint16_t max_trace_writer_id = std::numeric_limits<std::uint16_t>::max();

    struct PendingWrite {
        std::size_t dense_index = npos;
        std::uint32_t previous_value_index = npos32;
        bool had_value_before = false;
    };

    struct TraceRecord {
        Entity entity = null_entity;
        Timestamp timestamp = 0;
        std::uint16_t writer_id = 0;
        std::uint32_t value_index = npos32;
        bool tombstone = false;
    };

    struct PreallocatedTraceFrame {
        Timestamp timestamp = 0;
        bool valid = false;
        std::vector<Entity> dense_entities;
        std::vector<unsigned char> dense_data;
    };

    RawPagedSparseArray(std::size_t component_size,
                        std::size_t component_alignment,
                        ComponentTraceStorage trace_storage = ComponentTraceStorage::copy_on_write,
                        std::size_t page_size = 1024,
                        std::size_t preallocated_trace_capacity = 0)
        : component_size_(component_size),
          component_alignment_(component_alignment),
          trace_storage_(trace_storage),
          index_(page_size),
          preallocated_trace_capacity_(preallocated_trace_capacity) {
        if (component_size_ == 0) {
            throw std::invalid_argument("component size must be greater than zero");
        }
        if (component_alignment_ == 0 || (component_alignment_ & (component_alignment_ - 1)) != 0) {
            throw std::invalid_argument("component alignment must be a non-zero power of two");
        }
        if (trace_storage_ == ComponentTraceStorage::preallocated && preallocated_trace_capacity_ != 0) {
            preallocated_trace_frames_.resize(preallocated_trace_capacity_);
        }
    }

    virtual ~RawPagedSparseArray() {
        release_storage();
    }

    RawPagedSparseArray(RawPagedSparseArray&& other) noexcept
        : component_size_(other.component_size_),
          component_alignment_(other.component_alignment_),
          trace_storage_(other.trace_storage_),
          index_(std::move(other.index_)),
          dense_entities_(std::move(other.dense_entities_)),
          dense_data_(other.dense_data_),
          dense_capacity_(other.dense_capacity_),
          committed_count_(other.committed_count_),
          previous_values_(std::move(other.previous_values_)),
          trace_records_(std::move(other.trace_records_)),
          trace_values_(std::move(other.trace_values_)),
          preallocated_trace_frames_(std::move(other.preallocated_trace_frames_)),
          preallocated_trace_next_frame_(other.preallocated_trace_next_frame_),
          preallocated_trace_capacity_(other.preallocated_trace_capacity_),
          free_rows_(std::move(other.free_rows_)) {
        other.dense_data_ = nullptr;
        other.dense_capacity_ = 0;
        other.committed_count_ = 0;
        other.preallocated_trace_next_frame_ = 0;
        other.preallocated_trace_capacity_ = 0;
    }

    RawPagedSparseArray& operator=(RawPagedSparseArray&& other) noexcept {
        if (this != &other) {
            release_storage();
            component_size_ = other.component_size_;
            component_alignment_ = other.component_alignment_;
            trace_storage_ = other.trace_storage_;
            index_ = std::move(other.index_);
            dense_entities_ = std::move(other.dense_entities_);
            dense_data_ = other.dense_data_;
            dense_capacity_ = other.dense_capacity_;
            committed_count_ = other.committed_count_;
            previous_values_ = std::move(other.previous_values_);
            trace_records_ = std::move(other.trace_records_);
            trace_values_ = std::move(other.trace_values_);
            preallocated_trace_frames_ = std::move(other.preallocated_trace_frames_);
            preallocated_trace_next_frame_ = other.preallocated_trace_next_frame_;
            preallocated_trace_capacity_ = other.preallocated_trace_capacity_;
            free_rows_ = std::move(other.free_rows_);
            other.dense_data_ = nullptr;
            other.dense_capacity_ = 0;
            other.committed_count_ = 0;
            other.preallocated_trace_next_frame_ = 0;
            other.preallocated_trace_capacity_ = 0;
        }
        return *this;
    }

    RawPagedSparseArray(const RawPagedSparseArray&) = delete;
    RawPagedSparseArray& operator=(const RawPagedSparseArray&) = delete;

    bool contains(Entity entity) const {
        return try_get_raw(entity) != nullptr;
    }

    ComponentTraceStorage trace_storage() const {
        return trace_storage_;
    }

    static constexpr Timestamp max_trace_time() {
        return std::numeric_limits<Timestamp>::max();
    }

    static constexpr std::uint16_t max_trace_writer() {
        return max_trace_writer_id;
    }

    const void* try_get_raw(Entity entity) const {
        const std::size_t dense_index = dense_index_of(entity);
        return dense_index == npos ? nullptr : element_ptr(dense_index);
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
        Timestamp,
        const std::vector<Timestamp>&,
        Timestamp) const {
        return try_get_raw(entity);
    }

    const void* try_get_committed_visible_raw(Entity entity, Timestamp) const {
        return try_get_raw(entity);
    }

    virtual bool erase(Entity entity, TraceCommitContext trace_context = {}) {
        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return false;
        }

        if (trace_context.enabled && trace_storage_ == ComponentTraceStorage::copy_on_write) {
            append_trace_record(entity, trace_context, nullptr, true);
        }

        clear_row(dense_index);
        --committed_count_;
        return true;
    }

    virtual void clear() {
        release_storage();
        index_.clear();
        dense_entities_.clear();
        previous_values_.clear();
        trace_records_.clear();
        trace_values_.clear();
        reset_preallocated_trace_frames();
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

    void reserve_rows(std::size_t rows) {
        if (rows == 0) {
            return;
        }
        ensure_dense_capacity(rows);
        dense_entities_.reserve(rows);
        free_rows_.reserve(rows);
        ensure_preallocated_trace_frame_capacity();
    }

    void reserve_revision_values(std::size_t) {}
    void reserve_revision_overflow_nodes(std::size_t) {}

    const void* try_get_visible_dense_raw(
        std::size_t dense_index,
        Timestamp,
        const std::vector<Timestamp>&,
        Timestamp) const {
        if (dense_index >= dense_entities_.size() || dense_entities_[dense_index] == null_entity) {
            return nullptr;
        }
        return element_ptr(dense_index);
    }

    const void* try_get_committed_visible_dense_raw(std::size_t dense_index, Timestamp) const {
        if (dense_index >= dense_entities_.size() || dense_entities_[dense_index] == null_entity) {
            return nullptr;
        }
        return element_ptr(dense_index);
    }

    PendingWrite stage_write(Entity entity,
                             Timestamp,
                             const void* initial_value,
                             bool has_initial_value,
                             bool preserve_previous_value = false) {
        const std::size_t existing = dense_index_of(entity);
        const bool had_value_before = existing != npos;
        const std::size_t dense_index = had_value_before ? existing : assure_row(entity);
        if (!had_value_before) {
            ++committed_count_;
        }

        const std::uint32_t previous_value_index =
            had_value_before && preserve_previous_value ? append_previous_value(element_ptr(dense_index)) : npos32;
        if (has_initial_value) {
            std::memcpy(element_ptr(dense_index), initial_value, component_size_);
        } else if (!had_value_before) {
            std::memset(element_ptr(dense_index), 0, component_size_);
        }
        return PendingWrite{dense_index, previous_value_index, had_value_before};
    }

    const void* staged_ptr(const PendingWrite& pending, Timestamp) const {
        return pending.dense_index == npos ? nullptr : element_ptr(pending.dense_index);
    }

    void rollback_staged(Entity, const PendingWrite& pending, Timestamp) {
        if (pending.dense_index == npos || pending.dense_index >= dense_entities_.size()) {
            return;
        }
        if (pending.had_value_before) {
            if (pending.previous_value_index != npos32) {
                std::memcpy(element_ptr(pending.dense_index), previous_value_ptr(pending.previous_value_index), component_size_);
            }
            return;
        }
        clear_row(pending.dense_index);
        if (committed_count_ != 0) {
            --committed_count_;
        }
    }

    const void* pending_previous_ptr(const PendingWrite& pending) const {
        if (!pending.had_value_before || pending.previous_value_index == npos32) {
            return nullptr;
        }
        return previous_value_ptr(pending.previous_value_index);
    }

    void compact_trace_history(Timestamp current_time, Timestamp max_history) {
        if (trace_storage_ != ComponentTraceStorage::copy_on_write) {
            return;
        }

        const Timestamp cutoff = current_time > max_history ? current_time - max_history : 0;
        std::vector<TraceRecord> compacted;
        std::vector<unsigned char> compacted_values;
        compacted.reserve(trace_records_.size());

        for (std::size_t i = 0; i < trace_records_.size(); ++i) {
            const TraceRecord& record = trace_records_[i];
            bool keep = record.timestamp >= cutoff;
            if (!keep) {
                const bool has_later_for_entity = std::any_of(
                    trace_records_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                    trace_records_.end(),
                    [&](const TraceRecord& later) {
                        return later.entity == record.entity && later.timestamp >= cutoff;
                    });
                keep = has_later_for_entity;
            }
            if (!keep) {
                continue;
            }

            TraceRecord copied = record;
            if (!copied.tombstone) {
                copied.value_index = append_value_to(compacted_values, trace_value_ptr(record.value_index));
            }
            compacted.push_back(copied);
        }

        trace_records_ = std::move(compacted);
        trace_values_ = std::move(compacted_values);
    }

    void capture_preallocated_trace_frame(Timestamp timestamp) {
        if (trace_storage_ != ComponentTraceStorage::preallocated || preallocated_trace_capacity_ == 0) {
            return;
        }

        PreallocatedTraceFrame& slot = preallocated_trace_frames_[preallocated_trace_next_frame_];
        slot.timestamp = timestamp;
        slot.valid = true;
        slot.dense_entities = dense_entities_;

        const std::size_t byte_count = dense_entities_.size() * component_size_;
        const std::size_t capacity_bytes = dense_capacity_ * component_size_;
        if (slot.dense_data.size() < capacity_bytes) {
            slot.dense_data.resize(capacity_bytes);
        }
        if (byte_count != 0) {
            std::memcpy(slot.dense_data.data(), dense_data_, byte_count);
        }

        preallocated_trace_next_frame_ = (preallocated_trace_next_frame_ + 1) % preallocated_trace_capacity_;
    }

    void compact_preallocated_trace_history(Timestamp current_time, Timestamp max_history) {
        if (trace_storage_ != ComponentTraceStorage::preallocated) {
            return;
        }

        const Timestamp cutoff = current_time > max_history ? current_time - max_history : 0;
        for (PreallocatedTraceFrame& frame : preallocated_trace_frames_) {
            if (frame.valid && frame.timestamp < cutoff) {
                frame.valid = false;
                frame.timestamp = 0;
                frame.dense_entities.clear();
            }
        }
    }

protected:
    std::size_t dense_index_of(Entity entity) const {
        const std::size_t sparse = index_.sparse_index(entity);
        if (sparse == PagedSparseSet::npos || sparse >= dense_entities_.size()) {
            return npos;
        }
        return dense_entities_[sparse] == entity ? sparse : npos;
    }

    bool commit_staged_ref(Entity, const PendingWrite&, Timestamp, TraceCommitContext = {}) {
        return true;
    }

    void record_trace_commit(Entity entity, const PendingWrite& pending, TraceCommitContext trace_context) {
        if (!trace_context.enabled || trace_storage_ != ComponentTraceStorage::copy_on_write) {
            return;
        }
        append_trace_record(entity, trace_context, staged_ptr(pending, 0), false);
    }

    template <typename Func>
    void for_each_trace_change(Entity entity, Func&& func) const {
        if (trace_storage_ == ComponentTraceStorage::preallocated) {
            for_each_preallocated_trace_change(entity, std::forward<Func>(func));
            return;
        }

        for (const TraceRecord& record : trace_records_) {
            if (record.entity == entity) {
                func(
                    TraceChangeInfo{record.timestamp, record.writer_id, record.tombstone},
                    record.tombstone ? nullptr : trace_value_ptr(record.value_index));
            }
        }
    }

    bool rollback_to_trace_timestamp(Entity entity, Timestamp timestamp, TraceCommitContext trace_context) {
        if (trace_storage_ == ComponentTraceStorage::preallocated) {
            return rollback_to_preallocated_trace_timestamp(entity, timestamp);
        }

        const TraceRecord* target = nullptr;
        for (const TraceRecord& record : trace_records_) {
            if (record.entity == entity && record.timestamp <= timestamp) {
                target = &record;
            }
        }
        if (target == nullptr) {
            return false;
        }

        const std::size_t current_index = dense_index_of(entity);
        const bool current_present = current_index != npos;
        const bool target_present = !target->tombstone;
        if (current_present == target_present &&
            (!current_present ||
             std::memcmp(element_ptr(current_index), trace_value_ptr(target->value_index), component_size_) == 0)) {
            return false;
        }

        if (target_present) {
            const std::size_t dense_index = current_present ? current_index : assure_row(entity);
            std::memcpy(element_ptr(dense_index), trace_value_ptr(target->value_index), component_size_);
            if (!current_present) {
                ++committed_count_;
            }
        } else {
            clear_row(current_index);
            --committed_count_;
        }

        if (trace_context.enabled) {
            append_trace_record(
                entity,
                trace_context,
                target_present ? trace_value_ptr(target->value_index) : nullptr,
                !target_present);
        }
        return true;
    }

    void initialize_trace(TraceCommitContext trace_context) {
        if (trace_storage_ == ComponentTraceStorage::preallocated) {
            if (preallocated_trace_capacity_ == 0) {
                return;
            }
            capture_preallocated_trace_frame(trace_context.timestamp);
            return;
        }

        for (std::size_t dense_index = 0; dense_index < dense_entities_.size(); ++dense_index) {
            const Entity entity = dense_entities_[dense_index];
            if (entity == null_entity) {
                continue;
            }
            append_trace_record(entity, trace_context, element_ptr(dense_index), false);
        }
    }

private:
    friend class Registry;

    const void* previous_value_ptr(std::uint32_t value_index) const {
        return previous_values_.data() + static_cast<std::size_t>(value_index) * component_size_;
    }

    const void* trace_value_ptr(std::uint32_t value_index) const {
        return trace_values_.data() + static_cast<std::size_t>(value_index) * component_size_;
    }

    std::uint32_t append_previous_value(const void* value) {
        const std::size_t offset = previous_values_.size();
        previous_values_.resize(offset + component_size_);
        std::memcpy(previous_values_.data() + offset, value, component_size_);
        return static_cast<std::uint32_t>(offset / component_size_);
    }

    std::uint32_t append_trace_value(const void* value) {
        const unsigned char* source = static_cast<const unsigned char*>(value);
        std::vector<unsigned char> stable_copy;
        if (source != nullptr) {
            const unsigned char* begin = trace_values_.data();
            const unsigned char* end = begin + trace_values_.size();
            if (source >= begin && source < end) {
                stable_copy.resize(component_size_);
                std::memcpy(stable_copy.data(), source, component_size_);
                value = stable_copy.data();
            }
        }
        return append_value_to(trace_values_, value);
    }

    std::uint32_t append_value_to(std::vector<unsigned char>& values, const void* value) const {
        const std::size_t offset = values.size();
        values.resize(offset + component_size_);
        if (value == nullptr) {
            std::memset(values.data() + offset, 0, component_size_);
        } else {
            std::memcpy(values.data() + offset, value, component_size_);
        }
        return static_cast<std::uint32_t>(offset / component_size_);
    }

    void append_trace_record(Entity entity, TraceCommitContext trace_context, const void* value, bool tombstone) {
        trace_records_.push_back(TraceRecord{
            entity,
            trace_context.timestamp,
            trace_context.writer_id,
            tombstone ? npos32 : append_trace_value(value),
            tombstone,
        });
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
            std::memset(element_ptr(dense_index), 0, component_size_);
        } else {
            dense_index = dense_entities_.size();
            ensure_dense_capacity(dense_index + 1);
            dense_entities_.push_back(entity);
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
        std::memset(element_ptr(dense_index), 0, component_size_);
        free_rows_.push_back(dense_index);
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
        ensure_preallocated_trace_frame_capacity();
    }

    void release_storage() {
        if (dense_data_ != nullptr) {
            ::operator delete[](dense_data_, std::align_val_t(component_alignment_));
            dense_data_ = nullptr;
        }
        dense_capacity_ = 0;
    }

    void ensure_preallocated_trace_frame_capacity() {
        if (trace_storage_ != ComponentTraceStorage::preallocated) {
            return;
        }

        const std::size_t capacity_bytes = dense_capacity_ * component_size_;
        for (PreallocatedTraceFrame& frame : preallocated_trace_frames_) {
            frame.dense_entities.reserve(dense_capacity_);
            frame.dense_data.resize(capacity_bytes);
        }
    }

    void reset_preallocated_trace_frames() {
        if (trace_storage_ != ComponentTraceStorage::preallocated) {
            preallocated_trace_frames_.clear();
            preallocated_trace_next_frame_ = 0;
            return;
        }

        preallocated_trace_frames_.clear();
        preallocated_trace_frames_.resize(preallocated_trace_capacity_);
        preallocated_trace_next_frame_ = 0;
        ensure_preallocated_trace_frame_capacity();
    }

    const PreallocatedTraceFrame* find_preallocated_trace_frame(Timestamp timestamp) const {
        const PreallocatedTraceFrame* result = nullptr;
        for (const PreallocatedTraceFrame& frame : preallocated_trace_frames_) {
            if (frame.valid && frame.timestamp == timestamp) {
                result = &frame;
                break;
            }
        }
        return result;
    }

    std::size_t dense_index_in_frame(const PreallocatedTraceFrame& frame, Entity entity) const {
        for (std::size_t i = 0; i < frame.dense_entities.size(); ++i) {
            if (frame.dense_entities[i] == entity) {
                return i;
            }
        }
        return npos;
    }

    const unsigned char* frame_element_ptr(const PreallocatedTraceFrame& frame, std::size_t index) const {
        return frame.dense_data.data() + (index * component_size_);
    }

    template <typename Func>
    void for_each_preallocated_trace_change(Entity entity, Func&& func) const {
        std::vector<const PreallocatedTraceFrame*> ordered;
        ordered.reserve(preallocated_trace_frames_.size());
        for (const PreallocatedTraceFrame& frame : preallocated_trace_frames_) {
            if (frame.valid) {
                ordered.push_back(&frame);
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->timestamp < rhs->timestamp;
        });

        bool seen = false;
        bool last_tombstone = false;
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            const PreallocatedTraceFrame* frame = ordered[i];
            const std::size_t frame_index = dense_index_in_frame(*frame, entity);
            if (frame_index != npos) {
                seen = true;
                last_tombstone = false;
                func(TraceChangeInfo{frame->timestamp, 0, false}, frame_element_ptr(*frame, frame_index));
                continue;
            }

            bool appears_later = false;
            for (std::size_t next = i + 1; next < ordered.size(); ++next) {
                if (dense_index_in_frame(*ordered[next], entity) != npos) {
                    appears_later = true;
                    break;
                }
            }

            if ((seen || appears_later) && !last_tombstone) {
                last_tombstone = true;
                func(TraceChangeInfo{frame->timestamp, 0, true}, nullptr);
            }
        }
    }

    bool rollback_to_preallocated_trace_timestamp(Entity entity, Timestamp timestamp) {
        const PreallocatedTraceFrame* frame = find_preallocated_trace_frame(timestamp);
        if (frame == nullptr) {
            throw std::out_of_range("trace timestamp is not retained by preallocated trace storage");
        }

        const std::size_t current_index = dense_index_of(entity);
        const bool current_present = current_index != npos;
        const std::size_t frame_index = dense_index_in_frame(*frame, entity);
        const bool frame_present = frame_index != npos;
        if (!current_present && !frame_present) {
            return false;
        }

        if (current_present && frame_present &&
            std::memcmp(element_ptr(current_index), frame_element_ptr(*frame, frame_index), component_size_) == 0) {
            return false;
        }

        if (frame_present) {
            const std::size_t dense_index = current_present ? current_index : assure_row(entity);
            std::memcpy(element_ptr(dense_index), frame_element_ptr(*frame, frame_index), component_size_);
            if (!current_present) {
                ++committed_count_;
            }
        } else {
            clear_row(current_index);
            --committed_count_;
        }
        return true;
    }

    std::size_t component_size_;
    std::size_t component_alignment_;
    ComponentTraceStorage trace_storage_;
    PagedSparseSet index_;
    std::vector<Entity> dense_entities_;
    unsigned char* dense_data_ = nullptr;
    std::size_t dense_capacity_ = 0;
    std::size_t committed_count_ = 0;
    std::vector<unsigned char> previous_values_;
    std::vector<TraceRecord> trace_records_;
    std::vector<unsigned char> trace_values_;
    std::vector<PreallocatedTraceFrame> preallocated_trace_frames_;
    std::size_t preallocated_trace_next_frame_ = 0;
    std::size_t preallocated_trace_capacity_ = 0;
    std::vector<std::size_t> free_rows_;
};

template <typename T>
class ComponentStorage final : public RawPagedSparseArray {
public:
    explicit ComponentStorage(ComponentTraceStorage trace_storage = ComponentTraceStorage::copy_on_write,
                              std::size_t page_size = 1024,
                              std::size_t preallocated_trace_capacity = 0)
        : RawPagedSparseArray(sizeof(T), alignof(T), trace_storage, page_size, preallocated_trace_capacity) {}

    using PendingWrite = RawPagedSparseArray::PendingWrite;

    const T* try_get(Entity entity) const {
        return static_cast<const T*>(this->try_get_raw(entity));
    }

    const T* try_get_visible(
        Entity entity,
        Timestamp max_visible_tsn,
        const std::vector<Timestamp>& active_at_open,
        Timestamp own_tsn) const {
        return static_cast<const T*>(this->try_get_visible_raw(entity, max_visible_tsn, active_at_open, own_tsn));
    }

    PendingWrite stage_write(
        Entity entity,
        Timestamp tsn,
        const std::vector<Timestamp>& active_at_open,
        Timestamp max_visible_tsn) {
        constexpr bool preserve_previous_value = std::tuple_size_v<typename ComponentIndices<T>::type> != 0;
        if (const T* existing = static_cast<const T*>(this->try_get_visible_raw(entity, max_visible_tsn, active_at_open, tsn))) {
            return RawPagedSparseArray::stage_write(entity, tsn, existing, true, preserve_previous_value);
        }
        return RawPagedSparseArray::stage_write(entity, tsn, nullptr, false, preserve_previous_value);
    }

    PendingWrite stage_value(Entity entity, Timestamp tsn, const T& value) {
        constexpr bool preserve_previous_value = std::tuple_size_v<typename ComponentIndices<T>::type> != 0;
        return RawPagedSparseArray::stage_write(entity, tsn, &value, true, preserve_previous_value);
    }

    void reserve_rows(std::size_t rows) {
        RawPagedSparseArray::reserve_rows(rows);
    }

    void reserve_revision_values(std::size_t additional_values) {
        RawPagedSparseArray::reserve_revision_values(additional_values);
    }

    void reserve_revision_overflow_nodes(std::size_t additional_nodes) {
        RawPagedSparseArray::reserve_revision_overflow_nodes(additional_nodes);
    }

    T* staged_ptr(const PendingWrite& pending, Timestamp tsn) {
        return static_cast<T*>(const_cast<void*>(RawPagedSparseArray::staged_ptr(pending, tsn)));
    }

    const T* staged_ptr(const PendingWrite& pending, Timestamp tsn) const {
        return static_cast<const T*>(RawPagedSparseArray::staged_ptr(pending, tsn));
    }

    bool commit_staged(Entity entity, const PendingWrite& pending, Timestamp tsn, TraceCommitContext trace_context = {}) {
        const T* staged = staged_ptr(pending, tsn);
        if (staged == nullptr) {
            return false;
        }

        if (pending.had_value_before) {
            if constexpr (std::tuple_size_v<typename ComponentIndices<T>::type> != 0) {
                const T* previous = static_cast<const T*>(this->pending_previous_ptr(pending));
                indexes_.replace(entity, *previous, *staged);
            }
            this->record_trace_commit(entity, pending, trace_context);
            return false;
        }

        indexes_.insert(entity, *staged);
        this->record_trace_commit(entity, pending, trace_context);
        return true;
    }

    void rollback_staged(Entity entity, const PendingWrite& pending, Timestamp tsn) {
        if (pending.had_value_before) {
            if constexpr (std::tuple_size_v<typename ComponentIndices<T>::type> != 0) {
                const T* current = staged_ptr(pending, tsn);
                const T* previous = static_cast<const T*>(this->pending_previous_ptr(pending));
                if (current != nullptr && previous != nullptr) {
                    indexes_.replace(entity, *current, *previous);
                }
            }
        } else if (const T* current = staged_ptr(pending, tsn)) {
            indexes_.erase(entity, *current);
        }
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
        using query_spec = detail::ComponentIndexSpec<detail::default_index_backend_tag, false, Members...>;
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

    bool rollback_to_trace_timestamp(Entity entity, Timestamp timestamp, TraceCommitContext trace_context) {
        if (!RawPagedSparseArray::rollback_to_trace_timestamp(entity, timestamp, trace_context)) {
            return false;
        }

        rebuild_indexes();
        return true;
    }

    void compact_trace_history(Timestamp current_time, Timestamp max_history) {
        RawPagedSparseArray::compact_trace_history(current_time, max_history);
    }

    void capture_preallocated_trace_frame(Timestamp timestamp) {
        RawPagedSparseArray::capture_preallocated_trace_frame(timestamp);
    }

    void compact_preallocated_trace_history(Timestamp current_time, Timestamp max_history) {
        RawPagedSparseArray::compact_preallocated_trace_history(current_time, max_history);
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

}  // namespace ecs
