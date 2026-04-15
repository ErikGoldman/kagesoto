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

class RawPagedSparseArray {
public:
    static constexpr std::size_t inline_revision_capacity = 3;
    static constexpr std::size_t overflow_revision_capacity = 5;
    static constexpr std::uint32_t npos32 = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    struct RevisionRef {
        std::uint64_t tsn = 0;
        std::uint32_t value_index = npos32;
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
    };

    struct VisibleRef {
        const RevisionRef* ref = nullptr;
        bool found = false;
        bool tombstone = false;
    };

    RawPagedSparseArray(std::size_t component_size, std::size_t component_alignment, std::size_t page_size = 1024)
        : component_size_(component_size),
          component_alignment_(component_alignment),
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

    const void* try_get_raw(Entity entity) const {
        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return nullptr;
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

        const VisibleRef visible = visible_ref(dense_index, max_visible_tsn, active_at_open, own_tsn);
        return !visible.found || visible.tombstone ? nullptr : revision_value_ptr(visible.ref->value_index);
    }

    virtual bool erase(Entity entity) {
        const std::size_t dense_index = dense_index_of(entity);
        if (dense_index == npos) {
            return false;
        }

        const RevisionRef* committed = last_committed_ref(dense_index);
        if (committed == nullptr) {
            return false;
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

    PendingWrite stage_write(Entity entity, std::uint64_t tsn, const void* initial_value, bool has_initial_value) {
        const std::size_t dense_index = assure_row(entity);
        if (find_isolated_ref(dense_index, tsn) != nullptr) {
            return PendingWrite{dense_index};
        }

        const std::uint32_t value_index = append_value(has_initial_value ? initial_value : nullptr);
        append_ref(dense_index, RevisionRef{tsn, value_index, true, false, false});
        std::memcpy(element_ptr(dense_index), revision_value_ptr(value_index), component_size_);
        return PendingWrite{dense_index};
    }

    const void* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) const {
        const RevisionRef* isolated = find_isolated_ref(pending.dense_index, tsn);
        return isolated == nullptr || isolated->tombstone ? nullptr : revision_value_ptr(isolated->value_index);
    }

    void rollback_staged(Entity entity, const PendingWrite& pending, std::uint64_t tsn) {
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

    bool commit_staged_ref(Entity entity, const PendingWrite& pending, std::uint64_t tsn) {
        RevisionRef* isolated = find_isolated_ref(pending.dense_index, tsn);
        if (isolated == nullptr) {
            return false;
        }

        append_ref(pending.dense_index, RevisionRef{tsn, isolated->value_index, false, isolated->tombstone, false});
        isolated->voided = true;
        recompute_dense_head(entity, pending.dense_index);
        return true;
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

private:
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
        revision_values_.resize(offset + component_size_);
        if (value == nullptr) {
            std::memset(revision_values_.data() + offset, 0, component_size_);
        } else {
            std::memcpy(revision_values_.data() + offset, value, component_size_);
        }
        return static_cast<std::uint32_t>(offset / component_size_);
    }

    void append_ref(std::size_t dense_index, const RevisionRef& ref) {
        RevisionInfo& header = revision_headers_[dense_index];
        if (header.item_count < inline_revision_capacity) {
            header.revisions[header.item_count] = ref;
            ++header.item_count;
            return;
        }

        std::uint32_t overflow_index = header.overflow_index;
        if (overflow_index == npos32) {
            overflow_index = static_cast<std::uint32_t>(revision_overflow_.size());
            revision_overflow_.push_back(RevisionOverflow{});
            header.overflow_index = overflow_index;
        }

        while (true) {
            RevisionOverflow& overflow = revision_overflow_[overflow_index];
            if (overflow.item_count < overflow_revision_capacity) {
                overflow.revisions[overflow.item_count] = ref;
                ++overflow.item_count;
                ++header.item_count;
                return;
            }
            if (overflow.next_overflow == npos32) {
                const std::uint32_t next_index = static_cast<std::uint32_t>(revision_overflow_.size());
                overflow.next_overflow = next_index;
                revision_overflow_.push_back(RevisionOverflow{});
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
    explicit ComponentStorage(std::size_t page_size = 1024)
        : RawPagedSparseArray(sizeof(T), alignof(T), page_size) {}

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

        if (const T* existing = static_cast<const T*>(this->try_get_visible_raw(entity, max_visible_tsn, active_at_open, tsn))) {
            return RawPagedSparseArray::stage_write(entity, tsn, existing, true);
        }
        return RawPagedSparseArray::stage_write(entity, tsn, nullptr, false);
    }

    PendingWrite stage_value(Entity entity, std::uint64_t tsn, const T& value) {
        return RawPagedSparseArray::stage_write(entity, tsn, &value, true);
    }

    T* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) {
        return static_cast<T*>(const_cast<void*>(RawPagedSparseArray::staged_ptr(pending, tsn)));
    }

    const T* staged_ptr(const PendingWrite& pending, std::uint64_t tsn) const {
        return static_cast<const T*>(RawPagedSparseArray::staged_ptr(pending, tsn));
    }

    bool commit_staged(Entity entity, const PendingWrite& pending, std::uint64_t tsn) {
        const T* previous = try_get(entity);
        const bool had_committed = previous != nullptr;
        const T previous_value = had_committed ? *previous : T{};
        const T* staged = staged_ptr(pending, tsn);
        if (staged == nullptr) {
            return false;
        }

        if (!RawPagedSparseArray::commit_staged_ref(entity, pending, tsn)) {
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

    bool erase(Entity entity) override {
        const T* existing = try_get(entity);
        if (existing == nullptr) {
            return false;
        }

        indexes_.erase(entity, *existing);
        return RawPagedSparseArray::erase(entity);
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

    template <typename IndexSpec>
    std::vector<Entity> find(const typename IndexSpec::key_type& key) const {
        return indexes_.template find<IndexSpec>(key);
    }

    template <typename IndexSpec>
    Entity find_one(const typename IndexSpec::key_type& key) const {
        return indexes_.template find_one<IndexSpec>(key);
    }

private:
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

    template <typename IndexSpec, typename... KeyParts>
    std::vector<Entity> find(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find<IndexSpec>(
            detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...));
    }

    template <typename IndexSpec, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return null_entity;
        }

        return typed_raw()->template find_one<IndexSpec>(
            detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...));
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

    template <typename IndexSpec, typename... KeyParts>
    std::vector<Entity> find(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return {};
        }

        return typed_raw()->template find<IndexSpec>(
            detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...));
    }

    template <typename IndexSpec, typename... KeyParts>
    Entity find_one(KeyParts&&... key_parts) const {
        if (raw_ == nullptr) {
            return null_entity;
        }

        return typed_raw()->template find_one<IndexSpec>(
            detail::make_index_key<IndexSpec>(std::forward<KeyParts>(key_parts)...));
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
