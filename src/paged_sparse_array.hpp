#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "sparse_set.hpp"

namespace ecs {

class RawPagedSparseArray {
public:
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

    ~RawPagedSparseArray() {
        release_storage();
    }

    RawPagedSparseArray(RawPagedSparseArray&& other) noexcept
        : component_size_(other.component_size_),
          component_alignment_(other.component_alignment_),
          index_(std::move(other.index_)),
          dense_entities_(std::move(other.dense_entities_)),
          dense_data_(other.dense_data_),
          dense_size_(other.dense_size_),
          dense_capacity_(other.dense_capacity_) {
        other.dense_data_ = nullptr;
        other.dense_size_ = 0;
        other.dense_capacity_ = 0;
    }

    RawPagedSparseArray& operator=(RawPagedSparseArray&& other) noexcept {
        if (this != &other) {
            release_storage();
            component_size_ = other.component_size_;
            component_alignment_ = other.component_alignment_;
            index_ = std::move(other.index_);
            dense_entities_ = std::move(other.dense_entities_);
            dense_data_ = other.dense_data_;
            dense_size_ = other.dense_size_;
            dense_capacity_ = other.dense_capacity_;
            other.dense_data_ = nullptr;
            other.dense_size_ = 0;
            other.dense_capacity_ = 0;
        }
        return *this;
    }

    RawPagedSparseArray(const RawPagedSparseArray&) = delete;
    RawPagedSparseArray& operator=(const RawPagedSparseArray&) = delete;

    void* emplace_copy(Entity entity, const void* value) {
        if (void* existing = try_get_raw(entity)) {
            return existing;
        }

        const std::size_t new_index = dense_size_;
        ensure_capacity(dense_size_ + 1);
        std::memcpy(element_ptr(new_index), value, component_size_);
        ++dense_size_;

        try {
            dense_entities_.push_back(entity);

            if (index_.sparse_index(entity) != PagedSparseSet::npos) {
                --dense_size_;
                dense_entities_.pop_back();

                throw std::invalid_argument("entity index is already occupied by a different version");
            }

            index_.set_index(entity, new_index);
        } catch (...) {
            if (dense_size_ > new_index) {
                --dense_size_;
            }
            if (dense_entities_.size() > new_index) {
                dense_entities_.pop_back();
            }
            throw;
        }

        return element_ptr(new_index);
    }

    bool contains(Entity entity) const {
        return dense_index_of(entity) != PagedSparseSet::npos;
    }

    void* try_get_raw(Entity entity) {
        const std::size_t index = dense_index_of(entity);
        return index == PagedSparseSet::npos ? nullptr : element_ptr(index);
    }

    const void* try_get_raw(Entity entity) const {
        const std::size_t index = dense_index_of(entity);
        return index == PagedSparseSet::npos ? nullptr : element_ptr(index);
    }

    void* get_raw(Entity entity) {
        void* value = try_get_raw(entity);
        if (value == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return value;
    }

    const void* get_raw(Entity entity) const {
        const void* value = try_get_raw(entity);
        if (value == nullptr) {
            throw std::out_of_range("entity does not have this component");
        }
        return value;
    }

    bool erase(Entity entity) {
        const std::size_t index = dense_index_of(entity);
        if (index == PagedSparseSet::npos) {
            return false;
        }

        const std::size_t last_index = dense_size_ - 1;
        if (index != last_index) {
            std::memmove(element_ptr(index), element_ptr(last_index), component_size_);
            dense_entities_[index] = dense_entities_[last_index];
            index_.set_index(dense_entities_[index], index);
        }

        dense_entities_.pop_back();
        --dense_size_;
        index_.clear_index(entity);
        return true;
    }

    void clear() {
        dense_size_ = 0;
        for (const Entity entity : dense_entities_) {
            index_.clear_index(entity);
        }
        dense_entities_.clear();
    }

    std::size_t size() const {
        return dense_size_;
    }

    bool empty() const {
        return dense_size_ == 0;
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

    void* dense_at(std::size_t index) {
        return element_ptr(index);
    }

    const void* dense_at(std::size_t index) const {
        return element_ptr(index);
    }

private:
    std::size_t dense_index_of(Entity entity) const {
        const std::size_t index = index_.sparse_index(entity);
        if (index == PagedSparseSet::npos || index >= dense_size_) {
            return PagedSparseSet::npos;
        }

        return dense_entities_[index] == entity ? index : PagedSparseSet::npos;
    }

    void* element_ptr(std::size_t index) {
        return dense_data_ + (index * component_size_);
    }

    const void* element_ptr(std::size_t index) const {
        return dense_data_ + (index * component_size_);
    }

    void ensure_capacity(std::size_t required) {
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
            std::memcpy(new_data, dense_data_, dense_size_ * component_size_);
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
        dense_size_ = 0;
        dense_capacity_ = 0;
    }

    std::size_t component_size_;
    std::size_t component_alignment_;

    PagedSparseSet index_;
    std::vector<Entity> dense_entities_;
    unsigned char* dense_data_ = nullptr;

    std::size_t dense_size_ = 0;
    std::size_t dense_capacity_ = 0;
};

template <typename T>
class PagedSparseArrayView {
public:
    explicit PagedSparseArrayView(RawPagedSparseArray* raw = nullptr)
        : raw_(raw) {}

    T* try_get(Entity entity) {
        return raw_ == nullptr ? nullptr : static_cast<T*>(raw_->try_get_raw(entity));
    }

    const T* try_get(Entity entity) const {
        return raw_ == nullptr ? nullptr : static_cast<const T*>(raw_->try_get_raw(entity));
    }

    T& get(Entity entity) {
        return *static_cast<T*>(raw_->get_raw(entity));
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

    const std::vector<Entity>& entities() const {
        static const std::vector<Entity> empty;
        return raw_ == nullptr ? empty : raw_->entities();
    }

    template <typename Func>
    void each(Func&& func) {
        if (raw_ == nullptr) {
            return;
        }

        const auto& dense_entities = raw_->entities();
        for (std::size_t i = 0; i < raw_->size(); ++i) {
            func(dense_entities[i], *static_cast<T*>(raw_->dense_at(i)));
        }
    }

    template <typename Func>
    void each(Func&& func) const {
        if (raw_ == nullptr) {
            return;
        }

        const auto& dense_entities = raw_->entities();
        for (std::size_t i = 0; i < raw_->size(); ++i) {
            func(dense_entities[i], *static_cast<const T*>(raw_->dense_at(i)));
        }
    }

private:
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

    const std::vector<Entity>& entities() const {
        static const std::vector<Entity> empty;
        return raw_ == nullptr ? empty : raw_->entities();
    }

    template <typename Func>
    void each(Func&& func) const {
        if (raw_ == nullptr) {
            return;
        }

        const auto& dense_entities = raw_->entities();
        for (std::size_t i = 0; i < raw_->size(); ++i) {
            func(dense_entities[i], *static_cast<const T*>(raw_->dense_at(i)));
        }
    }

private:
    const RawPagedSparseArray* raw_;
};

}  // namespace ecs
