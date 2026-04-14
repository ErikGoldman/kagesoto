#include "sparse_set.hpp"

#include <algorithm>
#include <limits>
#include <memory>

namespace ecs {

namespace {

constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

}  // namespace

PagedSparseSet::PagedSparseSet(std::size_t page_size)
    : page_size_(page_size),
      page_shift_(0),
      page_mask_(0) {
    if (page_size_ == 0 || (page_size_ & (page_size_ - 1)) != 0) {
        throw std::invalid_argument("page size must be a non-zero power of two");
    }

    std::size_t shifted_page_size = page_size_;
    while (shifted_page_size > 1) {
        shifted_page_size = shifted_page_size >> 1;
        ++page_shift_;
    }

    page_mask_ = static_cast<Entity>(page_size_ - 1);
}

PagedSparseSet::~PagedSparseSet() = default;

PagedSparseSet::PagedSparseSet(PagedSparseSet&& other) noexcept = default;

PagedSparseSet& PagedSparseSet::operator=(PagedSparseSet&& other) noexcept = default;

std::size_t PagedSparseSet::page_index(Entity entity) const {
    return static_cast<std::size_t>(entity_index(entity) >> page_shift_);
}

std::size_t PagedSparseSet::page_offset(Entity entity) const {
    return static_cast<std::size_t>(entity_index(entity) & page_mask_);
}

const std::size_t* PagedSparseSet::try_page(Entity entity) const {
    const std::size_t index = page_index(entity);
    if (index >= sparse_pages_.size() || !sparse_pages_[index]) {
        return nullptr;
    }
    return sparse_pages_[index].get();
}

PagedSparseSet::Page& PagedSparseSet::ensure_page(Entity entity) {
    const std::size_t index = page_index(entity);
    if (index >= sparse_pages_.size()) {
        sparse_pages_.resize(index + 1);
    }

    if (!sparse_pages_[index]) {
        sparse_pages_[index] = std::make_unique<std::size_t[]>(page_size_);
        std::fill_n(sparse_pages_[index].get(), page_size_, npos);
    }

    return sparse_pages_[index];
}

void PagedSparseSet::set_sparse_index(Entity entity, std::size_t value) {
    ensure_page(entity)[page_offset(entity)] = value;
}

std::size_t PagedSparseSet::sparse_index(Entity entity) const {
    const std::size_t* page = try_page(entity);
    if (page == nullptr) {
        return npos;
    }

    return page[page_offset(entity)];
}

void PagedSparseSet::set_index(Entity entity, std::size_t value) {
    set_sparse_index(entity, value);
}

void PagedSparseSet::clear_index(Entity entity) {
    const std::size_t index = page_index(entity);
    if (index < sparse_pages_.size() && sparse_pages_[index]) {
        sparse_pages_[index][page_offset(entity)] = npos;
    }
}

std::size_t PagedSparseSet::find_slot(Entity entity) const {
    const std::size_t dense_index = sparse_index(entity);
    if (dense_index == npos || dense_index >= dense_entities_.size()) {
        return npos;
    }

    return entity_index(dense_entities_[dense_index]) == entity_index(entity) ? dense_index : npos;
}

std::size_t PagedSparseSet::find_index(Entity entity) const {
    const std::size_t dense_index = find_slot(entity);
    if (dense_index == npos) {
        return npos;
    }

    return dense_entities_[dense_index] == entity ? dense_index : npos;
}

bool PagedSparseSet::contains(Entity entity) const {
    return find_index(entity) != npos;
}

bool PagedSparseSet::insert(Entity entity) {
    if (sparse_index(entity) != npos) {
        return false;
    }

    Page& page = ensure_page(entity);
    dense_entities_.push_back(entity);
    page[page_offset(entity)] = dense_entities_.size() - 1;
    return true;
}

bool PagedSparseSet::erase(Entity entity) {
    const std::size_t index = find_index(entity);
    if (index == npos) {
        return false;
    }

    const std::size_t last_index = dense_entities_.size() - 1;
    const Entity last_entity = dense_entities_.back();

    if (index != last_index) {
        dense_entities_[index] = last_entity;
        set_sparse_index(last_entity, index);
    }

    dense_entities_.pop_back();
    clear_index(entity);
    return true;
}

void PagedSparseSet::clear() {
    for (const Entity entity : dense_entities_) {
        clear_index(entity);
    }

    dense_entities_.clear();
}

std::size_t PagedSparseSet::page_size() const {
    return page_size_;
}

std::size_t PagedSparseSet::index_of(Entity entity) const {
    return find_index(entity);
}

std::size_t PagedSparseSet::size() const {
    return dense_entities_.size();
}

bool PagedSparseSet::empty() const {
    return dense_entities_.empty();
}

const std::vector<Entity>& PagedSparseSet::entities() const {
    return dense_entities_;
}

}  // namespace ecs
