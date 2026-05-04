#include "ecs/ecs.hpp"

namespace ecs {

thread_local Registry::TypeErasedStorage::DeferredDirtyWrites*
    Registry::TypeErasedStorage::deferred_dirty_writes_ = nullptr;
thread_local bool Registry::TypeErasedStorage::defer_range_dirty_ = false;

Registry::TypeErasedStorage::TypeErasedStorage(const ComponentRecord& record)
    : info_(record.info), lifecycle_(record.lifecycle) {}

Registry::TypeErasedStorage::TypeErasedStorage(const TypeErasedStorage& other)
    : dense_indices_(other.dense_indices_),
      sparse_(other.sparse_),
      dirty_(other.dirty_),
      tombstones_(other.tombstone_count_ == 0 ? std::vector<unsigned char>{} : other.tombstones_),
      tombstone_indices_(other.tombstone_count_ == 0 ? std::vector<std::uint32_t>{} : other.tombstone_indices_),
      dirty_count_(other.dirty_count_),
      tombstone_count_(other.tombstone_count_),
      compact_lookup_(other.compact_lookup_),
      info_(other.info_),
      lifecycle_(other.lifecycle_) {
    copy_from(other);
}

Registry::TypeErasedStorage& Registry::TypeErasedStorage::operator=(const TypeErasedStorage& other) {
    if (this != &other) {
        TypeErasedStorage copy(other);
        *this = std::move(copy);
    }

    return *this;
}

Registry::TypeErasedStorage::TypeErasedStorage(TypeErasedStorage&& other) noexcept
    : dense_indices_(std::move(other.dense_indices_)),
      sparse_(std::move(other.sparse_)),
      dirty_(std::move(other.dirty_)),
      tombstones_(std::move(other.tombstones_)),
      tombstone_indices_(std::move(other.tombstone_indices_)),
      data_(other.data_),
      size_(other.size_),
      capacity_(other.capacity_),
      dirty_count_(other.dirty_count_),
      tombstone_count_(other.tombstone_count_),
      compact_lookup_(other.compact_lookup_),
      info_(other.info_),
      lifecycle_(other.lifecycle_),
      swap_scratch_(std::move(other.swap_scratch_)) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    other.dirty_count_ = 0;
    other.tombstone_count_ = 0;
    other.compact_lookup_ = false;
}

Registry::TypeErasedStorage& Registry::TypeErasedStorage::operator=(TypeErasedStorage&& other) noexcept {
    if (this != &other) {
        clear();
        deallocate(data_, info_.alignment);
        dense_indices_ = std::move(other.dense_indices_);
        sparse_ = std::move(other.sparse_);
        dirty_ = std::move(other.dirty_);
        tombstones_ = std::move(other.tombstones_);
        tombstone_indices_ = std::move(other.tombstone_indices_);
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        dirty_count_ = other.dirty_count_;
        tombstone_count_ = other.tombstone_count_;
        compact_lookup_ = other.compact_lookup_;
        info_ = other.info_;
        lifecycle_ = other.lifecycle_;
        swap_scratch_ = std::move(other.swap_scratch_);
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.dirty_count_ = 0;
        other.tombstone_count_ = 0;
        other.compact_lookup_ = false;
    }

    return *this;
}

Registry::TypeErasedStorage::~TypeErasedStorage() {
    clear();
    deallocate(data_, info_.alignment);
}

void Registry::TypeErasedStorage::emplace_or_replace_tag(std::uint32_t index) {
    if (!info_.tag) {
        throw std::logic_error("ecs component storage is not a tag");
    }

    if (contains(index)) {
        mark_dirty_dense(sparse_[index]);
        return;
    }

    ensure_sparse(index);
    clear_tombstone(index);

    const std::uint32_t dense = static_cast<std::uint32_t>(size_);
    dense_indices_.push_back(index);
    dirty_.push_back(1);
    ++dirty_count_;
    sparse_[index] = dense;
    ++size_;
}

void* Registry::TypeErasedStorage::emplace_or_replace_bytes(std::uint32_t index, const void* value) {
    if (info_.tag) {
        throw std::logic_error("ecs tags do not store component values");
    }
    if (!info_.trivially_copyable) {
        throw std::logic_error("runtime byte add requires a trivially copyable component");
    }

    if (void* existing = get(index)) {
        mark_dirty_dense(sparse_[index]);
        assign_bytes(existing, value);
        return existing;
    }

    ensure_sparse(index);
    clear_tombstone(index);
    ensure_capacity(size_ + 1);

    const std::uint32_t dense = static_cast<std::uint32_t>(size_);
    dense_indices_.push_back(index);
    dirty_.push_back(1);
    ++dirty_count_;
    sparse_[index] = dense;
    void* target = data_ + size_ * info_.size;
    assign_bytes(target, value);
    ++size_;
    return target;
}

void Registry::TypeErasedStorage::emplace_or_replace_copy(std::uint32_t index, const void* value) {
    if (info_.tag) {
        emplace_or_replace_tag(index);
        return;
    }
    if (void* existing = get(index)) {
        mark_dirty_dense(sparse_[index]);
        replace_copy(existing, value);
        return;
    }

    ensure_sparse(index);
    clear_tombstone(index);
    ensure_capacity(size_ + 1);

    const std::uint32_t dense = static_cast<std::uint32_t>(size_);
    dense_indices_.push_back(index);
    dirty_.push_back(1);
    ++dirty_count_;
    sparse_[index] = dense;
    construct_copy(data_ + size_ * info_.size, value);
    ++size_;
}

void* Registry::TypeErasedStorage::ensure(std::uint32_t index) {
    if (info_.tag) {
        throw std::logic_error("ecs tags do not store component values");
    }
    if (void* existing = write(index)) {
        return existing;
    }

    return emplace_or_replace_bytes(index, nullptr);
}

bool Registry::TypeErasedStorage::remove(std::uint32_t index) {
    if (!contains(index)) {
        return false;
    }

    erase_at(sparse_[index]);
    mark_tombstone(index, tombstone_dirty);
    return true;
}

void Registry::TypeErasedStorage::remove_index(std::uint32_t index) {
    (void)remove(index);
}

void Registry::TypeErasedStorage::remove_index_for_destroy(std::uint32_t index) {
    if (!contains(index)) {
        return;
    }

    erase_at(sparse_[index]);
    mark_tombstone(index, tombstone_dirty | tombstone_destroy_entity);
}

void* Registry::TypeErasedStorage::get(std::uint32_t index) {
    if (!contains(index)) {
        return nullptr;
    }
    if (info_.tag) {
        return nullptr;
    }

    return data_ + sparse_[index] * info_.size;
}

const void* Registry::TypeErasedStorage::get(std::uint32_t index) const {
    if (!contains(index)) {
        return nullptr;
    }
    if (info_.tag) {
        return nullptr;
    }

    return data_ + sparse_[index] * info_.size;
}

void* Registry::TypeErasedStorage::write(std::uint32_t index) {
    if (!contains(index)) {
        return nullptr;
    }

    const std::uint32_t dense = sparse_[index];
    mark_dirty_or_defer(index, dense);
    if (info_.tag) {
        return nullptr;
    }
    return data_ + dense * info_.size;
}

const void* Registry::TypeErasedStorage::get_unchecked(std::uint32_t index) const {
    assert(!info_.tag);
    assert(contains(index));
    return data_ + sparse_[index] * info_.size;
}

void* Registry::TypeErasedStorage::write_unchecked(std::uint32_t index) {
    assert(!info_.tag);
    assert(contains(index));
    const std::uint32_t dense = sparse_[index];
    mark_dirty_or_defer(index, dense);
    return data_ + dense * info_.size;
}

void* Registry::TypeErasedStorage::write_unchecked_without_dirty(std::uint32_t index) {
    assert(!info_.tag);
    assert(contains(index));
    return data_ + sparse_[index] * info_.size;
}

bool Registry::TypeErasedStorage::clear_dirty(std::uint32_t index) {
    if (!contains(index)) {
        if (has_tombstone(index)) {
            clear_tombstone(index);
            return true;
        }
        return false;
    }

    clear_dirty_dense(sparse_[index]);
    return true;
}

bool Registry::TypeErasedStorage::is_dirty(std::uint32_t index) const {
    if (!contains(index)) {
        return has_tombstone(index);
    }

    return dirty_[sparse_[index]] != 0;
}

void Registry::TypeErasedStorage::clear_all_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), std::uint8_t{0});
    std::fill(tombstones_.begin(), tombstones_.end(), no_tombstone);
    tombstone_indices_.clear();
    dirty_count_ = 0;
    tombstone_count_ = 0;
}

void Registry::TypeErasedStorage::mark_dirty(std::uint32_t index) {
    if (!contains(index)) {
        return;
    }
    mark_dirty_dense(sparse_[index]);
}

std::size_t Registry::TypeErasedStorage::dense_size() const noexcept {
    return size_;
}

std::uint32_t Registry::TypeErasedStorage::dense_index_at(std::size_t dense) const {
    return dense_indices_[dense];
}

std::uint32_t Registry::TypeErasedStorage::dense_position(std::uint32_t index) const {
    return contains(index) ? sparse_[index] : npos;
}

bool Registry::TypeErasedStorage::contains_index(std::uint32_t index) const {
    return contains(index);
}

bool Registry::TypeErasedStorage::has_dirty_entries() const {
    return dirty_count_ != 0 || tombstone_count_ != 0;
}

bool Registry::TypeErasedStorage::has_destroy_tombstone(std::uint32_t index) const {
    return has_tombstone(index) && (tombstones_[index] & tombstone_destroy_entity) != 0;
}

std::size_t Registry::TypeErasedStorage::tombstone_size() const noexcept {
    return tombstone_count_ == 0 ? 0 : tombstones_.size();
}

std::size_t Registry::TypeErasedStorage::tombstone_entry_count() const noexcept {
    return tombstone_indices_.size();
}

std::uint32_t Registry::TypeErasedStorage::tombstone_index_at(std::size_t position) const {
    return tombstone_indices_[position];
}

unsigned char Registry::TypeErasedStorage::tombstone_flags_at(std::size_t position) const {
    if (compact_lookup_) {
        return tombstones_[position];
    }
    return tombstones_[tombstone_indices_[position]];
}

bool Registry::TypeErasedStorage::has_dirty_tombstone_at(std::uint32_t index) const {
    return has_tombstone(index);
}

bool Registry::TypeErasedStorage::has_dirty_tombstone_at_position(std::size_t position) const {
    return (tombstone_flags_at(position) & tombstone_dirty) != 0;
}

bool Registry::TypeErasedStorage::has_destroy_tombstone_at_position(std::size_t position) const {
    return (tombstone_flags_at(position) & tombstone_destroy_entity) != 0;
}

const void* Registry::TypeErasedStorage::get_dense(std::size_t dense) const {
    if (info_.tag) {
        return nullptr;
    }
    return data_ + dense * info_.size;
}

void Registry::TypeErasedStorage::swap_dense(std::uint32_t lhs, std::uint32_t rhs) {
    if (lhs == rhs) {
        return;
    }
    if (lhs >= size_ || rhs >= size_) {
        throw std::out_of_range("ecs dense storage swap index is out of range");
    }

    if (!info_.tag) {
        unsigned char* lhs_value = data_ + lhs * info_.size;
        unsigned char* rhs_value = data_ + rhs * info_.size;

        if (info_.trivially_copyable) {
            std::array<unsigned char, 256> stack_temp;
            unsigned char* temp = stack_temp.data();
            if (info_.size > stack_temp.size()) {
                swap_scratch_.resize(info_.size);
                temp = swap_scratch_.data();
            }
            std::memcpy(temp, lhs_value, info_.size);
            std::memcpy(lhs_value, rhs_value, info_.size);
            std::memcpy(rhs_value, temp, info_.size);
        } else {
            unsigned char* temp = allocate(1, info_);
            try {
                lifecycle_.move_construct(temp, lhs_value);
                lifecycle_.destroy(lhs_value);
                lifecycle_.move_construct(lhs_value, rhs_value);
                lifecycle_.destroy(rhs_value);
                lifecycle_.move_construct(rhs_value, temp);
                lifecycle_.destroy(temp);
            } catch (...) {
                deallocate(temp, info_.alignment);
                throw;
            }
            deallocate(temp, info_.alignment);
        }
    }

    const std::uint32_t lhs_index = dense_indices_[lhs];
    const std::uint32_t rhs_index = dense_indices_[rhs];
    dense_indices_[lhs] = rhs_index;
    dense_indices_[rhs] = lhs_index;
    sparse_[lhs_index] = rhs;
    sparse_[rhs_index] = lhs;
    const unsigned char lhs_dirty = dirty_[lhs];
    dirty_[lhs] = dirty_[rhs];
    dirty_[rhs] = lhs_dirty;
}

void Registry::TypeErasedStorage::move_index_to_dense(std::uint32_t index, std::uint32_t dense) {
    const std::uint32_t current = dense_position(index);
    if (current == npos) {
        return;
    }
    swap_dense(current, dense);
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone() const {
    return std::make_unique<TypeErasedStorage>(*this);
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone_for_restore() const {
    auto copy = clone();
    copy->rebuild_lookup();
    return copy;
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone_dirty() const {
    auto copy = std::make_unique<TypeErasedStorage>(*this);
    for (std::size_t dense = copy->size_; dense > 0; --dense) {
        const std::uint32_t position = static_cast<std::uint32_t>(dense - 1);
        if (copy->dirty_[position] == 0) {
            copy->erase_at(position);
        }
    }
    return copy;
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone_excluding(const std::vector<bool>& excluded) const {
    return clone_compact_filtered(excluded, false);
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone_dirty_excluding(const std::vector<bool>& excluded) const {
    return clone_compact_filtered(excluded, true);
}

Registry::TypeErasedStorage::TypeErasedStorage(ComponentInfo info, ComponentLifecycle lifecycle)
    : info_(info), lifecycle_(lifecycle) {}

unsigned char* Registry::TypeErasedStorage::allocate(std::size_t capacity, const ComponentInfo& info) {
    if (capacity == 0 || info.tag) {
        return nullptr;
    }

    return static_cast<unsigned char*>(
        ::operator new(info.size * capacity, std::align_val_t{info.alignment}));
}

void Registry::TypeErasedStorage::deallocate(unsigned char* data, std::size_t alignment) noexcept {
    if (data != nullptr) {
        ::operator delete(data, std::align_val_t{alignment});
    }
}

void Registry::TypeErasedStorage::assign_bytes(void* target, const void* value) {
    if (info_.tag) {
        throw std::logic_error("ecs tags do not store component values");
    }
    if (value != nullptr) {
        std::memcpy(target, value, info_.size);
    } else {
        std::memset(target, 0, info_.size);
    }
}

void Registry::TypeErasedStorage::construct_copy(void* target, const void* value) {
    if (info_.tag) {
        return;
    }
    if (info_.trivially_copyable) {
        std::memcpy(target, value, info_.size);
        return;
    }
    if (lifecycle_.copy_construct == nullptr) {
        throw std::logic_error("ecs component storage is not copyable");
    }
    lifecycle_.copy_construct(target, value);
}

void Registry::TypeErasedStorage::replace_copy(void* target, const void* value) {
    if (info_.tag) {
        return;
    }
    if (info_.trivially_copyable) {
        std::memcpy(target, value, info_.size);
        return;
    }
    if (lifecycle_.copy_construct == nullptr) {
        throw std::logic_error("ecs component storage is not copyable");
    }
    unsigned char* replacement = allocate(1, info_);
    bool replacement_constructed = false;
    try {
        lifecycle_.copy_construct(replacement, value);
        replacement_constructed = true;
        lifecycle_.destroy(target);
        lifecycle_.move_construct(target, replacement);
        lifecycle_.destroy(replacement);
        replacement_constructed = false;
    } catch (...) {
        if (replacement_constructed) {
            lifecycle_.destroy(replacement);
        }
        deallocate(replacement, info_.alignment);
        throw;
    }
    deallocate(replacement, info_.alignment);
}

void Registry::TypeErasedStorage::ensure_sparse(std::uint32_t index) {
    if (index >= sparse_.size()) {
        sparse_.resize(static_cast<std::size_t>(index) + 1, npos);
    }
    if (index >= tombstones_.size()) {
        tombstones_.resize(static_cast<std::size_t>(index) + 1, no_tombstone);
    }
}

bool Registry::TypeErasedStorage::contains(std::uint32_t index) const {
    if (index >= sparse_.size()) {
        return false;
    }

    const std::uint32_t dense = sparse_[index];
    return dense != npos && dense < size_ && dense_indices_[dense] == index;
}

void Registry::TypeErasedStorage::ensure_capacity(std::size_t required) {
    if (info_.tag) {
        capacity_ = std::max(capacity_, required);
        return;
    }
    if (required <= capacity_) {
        return;
    }

    const std::size_t next_capacity = std::max<std::size_t>(required, capacity_ == 0 ? 8 : capacity_ * 2);
    unsigned char* next = allocate(next_capacity, info_);

    if (info_.trivially_copyable) {
        if (size_ != 0) {
            std::memcpy(next, data_, info_.size * size_);
        }
    } else {
        std::size_t constructed = 0;
        try {
            for (; constructed < size_; ++constructed) {
                lifecycle_.move_construct(
                    next + constructed * info_.size,
                    data_ + constructed * info_.size);
            }
        } catch (...) {
            for (std::size_t i = 0; i < constructed; ++i) {
                lifecycle_.destroy(next + i * info_.size);
            }
            deallocate(next, info_.alignment);
            throw;
        }

        for (std::size_t i = 0; i < size_; ++i) {
            lifecycle_.destroy(data_ + i * info_.size);
        }
    }

    deallocate(data_, info_.alignment);
    data_ = next;
    capacity_ = next_capacity;
}

void Registry::TypeErasedStorage::erase_at(std::uint32_t dense) {
    const std::uint32_t removed_index = dense_indices_[dense];
    const std::uint32_t last_dense = static_cast<std::uint32_t>(size_ - 1);
    clear_dirty_dense(dense);

    if (dense != last_dense) {
        const std::uint32_t moved_index = dense_indices_[last_dense];

        if (!info_.tag) {
            unsigned char* target = data_ + dense * info_.size;
            unsigned char* last = data_ + last_dense * info_.size;
            if (info_.trivially_copyable) {
                std::memcpy(target, last, info_.size);
            } else {
                lifecycle_.destroy(target);
                lifecycle_.move_construct(target, last);
                lifecycle_.destroy(last);
            }
        }

        dense_indices_[dense] = moved_index;
        dirty_[dense] = dirty_[last_dense];
        sparse_[moved_index] = dense;
    } else if (!info_.tag && !info_.trivially_copyable) {
        unsigned char* target = data_ + dense * info_.size;
        lifecycle_.destroy(target);
    }

    sparse_[removed_index] = npos;
    dense_indices_.pop_back();
    dirty_.pop_back();
    --size_;
}

void Registry::TypeErasedStorage::clear() noexcept {
    if (!info_.tag && !info_.trivially_copyable) {
        for (std::size_t i = 0; i < size_; ++i) {
            lifecycle_.destroy(data_ + i * info_.size);
        }
    }

    size_ = 0;
    dense_indices_.clear();
    dirty_.clear();
    std::fill(sparse_.begin(), sparse_.end(), npos);
    std::fill(tombstones_.begin(), tombstones_.end(), no_tombstone);
    tombstone_indices_.clear();
    dirty_count_ = 0;
    tombstone_count_ = 0;
}

void Registry::TypeErasedStorage::mark_tombstone(std::uint32_t index, unsigned char flags) {
    ensure_sparse(index);
    if (tombstones_[index] == no_tombstone && flags != no_tombstone) {
        ++tombstone_count_;
        tombstone_indices_.push_back(index);
    } else if (tombstones_[index] != no_tombstone && flags == no_tombstone) {
        --tombstone_count_;
        erase_tombstone_index(index);
    }
    tombstones_[index] = flags;
}

void Registry::TypeErasedStorage::clear_tombstone(std::uint32_t index) {
    if (index < tombstones_.size() && tombstones_[index] != no_tombstone) {
        tombstones_[index] = no_tombstone;
        erase_tombstone_index(index);
        --tombstone_count_;
    }
}

bool Registry::TypeErasedStorage::has_tombstone(std::uint32_t index) const {
    return index < tombstones_.size() && tombstones_[index] != no_tombstone;
}

Registry::TypeErasedStorage::DeferredDirtyScope::DeferredDirtyScope(
    DeferredDirtyWrites& writes,
    bool defer_range_dirty)
    : previous_writes_(deferred_dirty_writes_),
      previous_defer_range_dirty_(defer_range_dirty_) {
    deferred_dirty_writes_ = &writes;
    defer_range_dirty_ = defer_range_dirty;
}

Registry::TypeErasedStorage::DeferredDirtyScope::~DeferredDirtyScope() {
    deferred_dirty_writes_ = previous_writes_;
    defer_range_dirty_ = previous_defer_range_dirty_;
}

bool Registry::TypeErasedStorage::range_dirty_deferred() noexcept {
    return defer_range_dirty_;
}

void Registry::TypeErasedStorage::mark_dirty_dense(std::uint32_t dense) {
    if (dirty_[dense] == 0) {
        ++dirty_count_;
        dirty_[dense] = 1;
    }
}

void Registry::TypeErasedStorage::clear_dirty_dense(std::uint32_t dense) {
    if (dirty_[dense] != 0) {
        dirty_[dense] = 0;
        --dirty_count_;
    }
}

void Registry::TypeErasedStorage::mark_dirty_or_defer(std::uint32_t index, std::uint32_t dense) {
    if (deferred_dirty_writes_ != nullptr) {
        deferred_dirty_writes_->push_back(DeferredDirtyWrite{this, index});
        return;
    }
    mark_dirty_dense(dense);
}

void Registry::TypeErasedStorage::erase_tombstone_index(std::uint32_t index) {
    const auto found = std::find(tombstone_indices_.begin(), tombstone_indices_.end(), index);
    if (found != tombstone_indices_.end()) {
        tombstone_indices_.erase(found);
    }
}

std::unique_ptr<Registry::TypeErasedStorage> Registry::TypeErasedStorage::clone_compact_filtered(
    const std::vector<bool>& excluded,
    bool dirty_only) const {
    auto copy = std::unique_ptr<TypeErasedStorage>(new TypeErasedStorage(info_, lifecycle_));
    copy->compact_lookup_ = true;

    if (!dirty_only && !has_excluded_storage_entries(excluded)) {
        copy->dense_indices_ = dense_indices_;
        copy->dirty_ = dirty_;
        copy->capacity_ = size_;
        copy->dirty_count_ = dirty_count_;
        if (info_.tag) {
            copy->size_ = size_;
        } else if (copy->capacity_ != 0) {
            copy->data_ = allocate(copy->capacity_, info_);
            if (info_.trivially_copyable) {
                std::memcpy(copy->data_, data_, info_.size * size_);
                copy->size_ = size_;
            } else {
                if (lifecycle_.copy_construct == nullptr) {
                    throw std::logic_error("ecs component storage is not copyable");
                }
                for (; copy->size_ < size_; ++copy->size_) {
                    lifecycle_.copy_construct(
                        copy->data_ + copy->size_ * info_.size,
                        data_ + copy->size_ * info_.size);
                }
            }
        }
        copy_compact_tombstones_excluding(*copy, excluded);
        return copy;
    }

    const std::size_t capacity_hint = dirty_only ? dirty_count_ : size_;
    copy->dense_indices_.reserve(capacity_hint);
    copy->dirty_.reserve(capacity_hint);
    if (!info_.tag && capacity_hint != 0) {
        copy->capacity_ = capacity_hint;
        copy->data_ = allocate(copy->capacity_, info_);
    }

    for (std::size_t dense = 0; dense < size_; ++dense) {
        const std::uint32_t index = dense_indices_[dense];
        if ((index < excluded.size() && excluded[index]) || (dirty_only && dirty_[dense] == 0)) {
            continue;
        }

        if (!info_.tag) {
            const void* source = data_ + dense * info_.size;
            void* target = copy->data_ + copy->size_ * info_.size;
            copy->construct_copy(target, source);
        }

        copy->dense_indices_.push_back(index);
        copy->dirty_.push_back(dirty_[dense]);
        if (dirty_[dense] != 0) {
            ++copy->dirty_count_;
        }
        ++copy->size_;
    }

    copy_compact_tombstones_excluding(*copy, excluded);
    return copy;
}

bool Registry::TypeErasedStorage::has_excluded_storage_entries(const std::vector<bool>& excluded) const {
    if (excluded.empty()) {
        return false;
    }
    for (std::uint32_t index : dense_indices_) {
        if (index < excluded.size() && excluded[index]) {
            return true;
        }
    }
    for (std::uint32_t index : tombstone_indices_) {
        if (index < excluded.size() && excluded[index]) {
            return true;
        }
    }
    return false;
}

void Registry::TypeErasedStorage::copy_compact_tombstones_excluding(
    TypeErasedStorage& copy,
    const std::vector<bool>& excluded) const {
    if (tombstone_count_ == 0) {
        return;
    }

    copy.tombstone_indices_.reserve(tombstone_indices_.size());
    copy.tombstones_.reserve(tombstone_indices_.size());
    for (std::uint32_t index : tombstone_indices_) {
        if (index < excluded.size() && excluded[index]) {
            continue;
        }
        const unsigned char flags = tombstones_[index];
        if (flags == no_tombstone) {
            continue;
        }
        copy.tombstone_indices_.push_back(index);
        copy.tombstones_.push_back(flags);
        ++copy.tombstone_count_;
    }
}

void Registry::TypeErasedStorage::rebuild_lookup() {
    std::size_t lookup_size = 0;
    for (std::uint32_t index : dense_indices_) {
        lookup_size = std::max(lookup_size, static_cast<std::size_t>(index) + 1);
    }
    for (std::uint32_t index : tombstone_indices_) {
        lookup_size = std::max(lookup_size, static_cast<std::size_t>(index) + 1);
    }

    sparse_.assign(lookup_size, npos);
    for (std::size_t dense = 0; dense < dense_indices_.size(); ++dense) {
        sparse_[dense_indices_[dense]] = static_cast<std::uint32_t>(dense);
    }

    std::vector<unsigned char> rebuilt_tombstones(lookup_size, no_tombstone);
    if (compact_lookup_) {
        for (std::size_t position = 0; position < tombstone_indices_.size(); ++position) {
            rebuilt_tombstones[tombstone_indices_[position]] = tombstones_[position];
        }
    } else {
        for (std::uint32_t index : tombstone_indices_) {
            if (index < tombstones_.size()) {
                rebuilt_tombstones[index] = tombstones_[index];
            }
        }
    }
    tombstones_ = std::move(rebuilt_tombstones);
    compact_lookup_ = false;
}

void Registry::TypeErasedStorage::copy_from(const TypeErasedStorage& other) {
    size_ = other.size_;
    capacity_ = other.capacity_;
    if (capacity_ == 0 || info_.tag) {
        return;
    }

    data_ = allocate(capacity_, info_);
    if (info_.trivially_copyable) {
        if (size_ != 0) {
            std::memcpy(data_, other.data_, info_.size * size_);
        }
        return;
    }

    if (lifecycle_.copy_construct == nullptr) {
        deallocate(data_, info_.alignment);
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        throw std::logic_error("ecs component storage is not copyable");
    }

    std::size_t constructed = 0;
    try {
        for (; constructed < size_; ++constructed) {
            lifecycle_.copy_construct(
                data_ + constructed * info_.size,
                other.data_ + constructed * info_.size);
        }
    } catch (...) {
        for (std::size_t i = 0; i < constructed; ++i) {
            lifecycle_.destroy(data_ + i * info_.size);
        }
        deallocate(data_, info_.alignment);
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        throw;
    }
}

}  // namespace ecs
