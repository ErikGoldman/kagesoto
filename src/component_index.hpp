#pragma once

#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "bplus_tree.hpp"

namespace ecs {

enum class PredicateOperator {
    eq,
    ne,
    gt,
    gte,
    lt,
    lte,
};

template <typename Component>
struct ComponentIndices {
    using type = std::tuple<>;
};

namespace detail {

template <typename MemberPointer>
struct member_pointer_traits;

template <typename Component, typename Member>
struct member_pointer_traits<Member Component::*> {
    using component_type = Component;
    using member_type = Member;
};

template <bool Unique, auto... Members>
struct ComponentIndexSpec;

template <bool Unique, auto Member>
struct ComponentIndexSpec<Unique, Member> {
    using member_pointer = decltype(Member);
    using pointer_traits = member_pointer_traits<member_pointer>;
    using component_type = typename pointer_traits::component_type;
    using key_type = typename pointer_traits::member_type;

    static constexpr bool unique = Unique;
    static constexpr bool is_single_member = true;
    static constexpr auto member = Member;

    static key_type key(const component_type& component) {
        return component.*Member;
    }
};

template <bool Unique, auto FirstMember, auto... RestMembers>
struct ComponentIndexSpec<Unique, FirstMember, RestMembers...> {
    using first_pointer = decltype(FirstMember);
    using first_traits = member_pointer_traits<first_pointer>;
    using component_type = typename first_traits::component_type;
    using key_type = std::tuple<typename first_traits::member_type,
                                typename member_pointer_traits<decltype(RestMembers)>::member_type...>;

    static constexpr bool unique = Unique;
    static constexpr bool is_single_member = false;

    static_assert((std::is_same_v<component_type, typename member_pointer_traits<decltype(RestMembers)>::component_type> && ...),
                  "all indexed members must belong to the same component type");

    static key_type key(const component_type& component) {
        return key_type{component.*FirstMember, component.*RestMembers...};
    }
};

template <typename IndexSpec, auto... Members>
struct index_matches_members : std::false_type {};

template <bool Unique, auto... IndexedMembers, auto... Members>
struct index_matches_members<ComponentIndexSpec<Unique, IndexedMembers...>, Members...>
    : std::is_same<
          std::tuple<std::integral_constant<decltype(IndexedMembers), IndexedMembers>...>,
          std::tuple<std::integral_constant<decltype(Members), Members>...>> {};

template <typename IndexSpec, typename... Parts>
typename IndexSpec::key_type make_index_key(Parts&&... parts) {
    if constexpr (sizeof...(Parts) == 1 &&
                  std::is_same_v<typename IndexSpec::key_type, std::decay_t<std::tuple_element_t<0, std::tuple<Parts...>>>>) {
        return typename IndexSpec::key_type(std::forward<Parts>(parts)...);
    } else {
        return typename IndexSpec::key_type{std::forward<Parts>(parts)...};
    }
}

template <typename IndexSpec>
class IndexRuntime {
public:
    using component_type = typename IndexSpec::component_type;
    using key_type = typename IndexSpec::key_type;

    void clear() {
        tree_.clear();
    }

    void insert(Entity entity, const component_type& component) {
        tree_.insert(IndexSpec::key(component), entity);
    }

    void erase(Entity entity, const component_type& component) {
        tree_.erase(IndexSpec::key(component), entity);
    }

    void replace(Entity entity, const component_type& previous, const component_type& next) {
        const key_type previous_key = IndexSpec::key(previous);
        const key_type next_key = IndexSpec::key(next);

        if constexpr (IndexSpec::unique) {
            if (!(previous_key == next_key)) {
                const Entity existing = tree_.find_one(next_key);
                if (existing != null_entity && existing != entity) {
                    throw std::invalid_argument("unique index constraint violated");
                }
            }
        }

        tree_.erase(previous_key, entity);
        tree_.insert(next_key, entity);
    }

    std::vector<Entity> find(const key_type& key) const {
        return tree_.find(key);
    }

    std::vector<Entity> find_compare(PredicateOperator op, const key_type& key) const {
        switch (op) {
            case PredicateOperator::eq:
                return tree_.find(key);
            case PredicateOperator::ne:
                return tree_.find_not_equal(key);
            case PredicateOperator::gt:
                return tree_.find_greater_than(key, false);
            case PredicateOperator::gte:
                return tree_.find_greater_than(key, true);
            case PredicateOperator::lt:
                return tree_.find_less_than(key, false);
            case PredicateOperator::lte:
                return tree_.find_less_than(key, true);
        }

        return {};
    }

    Entity find_one(const key_type& key) const {
        return tree_.find_one(key);
    }

private:
    detail::BPlusTree<key_type> tree_{IndexSpec::unique};
};

template <typename Query, typename... Declared>
inline constexpr bool tuple_contains_v = (std::is_same_v<Query, Declared> || ...);

template <auto Member, typename... Declared>
struct member_index_spec;

template <typename Tuple, auto... Members>
struct tuple_member_pack_index_spec;

template <auto Member, typename Tuple>
struct tuple_member_index_spec;

template <auto Member, typename IndexSpec, bool = IndexSpec::is_single_member>
struct member_index_matches : std::false_type {};

template <auto Member, typename IndexSpec>
struct member_index_matches<Member, IndexSpec, true>
    : std::bool_constant<(IndexSpec::member == Member)> {};

template <auto Member>
struct member_index_spec<Member> {
    using type = void;
};

template <auto Member, typename First, typename... Rest>
struct member_index_spec<Member, First, Rest...> {
    using type = std::conditional_t<
        member_index_matches<Member, First>::value,
        First,
        typename member_index_spec<Member, Rest...>::type>;
};

template <auto Member, typename... Declared>
struct tuple_member_index_spec<Member, std::tuple<Declared...>> {
    using type = typename member_index_spec<Member, Declared...>::type;
};

template <auto... Members>
struct tuple_member_pack_index_spec<std::tuple<>, Members...> {
    using type = void;
};

template <typename First, typename... Rest, auto... Members>
struct tuple_member_pack_index_spec<std::tuple<First, Rest...>, Members...> {
    using type = std::conditional_t<
        index_matches_members<First, Members...>::value,
        First,
        typename tuple_member_pack_index_spec<std::tuple<Rest...>, Members...>::type>;
};

template <typename Component, typename Tuple>
class IndexSet;

template <typename Component, typename... IndexSpecs>
class IndexSet<Component, std::tuple<IndexSpecs...>> {
public:
    void clear() {
        (std::get<IndexRuntime<IndexSpecs>>(indexes_).clear(), ...);
    }

    void insert(Entity entity, const Component& component) {
        (std::get<IndexRuntime<IndexSpecs>>(indexes_).insert(entity, component), ...);
    }

    void erase(Entity entity, const Component& component) {
        (std::get<IndexRuntime<IndexSpecs>>(indexes_).erase(entity, component), ...);
    }

    void replace(Entity entity, const Component& previous, const Component& next) {
        (std::get<IndexRuntime<IndexSpecs>>(indexes_).replace(entity, previous, next), ...);
    }

    template <typename IndexSpec>
    std::vector<Entity> find(const typename IndexSpec::key_type& key) const {
        static_assert(tuple_contains_v<IndexSpec, IndexSpecs...>, "component does not declare this index");
        return std::get<IndexRuntime<IndexSpec>>(indexes_).find(key);
    }

    template <typename IndexSpec>
    std::vector<Entity> find_compare(PredicateOperator op, const typename IndexSpec::key_type& key) const {
        static_assert(tuple_contains_v<IndexSpec, IndexSpecs...>, "component does not declare this index");
        return std::get<IndexRuntime<IndexSpec>>(indexes_).find_compare(op, key);
    }

    template <typename IndexSpec>
    Entity find_one(const typename IndexSpec::key_type& key) const {
        static_assert(tuple_contains_v<IndexSpec, IndexSpecs...>, "component does not declare this index");
        return std::get<IndexRuntime<IndexSpec>>(indexes_).find_one(key);
    }

private:
    std::tuple<IndexRuntime<IndexSpecs>...> indexes_;
};

template <typename Component>
class IndexSet<Component, std::tuple<>> {
public:
    void clear() {}
    void insert(Entity, const Component&) {}
    void erase(Entity, const Component&) {}
    void replace(Entity, const Component&, const Component&) {}

    template <typename IndexSpec>
    std::vector<Entity> find(const typename IndexSpec::key_type&) const {
        static_assert(!std::is_same_v<IndexSpec, IndexSpec>, "component does not declare this index");
        return {};
    }

    template <typename IndexSpec>
    std::vector<Entity> find_compare(PredicateOperator, const typename IndexSpec::key_type&) const {
        static_assert(!std::is_same_v<IndexSpec, IndexSpec>, "component does not declare this index");
        return {};
    }

    template <typename IndexSpec>
    Entity find_one(const typename IndexSpec::key_type&) const {
        static_assert(!std::is_same_v<IndexSpec, IndexSpec>, "component does not declare this index");
        return null_entity;
    }
};

}  // namespace detail

template <auto... Members>
using Index = detail::ComponentIndexSpec<false, Members...>;

template <auto... Members>
using UniqueIndex = detail::ComponentIndexSpec<true, Members...>;

}  // namespace ecs
