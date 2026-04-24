#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "sparse_set.hpp"

#ifndef ECS_INDEX_BACKEND
#define ECS_INDEX_BACKEND 2
#endif

#define ECS_INDEX_BACKEND_DEFERRED_BPLUS 0
#define ECS_INDEX_BACKEND_OPTIMIZED_BPLUS 1
#define ECS_INDEX_BACKEND_FLAT_SORTED 2

namespace ecs {

namespace index_backend {

struct optimized_bplus {};
struct flat_sorted {};

}  // namespace index_backend

namespace detail {

using default_index_backend_tag =
#if ECS_INDEX_BACKEND == ECS_INDEX_BACKEND_DEFERRED_BPLUS
    index_backend::optimized_bplus;
#elif ECS_INDEX_BACKEND == ECS_INDEX_BACKEND_OPTIMIZED_BPLUS
    index_backend::optimized_bplus;
#elif ECS_INDEX_BACKEND == ECS_INDEX_BACKEND_FLAT_SORTED
    index_backend::flat_sorted;
#else
#error "Unsupported ECS_INDEX_BACKEND"
#endif

template <typename Key, typename Compare>
class OrderedEntriesStore {
public:
    struct Entry {
        Key key;
        Entity entity;
    };

    using const_iterator = typename std::vector<Entry>::const_iterator;

    explicit OrderedEntriesStore(bool unique = false, Compare compare = Compare{})
        : unique_(unique),
          compare_(std::move(compare)) {}

    void clear() {
        entries_.clear();
    }

    void insert(const Key& key, Entity entity) {
        const Entry entry{key, entity};
        if (unique_ && find_one(key) != null_entity) {
            throw std::invalid_argument("unique index constraint violated");
        }

        const auto it = std::lower_bound(entries_.begin(), entries_.end(), entry, entry_less());
        entries_.insert(it, entry);
    }

    void erase(const Key& key, Entity entity) {
        const Entry target{key, entity};
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), target, entry_less());
        if (it != entries_.end() && it->entity == entity && keys_equal(it->key, key)) {
            entries_.erase(it);
        }
    }

    std::vector<Entity> find_equal(const Key& key) const {
        const auto range = equal_range(key);
        return collect(range.first, range.second);
    }

    Entity find_one(const Key& key) const {
        const auto it = lower_bound(key);
        if (it == entries_.end() || compare_(key, it->key)) {
            return null_entity;
        }
        return it->entity;
    }

    std::vector<Entity> find_less_than(const Key& key, bool inclusive) const {
        const auto end = inclusive
            ? std::upper_bound(entries_.begin(), entries_.end(), key,
                               [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); })
            : lower_bound(key);
        return collect(entries_.begin(), end);
    }

    std::vector<Entity> find_greater_than(const Key& key, bool inclusive) const {
        const auto begin = inclusive
            ? lower_bound(key)
            : std::upper_bound(entries_.begin(), entries_.end(), key,
                               [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); });
        return collect(begin, entries_.end());
    }

    std::vector<Entity> find_not_equal(const Key& key) const {
        const auto range = equal_range(key);
        std::vector<Entity> matches;
        matches.reserve(entries_.size() - static_cast<std::size_t>(range.second - range.first));
        append_entities(matches, entries_.begin(), range.first);
        append_entities(matches, range.second, entries_.end());
        return matches;
    }

    const std::vector<Entry>& entries() const {
        return entries_;
    }

    const Compare& compare() const {
        return compare_;
    }

    bool unique() const {
        return unique_;
    }

    bool keys_equal(const Key& lhs, const Key& rhs) const {
        return !compare_(lhs, rhs) && !compare_(rhs, lhs);
    }

    const_iterator lower_bound(const Key& key) const {
        return std::lower_bound(entries_.begin(), entries_.end(), key,
                                [&](const Entry& entry, const Key& value) { return compare_(entry.key, value); });
    }

    std::pair<const_iterator, const_iterator> equal_range(const Key& key) const {
        const auto begin = lower_bound(key);
        const auto end = std::upper_bound(entries_.begin(), entries_.end(), key,
                                          [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); });
        return {begin, end};
    }

    template <typename Iter>
    static void append_entities(std::vector<Entity>& matches, Iter begin, Iter end) {
        for (auto it = begin; it != end; ++it) {
            matches.push_back(it->entity);
        }
    }

    template <typename Iter>
    static std::vector<Entity> collect(Iter begin, Iter end) {
        std::vector<Entity> matches;
        matches.reserve(static_cast<std::size_t>(end - begin));
        append_entities(matches, begin, end);
        return matches;
    }

private:
    struct EntryLess {
        Compare compare;

        bool operator()(const Entry& lhs, const Entry& rhs) const {
            if (compare(lhs.key, rhs.key)) {
                return true;
            }
            if (compare(rhs.key, lhs.key)) {
                return false;
            }
            return lhs.entity < rhs.entity;
        }
    };

    EntryLess entry_less() const {
        return EntryLess{compare_};
    }

    bool unique_;
    Compare compare_;
    std::vector<Entry> entries_;
};

inline unsigned first_set_bit(unsigned mask) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward(&index, mask);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctz(mask));
#endif
}

template <typename Key, typename Compare>
std::size_t scalar_child_index(const std::vector<Key>& separators, const Key& key, const Compare& compare) {
    return static_cast<std::size_t>(
        std::upper_bound(separators.begin(), separators.end(), key, compare) - separators.begin());
}

template <typename Key, typename Compare>
std::size_t child_index_for_separators(const std::vector<Key>& separators, const Key& key, const Compare& compare) {
    if constexpr (std::is_same_v<Key, std::int32_t> && std::is_same_v<Compare, std::less<Key>>) {
#if defined(__SSE2__)
        const __m128i key_vector = _mm_set1_epi32(key);
        std::size_t index = 0;

        while (index + 4 <= separators.size()) {
            const __m128i separators_vector =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(separators.data() + index));
            const __m128i cmp = _mm_cmpgt_epi32(separators_vector, key_vector);
            const unsigned mask = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(cmp)));
            if (mask != 0) {
                return index + static_cast<std::size_t>(first_set_bit(mask));
            }
            index += 4;
        }

        for (; index < separators.size(); ++index) {
            if (key < separators[index]) {
                return index;
            }
        }

        return separators.size();
#else
        return scalar_child_index(separators, key, compare);
#endif
    } else {
        return scalar_child_index(separators, key, compare);
    }
}

template <typename Key, typename Compare = std::less<Key>>
class FlatSortedIndex {
public:
    explicit FlatSortedIndex(bool unique = false, Compare compare = Compare{})
        : store_(unique, std::move(compare)) {}

    void clear() {
        store_.clear();
    }

    void insert(const Key& key, Entity entity) {
        store_.insert(key, entity);
    }

    void erase(const Key& key, Entity entity) {
        store_.erase(key, entity);
    }

    std::vector<Entity> find(const Key& key) const {
        return store_.find_equal(key);
    }

    std::vector<Entity> find_less_than(const Key& key, bool inclusive) const {
        return store_.find_less_than(key, inclusive);
    }

    std::vector<Entity> find_greater_than(const Key& key, bool inclusive) const {
        return store_.find_greater_than(key, inclusive);
    }

    std::vector<Entity> find_not_equal(const Key& key) const {
        return store_.find_not_equal(key);
    }

    Entity find_one(const Key& key) const {
        return store_.find_one(key);
    }

private:
    OrderedEntriesStore<Key, Compare> store_;
};

template <typename Key, typename Compare = std::less<Key>>
class DeferredBPlusIndex {
public:
    using Entry = typename OrderedEntriesStore<Key, Compare>::Entry;

    static constexpr std::size_t leaf_capacity = 16;
    static constexpr std::size_t internal_fanout = 16;

    explicit DeferredBPlusIndex(bool unique = false, Compare compare = Compare{})
        : store_(unique, std::move(compare)) {}

    void clear() {
        store_.clear();
        root_.reset();
        leftmost_leaf_ = nullptr;
        tree_dirty_ = false;
    }

    void insert(const Key& key, Entity entity) {
        store_.insert(key, entity);
        tree_dirty_ = true;
    }

    void erase(const Key& key, Entity entity) {
        store_.erase(key, entity);
        tree_dirty_ = true;
    }

    std::vector<Entity> find(const Key& key) const {
        ensure_built();
        std::vector<Entity> matches;
        const LeafNode* leaf = find_leaf(key);
        while (leaf != nullptr) {
            bool found_in_leaf = false;
            for (const Entry& entry : leaf->entries) {
                if (store_.compare()(entry.key, key)) {
                    continue;
                }
                if (store_.compare()(key, entry.key)) {
                    return matches;
                }

                matches.push_back(entry.entity);
                found_in_leaf = true;
                if (store_.unique()) {
                    return matches;
                }
            }

            if (!found_in_leaf) {
                return matches;
            }

            leaf = leaf->next;
        }
        return matches;
    }

    std::vector<Entity> find_less_than(const Key& key, bool inclusive) const {
        return store_.find_less_than(key, inclusive);
    }

    std::vector<Entity> find_greater_than(const Key& key, bool inclusive) const {
        return store_.find_greater_than(key, inclusive);
    }

    std::vector<Entity> find_not_equal(const Key& key) const {
        return store_.find_not_equal(key);
    }

    Entity find_one(const Key& key) const {
        const std::vector<Entity> matches = find(key);
        return matches.empty() ? null_entity : matches.front();
    }

private:
    struct Node {
        virtual ~Node() = default;

        Key first_key{};
        bool is_leaf = false;
    };

    struct LeafNode final : Node {
        LeafNode() {
            this->is_leaf = true;
        }

        std::vector<Entry> entries;
        LeafNode* prev = nullptr;
        LeafNode* next = nullptr;
    };

    struct InternalNode final : Node {
        std::vector<Key> separators;
        std::vector<std::unique_ptr<Node>> children;
    };

    void ensure_built() const {
        if (!tree_dirty_) {
            return;
        }
        rebuild();
    }

    void rebuild() const {
        root_.reset();
        leftmost_leaf_ = nullptr;

        const auto& entries = store_.entries();
        if (entries.empty()) {
            tree_dirty_ = false;
            return;
        }

        std::vector<std::unique_ptr<Node>> level;
        level.reserve((entries.size() + leaf_capacity - 1) / leaf_capacity);

        LeafNode* previous_leaf = nullptr;
        for (std::size_t offset = 0; offset < entries.size(); offset += leaf_capacity) {
            auto leaf = std::make_unique<LeafNode>();
            const std::size_t end = std::min(offset + leaf_capacity, entries.size());
            leaf->entries.assign(entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                 entries.begin() + static_cast<std::ptrdiff_t>(end));
            leaf->first_key = leaf->entries.front().key;

            if (previous_leaf != nullptr) {
                leaf->prev = previous_leaf;
                previous_leaf->next = leaf.get();
            } else {
                leftmost_leaf_ = leaf.get();
            }
            previous_leaf = leaf.get();

            level.push_back(std::move(leaf));
        }

        while (level.size() > 1) {
            std::vector<std::unique_ptr<Node>> next_level;
            next_level.reserve((level.size() + internal_fanout - 1) / internal_fanout);

            for (std::size_t offset = 0; offset < level.size(); offset += internal_fanout) {
                auto internal = std::make_unique<InternalNode>();
                const std::size_t end = std::min(offset + internal_fanout, level.size());
                internal->children.reserve(end - offset);
                internal->separators.reserve(end - offset > 0 ? end - offset - 1 : 0);

                for (std::size_t i = offset; i < end; ++i) {
                    if (!internal->children.empty()) {
                        internal->separators.push_back(level[i]->first_key);
                    }
                    internal->children.push_back(std::move(level[i]));
                }

                internal->first_key = internal->children.front()->first_key;
                next_level.push_back(std::move(internal));
            }

            level = std::move(next_level);
        }

        root_ = std::move(level.front());
        tree_dirty_ = false;
    }

    const LeafNode* find_leaf(const Key& key) const {
        if (root_ == nullptr) {
            return nullptr;
        }

        const Node* node = root_.get();
        while (!node->is_leaf) {
            const auto* internal = static_cast<const InternalNode*>(node);
            node = internal->children[child_index_for_separators(internal->separators, key, store_.compare())].get();
        }

        auto* leaf = static_cast<const LeafNode*>(node);
        while (leaf->prev != nullptr && store_.keys_equal(leaf->entries.front().key, key)) {
            const Entry& previous_last = leaf->prev->entries.back();
            if (!store_.keys_equal(previous_last.key, key)) {
                break;
            }
            leaf = leaf->prev;
        }

        return leaf;
    }

    OrderedEntriesStore<Key, Compare> store_;
    mutable std::unique_ptr<Node> root_;
    mutable LeafNode* leftmost_leaf_ = nullptr;
    mutable bool tree_dirty_ = false;
};

template <typename Key, typename Compare = std::less<Key>>
class OptimizedBPlusIndex {
public:
    using Entry = typename OrderedEntriesStore<Key, Compare>::Entry;

    static constexpr std::size_t leaf_capacity = 16;
    static constexpr std::size_t internal_fanout = 16;

    explicit OptimizedBPlusIndex(bool unique = false, Compare compare = Compare{})
        : store_(unique, std::move(compare)) {}

    void clear() {
        store_.clear();
        leaves_.clear();
        internal_levels_.clear();
        tree_dirty_ = false;
    }

    void insert(const Key& key, Entity entity) {
        store_.insert(key, entity);
        tree_dirty_ = true;
    }

    void erase(const Key& key, Entity entity) {
        store_.erase(key, entity);
        tree_dirty_ = true;
    }

    std::vector<Entity> find(const Key& key) const {
        ensure_built();
        const LeafNode* leaf = find_leaf(key);
        if (leaf == nullptr) {
            return {};
        }

        std::vector<Entity> matches;
        const auto& entries = store_.entries();
        std::size_t leaf_index = static_cast<std::size_t>(leaf - leaves_.data());
        while (leaf_index < leaves_.size()) {
            const LeafNode& current = leaves_[leaf_index];
            bool found_in_leaf = false;
            for (std::size_t i = current.begin; i < current.end; ++i) {
                const Entry& entry = entries[i];
                if (store_.compare()(entry.key, key)) {
                    continue;
                }
                if (store_.compare()(key, entry.key)) {
                    return matches;
                }

                matches.push_back(entry.entity);
                found_in_leaf = true;
                if (store_.unique()) {
                    return matches;
                }
            }

            if (!found_in_leaf || current.next == npos) {
                return matches;
            }
            leaf_index = current.next;
        }

        return matches;
    }

    std::vector<Entity> find_less_than(const Key& key, bool inclusive) const {
        return store_.find_less_than(key, inclusive);
    }

    std::vector<Entity> find_greater_than(const Key& key, bool inclusive) const {
        return store_.find_greater_than(key, inclusive);
    }

    std::vector<Entity> find_not_equal(const Key& key) const {
        return store_.find_not_equal(key);
    }

    Entity find_one(const Key& key) const {
        const std::vector<Entity> matches = find(key);
        return matches.empty() ? null_entity : matches.front();
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    struct LeafNode {
        Key first_key{};
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t prev = npos;
        std::size_t next = npos;
    };

    struct InternalNode {
        Key first_key{};
        std::vector<Key> separators;
        std::vector<std::size_t> children;
    };

    void ensure_built() const {
        if (!tree_dirty_) {
            return;
        }
        rebuild();
    }

    void rebuild() const {
        leaves_.clear();
        internal_levels_.clear();

        const auto& entries = store_.entries();
        if (entries.empty()) {
            tree_dirty_ = false;
            return;
        }

        leaves_.reserve((entries.size() + leaf_capacity - 1) / leaf_capacity);
        for (std::size_t offset = 0; offset < entries.size(); offset += leaf_capacity) {
            const std::size_t end = std::min(offset + leaf_capacity, entries.size());
            leaves_.push_back(LeafNode{entries[offset].key, offset, end, npos, npos});
        }
        for (std::size_t i = 0; i + 1 < leaves_.size(); ++i) {
            leaves_[i].next = i + 1;
            leaves_[i + 1].prev = i;
        }

        std::vector<Key> child_first_keys;
        child_first_keys.reserve(leaves_.size());
        for (const LeafNode& leaf : leaves_) {
            child_first_keys.push_back(leaf.first_key);
        }

        std::size_t child_count = leaves_.size();
        std::vector<std::vector<InternalNode>> bottom_up_levels;
        while (child_count > 1) {
            std::vector<InternalNode> level;
            level.reserve((child_count + internal_fanout - 1) / internal_fanout);

            std::vector<Key> next_first_keys;
            next_first_keys.reserve((child_count + internal_fanout - 1) / internal_fanout);

            for (std::size_t offset = 0; offset < child_count; offset += internal_fanout) {
                const std::size_t end = std::min(offset + internal_fanout, child_count);
                InternalNode node;
                node.first_key = child_first_keys[offset];
                node.children.reserve(end - offset);
                node.separators.reserve(end - offset > 0 ? end - offset - 1 : 0);

                for (std::size_t i = offset; i < end; ++i) {
                    if (!node.children.empty()) {
                        node.separators.push_back(child_first_keys[i]);
                    }
                    node.children.push_back(i);
                }

                next_first_keys.push_back(node.first_key);
                level.push_back(std::move(node));
            }

            bottom_up_levels.push_back(std::move(level));
            child_first_keys = std::move(next_first_keys);
            child_count = child_first_keys.size();
        }

        internal_levels_.assign(bottom_up_levels.rbegin(), bottom_up_levels.rend());
        tree_dirty_ = false;
    }

    const LeafNode* find_leaf(const Key& key) const {
        if (leaves_.empty()) {
            return nullptr;
        }
        if (internal_levels_.empty()) {
            return &leaves_.front();
        }

        std::size_t node_index = 0;
        for (std::size_t level_index = 0; level_index < internal_levels_.size(); ++level_index) {
            const InternalNode& node = internal_levels_[level_index][node_index];
            const std::size_t child_slot = child_index_for_separators(node.separators, key, store_.compare());
            node_index = node.children[child_slot];
        }

        while (leaves_[node_index].prev != npos && store_.keys_equal(leaves_[node_index].first_key, key)) {
            const LeafNode& previous = leaves_[leaves_[node_index].prev];
            if (!store_.keys_equal(store_.entries()[previous.end - 1].key, key)) {
                break;
            }
            node_index = leaves_[node_index].prev;
        }

        return &leaves_[node_index];
    }

    OrderedEntriesStore<Key, Compare> store_;
    mutable std::vector<LeafNode> leaves_;
    mutable std::vector<std::vector<InternalNode>> internal_levels_;
    mutable bool tree_dirty_ = false;
};

template <typename BackendTag, typename Key, typename Compare = std::less<Key>>
struct index_backend_type;

template <typename Key, typename Compare>
struct index_backend_type<index_backend::optimized_bplus, Key, Compare> {
    using type = OptimizedBPlusIndex<Key, Compare>;
};

template <typename Key, typename Compare>
struct index_backend_type<index_backend::flat_sorted, Key, Compare> {
    using type = FlatSortedIndex<Key, Compare>;
};

template <typename BackendTag, typename Key, typename Compare = std::less<Key>>
using index_backend_type_t = typename index_backend_type<BackendTag, Key, Compare>::type;

}  // namespace detail

}  // namespace ecs
