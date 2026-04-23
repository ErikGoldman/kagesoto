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

namespace ecs {

namespace detail {

template <typename Key, typename Compare>
class SimdBPlusTree {
public:
    static constexpr std::size_t leaf_capacity = 16;
    static constexpr std::size_t internal_fanout = 16;

    struct Entry {
        Key key;
        Entity entity;
    };

    explicit SimdBPlusTree(bool unique = false, Compare compare = Compare{})
        : unique_(unique),
          compare_(std::move(compare)) {}

    void clear() {
        entries_.clear();
        root_.reset();
        leftmost_leaf_ = nullptr;
        tree_dirty_ = false;
    }

    void insert(const Key& key, Entity entity) {
        const Entry entry{key, entity};
        if (unique_ && find_one_in_entries(key) != null_entity) {
            throw std::invalid_argument("unique index constraint violated");
        }

        const auto it = std::lower_bound(entries_.begin(), entries_.end(), entry, entry_less);

        entries_.insert(it, entry);
        tree_dirty_ = true;
    }

    void erase(const Key& key, Entity entity) {
        const Entry target{key, entity};
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), target, entry_less);
        if (it != entries_.end() && it->entity == entity && keys_equal(it->key, key)) {
            entries_.erase(it);
            tree_dirty_ = true;
        }
    }

    std::vector<Entity> find(const Key& key) const {
        ensure_built();
        std::vector<Entity> matches;
        const LeafNode* leaf = find_leaf(key);
        while (leaf != nullptr) {
            bool found_in_leaf = false;
            for (const Entry& entry : leaf->entries) {
                if (compare_(entry.key, key)) {
                    continue;
                }
                if (compare_(key, entry.key)) {
                    return matches;
                }

                matches.push_back(entry.entity);
                found_in_leaf = true;
                if (unique_) {
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
        const auto end = inclusive
            ? std::upper_bound(entries_.begin(), entries_.end(), key,
                               [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); })
            : std::lower_bound(entries_.begin(), entries_.end(), key,
                               [&](const Entry& entry, const Key& value) { return compare_(entry.key, value); });
        return collect(entries_.begin(), end);
    }

    std::vector<Entity> find_greater_than(const Key& key, bool inclusive) const {
        const auto begin = inclusive
            ? std::lower_bound(entries_.begin(), entries_.end(), key,
                               [&](const Entry& entry, const Key& value) { return compare_(entry.key, value); })
            : std::upper_bound(entries_.begin(), entries_.end(), key,
                               [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); });
        return collect(begin, entries_.end());
    }

    std::vector<Entity> find_not_equal(const Key& key) const {
        const auto lower = std::lower_bound(entries_.begin(), entries_.end(), key,
                                            [&](const Entry& entry, const Key& value) { return compare_(entry.key, value); });
        const auto upper = std::upper_bound(entries_.begin(), entries_.end(), key,
                                            [&](const Key& value, const Entry& entry) { return compare_(value, entry.key); });

        std::vector<Entity> matches;
        matches.reserve(entries_.size() - static_cast<std::size_t>(upper - lower));
        append_entities(matches, entries_.begin(), lower);
        append_entities(matches, upper, entries_.end());
        return matches;
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
        LeafNode* next = nullptr;
    };

    struct InternalNode final : Node {
        std::vector<Key> separators;
        std::vector<std::unique_ptr<Node>> children;
    };

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

    static inline EntryLess entry_less{Compare{}};

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

    bool keys_equal(const Key& lhs, const Key& rhs) const {
        return !compare_(lhs, rhs) && !compare_(rhs, lhs);
    }

    void ensure_built() const {
        if (!tree_dirty_) {
            return;
        }
        rebuild();
    }

    Entity find_one_in_entries(const Key& key) const {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                         [&](const Entry& entry, const Key& value) { return compare_(entry.key, value); });
        if (it == entries_.end() || compare_(key, it->key)) {
            return null_entity;
        }
        return it->entity;
    }

    void rebuild() const {
        root_.reset();
        leftmost_leaf_ = nullptr;

        if (entries_.empty()) {
            tree_dirty_ = false;
            return;
        }

        std::vector<std::unique_ptr<Node>> level;
        level.reserve((entries_.size() + leaf_capacity - 1) / leaf_capacity);

        LeafNode* previous_leaf = nullptr;
        for (std::size_t offset = 0; offset < entries_.size(); offset += leaf_capacity) {
            auto leaf = std::make_unique<LeafNode>();
            const std::size_t end = std::min(offset + leaf_capacity, entries_.size());
            leaf->entries.assign(entries_.begin() + static_cast<std::ptrdiff_t>(offset),
                                 entries_.begin() + static_cast<std::ptrdiff_t>(end));
            leaf->first_key = leaf->entries.front().key;

            if (previous_leaf != nullptr) {
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
            node = internal->children[child_index(*internal, key)].get();
        }

        return static_cast<const LeafNode*>(node);
    }

    std::size_t child_index(const InternalNode& node, const Key& key) const {
        if constexpr (std::is_same_v<Key, std::int32_t> && std::is_same_v<Compare, std::less<Key>>) {
            return simd_child_index_i32(node.separators, key);
        } else {
            return scalar_child_index(node.separators, key);
        }
    }

    std::size_t scalar_child_index(const std::vector<Key>& separators, const Key& key) const {
        return static_cast<std::size_t>(
            std::upper_bound(separators.begin(), separators.end(), key, compare_) - separators.begin());
    }

    std::size_t simd_child_index_i32(const std::vector<Key>& separators, const Key& key) const {
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
        return scalar_child_index(separators, key);
#endif
    }

    static unsigned first_set_bit(unsigned mask) {
#if defined(_MSC_VER)
        unsigned long index = 0;
        _BitScanForward(&index, mask);
        return static_cast<unsigned>(index);
#else
        return static_cast<unsigned>(__builtin_ctz(mask));
#endif
    }

    bool unique_;
    Compare compare_;
    std::vector<Entry> entries_;
    mutable std::unique_ptr<Node> root_;
    mutable LeafNode* leftmost_leaf_ = nullptr;
    mutable bool tree_dirty_ = false;
};

template <typename Key, typename Compare = std::less<Key>>
using BPlusTree = SimdBPlusTree<Key, Compare>;

}  // namespace detail

}  // namespace ecs
