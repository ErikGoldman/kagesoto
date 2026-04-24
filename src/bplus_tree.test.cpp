#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "bplus_tree.hpp"

namespace {

template <typename Index>
void seed_index(Index& index) {
    index.insert(4, 40);
    index.insert(1, 10);
    index.insert(4, 41);
    index.insert(8, 80);
    index.insert(6, 60);
}

template <typename Index>
void exercise_common_mutations(Index& index) {
    seed_index(index);
    index.erase(4, 40);
    index.insert(2, 20);
}

template <typename Index>
void require_common_queries(Index& index) {
    exercise_common_mutations(index);

    REQUIRE(index.find(1) == std::vector<ecs::Entity>{10});
    REQUIRE(index.find(4) == std::vector<ecs::Entity>{41});
    REQUIRE(index.find(7).empty());
    REQUIRE(index.find_less_than(4, false) == std::vector<ecs::Entity>{10, 20});
    REQUIRE(index.find_less_than(4, true) == std::vector<ecs::Entity>{10, 20, 41});
    REQUIRE(index.find_greater_than(4, false) == std::vector<ecs::Entity>{60, 80});
    REQUIRE(index.find_greater_than(4, true) == std::vector<ecs::Entity>{41, 60, 80});
    REQUIRE(index.find_not_equal(4) == std::vector<ecs::Entity>{10, 20, 60, 80});
    REQUIRE(index.find_one(8) == 80);
    REQUIRE(index.find_one(9) == ecs::null_entity);
}

}  // namespace

TEST_CASE("all index backends preserve the same query semantics") {
    ecs::detail::DeferredBPlusIndex<std::int32_t> deferred;
    ecs::detail::OptimizedBPlusIndex<std::int32_t> optimized;
    ecs::detail::FlatSortedIndex<std::int32_t> flat;

    require_common_queries(deferred);
    require_common_queries(optimized);
    require_common_queries(flat);
}

TEST_CASE("all index backends enforce unique indexes consistently") {
    ecs::detail::DeferredBPlusIndex<std::tuple<std::int32_t, std::int32_t>> deferred(true);
    ecs::detail::OptimizedBPlusIndex<std::tuple<std::int32_t, std::int32_t>> optimized(true);
    ecs::detail::FlatSortedIndex<std::tuple<std::int32_t, std::int32_t>> flat(true);

    const std::tuple<std::int32_t, std::int32_t> key{5, 7};
    deferred.insert(key, 1);
    optimized.insert(key, 1);
    flat.insert(key, 1);

    REQUIRE_THROWS_AS(deferred.insert(key, 2), std::invalid_argument);
    REQUIRE_THROWS_AS(optimized.insert(key, 2), std::invalid_argument);
    REQUIRE_THROWS_AS(flat.insert(key, 2), std::invalid_argument);
}

TEST_CASE("all index backends return duplicate equality hits that span multiple leaves") {
    ecs::detail::DeferredBPlusIndex<std::int32_t> deferred;
    ecs::detail::OptimizedBPlusIndex<std::int32_t> optimized;
    ecs::detail::FlatSortedIndex<std::int32_t> flat;

    for (ecs::Entity entity = 0; entity < 512; ++entity) {
        deferred.insert(32, entity);
        optimized.insert(32, entity);
        flat.insert(32, entity);
    }

    const auto deferred_matches = deferred.find(32);
    const auto optimized_matches = optimized.find(32);
    const auto flat_matches = flat.find(32);

    REQUIRE(deferred_matches.size() == 512);
    REQUIRE(optimized_matches.size() == 512);
    REQUIRE(flat_matches.size() == 512);
    REQUIRE(deferred_matches == flat_matches);
    REQUIRE(optimized_matches == flat_matches);
}
