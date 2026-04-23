#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <vector>

#include "ecs/ecs.hpp"

namespace {

struct IndexedPosition {
    std::int32_t x;
    std::int32_t y;
};

struct Velocity {
    std::int32_t dx;
    std::int32_t dy;
};

using XIndex = ecs::Index<&IndexedPosition::x>;
using XYUniqueIndex = ecs::UniqueIndex<&IndexedPosition::x, &IndexedPosition::y>;

ecs::Registry make_index_mvcc_registry() {
    ecs::Registry registry(4);
    registry.set_storage_mode<IndexedPosition>(ecs::ComponentStorageMode::mvcc);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::mvcc);
    return registry;
}

}  // namespace

namespace ecs {

template <>
struct ComponentIndices<IndexedPosition> {
    using type = std::tuple<XIndex, XYUniqueIndex>;
};

}  // namespace ecs

TEST_CASE("transaction storage supports indexed lookup on single and compound keys") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_all<&IndexedPosition::x>(10) == std::vector<ecs::Entity>{first, second});
    REQUIRE(positions.find_all<&IndexedPosition::x, &IndexedPosition::y>(10, 2) == std::vector<ecs::Entity>{second});
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(30, 3) == third);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(99, 99) == ecs::null_entity);
}

TEST_CASE("transaction storage lookup can scan non indexed fields and return first matches") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_all<&IndexedPosition::y>(2) == std::vector<ecs::Entity>{second, third});
    REQUIRE(positions.find_one<&IndexedPosition::x>(10) == first);
    REQUIRE(positions.find_one<&IndexedPosition::y>(2) == second);
    REQUIRE(positions.find_one<&IndexedPosition::y>(99) == ecs::null_entity);
}

TEST_CASE("component indexes enforce uniqueness constraints") {
    auto registry = make_index_mvcc_registry();

    const ecs::Entity first = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{5, 7});
        tx.commit();
    }

    const ecs::Entity second = registry.create();
    auto tx = registry.transaction<IndexedPosition, Velocity>();
    tx.write<IndexedPosition>(second, IndexedPosition{5, 7});
    REQUIRE_THROWS_AS(tx.commit(), std::invalid_argument);
}

TEST_CASE("component indexes stay in sync for remove and replace") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(entity, IndexedPosition{1, 2});
        tx.commit();
    }

    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(1, 2) == entity);
    }

    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(entity, IndexedPosition{8, 9});
        tx.commit();
    }

    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(1, 2) == ecs::null_entity);
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(8, 9) == entity);
    }

    REQUIRE(registry.remove<IndexedPosition>(entity));

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(8, 9) == ecs::null_entity);
}

TEST_CASE("transaction storage exposes staged indexed inserts before commit") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    IndexedPosition* staged = tx.write<IndexedPosition>(entity, IndexedPosition{11, 22});
    REQUIRE(staged != nullptr);

    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_all<&IndexedPosition::x>(11) == std::vector<ecs::Entity>{entity});
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(11, 22) == entity);

    tx.commit();

    auto read_tx = registry.transaction<IndexedPosition, Velocity>();
    auto committed_positions = read_tx.storage<IndexedPosition>();
    REQUIRE(committed_positions.find_all<&IndexedPosition::x>(11) == std::vector<ecs::Entity>{entity});
    REQUIRE(committed_positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(11, 22) == entity);
}

TEST_CASE("transaction storage switches indexed keys immediately within the transaction") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(entity, IndexedPosition{3, 4});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    IndexedPosition* staged = tx.write<IndexedPosition>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 30;
    staged->y = 40;

    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_all<&IndexedPosition::x>(3).empty());
    REQUIRE(positions.find_all<&IndexedPosition::x>(30) == std::vector<ecs::Entity>{entity});
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(3, 4) == ecs::null_entity);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(30, 40) == entity);

    tx.commit();

    auto read_tx = registry.transaction<IndexedPosition, Velocity>();
    auto committed_positions = read_tx.storage<IndexedPosition>();
    REQUIRE(committed_positions.find_all<&IndexedPosition::x>(3).empty());
    REQUIRE(committed_positions.find_all<&IndexedPosition::x>(30) == std::vector<ecs::Entity>{entity});
    REQUIRE(committed_positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(3, 4) == ecs::null_entity);
    REQUIRE(committed_positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(30, 40) == entity);
}

TEST_CASE("component row reuse after remove does not leak stale index state") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{12, 34});
        tx.commit();
    }

    REQUIRE(registry.remove<IndexedPosition>(first));

    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(second, IndexedPosition{56, 78});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE_FALSE(tx.has<IndexedPosition>(first));
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(12, 34) == ecs::null_entity);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(56, 78) == second);
}

TEST_CASE("concurrent transactions enforce unique indexes against newly committed rows") {
    auto registry = make_index_mvcc_registry();

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    auto older = registry.transaction<IndexedPosition, Velocity>();
    auto newer = registry.transaction<IndexedPosition, Velocity>();

    older.write<IndexedPosition>(first, IndexedPosition{90, 91});
    newer.write<IndexedPosition>(second, IndexedPosition{90, 91});

    newer.commit();
    REQUIRE_THROWS_AS(older.commit(), std::invalid_argument);

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(90, 91) == second);
}

TEST_CASE("trace indexed components keep indexes in sync across rollback tombstones and compaction") {
    ecs::Registry registry(4);
    registry.set_storage_mode<IndexedPosition>(ecs::ComponentStorageMode::trace);
    registry.set_trace_max_history(5);

    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<IndexedPosition>();
        tx.write<IndexedPosition>(entity, IndexedPosition{10, 11});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<IndexedPosition>();
        tx.write<IndexedPosition>(entity, IndexedPosition{20, 21});
        tx.commit();
    }

    {
        auto tx = registry.transaction<IndexedPosition>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 11) == ecs::null_entity);
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(20, 21) == entity);
    }

    registry.set_current_trace_time(3);
    REQUIRE(registry.rollback_to_timestamp<IndexedPosition>(entity, 1));

    {
        auto tx = registry.transaction<IndexedPosition>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 11) == entity);
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(20, 21) == ecs::null_entity);
    }

    registry.set_current_trace_time(4);
    REQUIRE(registry.remove<IndexedPosition>(entity));

    {
        auto tx = registry.transaction<IndexedPosition>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 11) == ecs::null_entity);
        REQUIRE(positions.find_all<&IndexedPosition::x>(10).empty());
    }

    registry.set_current_trace_time(7);
    {
        auto tx = registry.transaction<IndexedPosition>();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 11) == ecs::null_entity);
        REQUIRE(positions.find_all<&IndexedPosition::x>(10).empty());
    }

    registry.set_current_trace_time(8);
    REQUIRE(registry.rollback_to_timestamp<IndexedPosition>(entity, 3));

    auto tx = registry.transaction<IndexedPosition>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(10, 11) == entity);
    REQUIRE(positions.find_all<&IndexedPosition::x>(10) == std::vector<ecs::Entity>{entity});
}

TEST_CASE("trace indexed components compact history independently per entity") {
    ecs::Registry registry(4);
    registry.set_storage_mode<IndexedPosition>(ecs::ComponentStorageMode::trace);
    registry.set_trace_max_history(2);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<IndexedPosition>();
        tx.write<IndexedPosition>(first, IndexedPosition{1, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{2, 2});
        tx.commit();
    }

    registry.set_current_trace_time(4);
    {
        auto tx = registry.transaction<IndexedPosition>();
        tx.write<IndexedPosition>(first, IndexedPosition{4, 4});
        tx.write<IndexedPosition>(second, IndexedPosition{5, 5});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition>();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(4, 4) == first);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(5, 5) == second);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(1, 1) == ecs::null_entity);
    REQUIRE(positions.find_one<&IndexedPosition::x, &IndexedPosition::y>(2, 2) == ecs::null_entity);
}

TEST_CASE("transaction storage explain prefers an index seek for keyed lookups") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    const ecs::QueryExplain explain = positions.explain_find<&IndexedPosition::x>(10);

    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.uses_component_indexes);
    REQUIRE(explain.anchor_component == ecs::component_id<IndexedPosition>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.estimated_entity_lookups == 0);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::index_seek);
    REQUIRE(explain.steps[0].uses_component_index);
    REQUIRE(explain.steps[0].visible_rows == 3);
}

TEST_CASE("transaction storage explain prefers a scan for full iteration even with indexes") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    const ecs::QueryExplain explain = positions.explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE_FALSE(explain.uses_component_indexes);
    REQUIRE(explain.anchor_component == ecs::component_id<IndexedPosition>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.estimated_entity_lookups == 0);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE_FALSE(explain.steps[0].uses_component_index);
    REQUIRE(explain.steps[0].visible_rows == 2);
}

TEST_CASE("transaction storage explain scans when a field lookup is not selective") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    auto positions = tx.storage<IndexedPosition>();
    const ecs::QueryExplain explain = positions.explain_find<&IndexedPosition::x>(10);

    REQUIRE_FALSE(explain.empty);
    REQUIRE_FALSE(explain.uses_component_indexes);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE_FALSE(explain.steps[0].uses_component_index);
}

TEST_CASE("plain view explain stays on scans without a selective predicate even if indexes exist") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain explain = tx.view<const IndexedPosition>().explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE_FALSE(explain.uses_component_indexes);
    REQUIRE(explain.anchor_component == ecs::component_id<IndexedPosition>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 3);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE_FALSE(explain.steps[0].uses_component_index);
}

TEST_CASE("predicate view explain uses an index for selective equality") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain explain = tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.uses_component_indexes);
    REQUIRE(explain.anchor_component == ecs::component_id<IndexedPosition>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::index_seek);
    REQUIRE(explain.steps[0].uses_component_index);
}

TEST_CASE("predicate view explain keeps a scan for unselective equality") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain explain = tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE_FALSE(explain.uses_component_indexes);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE_FALSE(explain.steps[0].uses_component_index);
}

TEST_CASE("predicate view explain uses an index for selective ranges and scans for not-equal") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{1, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{2, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{3, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{100, 4});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain gt_explain = tx.view<const IndexedPosition>().where_gt<&IndexedPosition::x>(50).explain();
    REQUIRE_FALSE(gt_explain.empty);
    REQUIRE(gt_explain.uses_component_indexes);
    REQUIRE(gt_explain.candidate_rows == 1);
    REQUIRE(gt_explain.steps[0].access == ecs::QueryAccessKind::index_seek);

    const ecs::QueryExplain ne_explain = tx.view<const IndexedPosition>().where_ne<&IndexedPosition::x>(2).explain();
    REQUIRE_FALSE(ne_explain.empty);
    REQUIRE_FALSE(ne_explain.uses_component_indexes);
    REQUIRE(ne_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
}

TEST_CASE("predicate views filter correctly and include staged rows") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    tx.write<IndexedPosition>(third, IndexedPosition{30, 3});

    std::vector<ecs::Entity> seen;
    tx.view<const IndexedPosition>()
        .where_gte<&IndexedPosition::x>(20)
        .where_lte<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            seen.push_back(entity);
        });

    REQUIRE(seen == std::vector<ecs::Entity>{second, third});
}

TEST_CASE("predicate views support OR across indexed predicates") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{40, 4});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    std::vector<ecs::Entity> seen;
    tx.view<const IndexedPosition>()
        .where_eq<&IndexedPosition::x>(10)
        .or_where_eq<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            seen.push_back(entity);
        });

    REQUIRE(seen == std::vector<ecs::Entity>{first, third});

    const ecs::QueryExplain explain =
        tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).or_where_eq<&IndexedPosition::x>(30).explain();
    REQUIRE(explain.uses_component_indexes);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::index_seek);
}

TEST_CASE("predicate views support arbitrary nested boolean groups") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    const ecs::Entity fifth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{40, 4});
        tx.write<IndexedPosition>(fifth, IndexedPosition{50, 5});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    std::vector<ecs::Entity> seen;
    tx.view<const IndexedPosition>()
        .and_group([&](auto q) {
            return q.template where_gte<&IndexedPosition::x>(20)
                .and_group([&](auto nested) {
                    return nested.template where_lte<&IndexedPosition::x>(40)
                        .template or_where_eq<&IndexedPosition::x>(50);
                });
        })
        .where_ne<&IndexedPosition::x>(50)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            seen.push_back(entity);
        });

    REQUIRE(seen == std::vector<ecs::Entity>{second, third, fourth});
}

TEST_CASE("predicate view explain uses indexes for selective lt lte and gte ranges") {
    ecs::Registry registry(8);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    const ecs::Entity fifth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{40, 4});
        tx.write<IndexedPosition>(fifth, IndexedPosition{50, 5});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();

    const ecs::QueryExplain lt_explain = tx.view<const IndexedPosition>().where_lt<&IndexedPosition::x>(15).explain();
    REQUIRE_FALSE(lt_explain.empty);
    REQUIRE(lt_explain.uses_component_indexes);
    REQUIRE(lt_explain.candidate_rows == 1);
    REQUIRE(lt_explain.steps[0].access == ecs::QueryAccessKind::index_seek);

    const ecs::QueryExplain lte_explain = tx.view<const IndexedPosition>().where_lte<&IndexedPosition::x>(20).explain();
    REQUIRE_FALSE(lte_explain.empty);
    REQUIRE(lte_explain.uses_component_indexes);
    REQUIRE(lte_explain.candidate_rows == 2);
    REQUIRE(lte_explain.steps[0].access == ecs::QueryAccessKind::index_seek);

    const ecs::QueryExplain gte_explain = tx.view<const IndexedPosition>().where_gte<&IndexedPosition::x>(50).explain();
    REQUIRE_FALSE(gte_explain.empty);
    REQUIRE(gte_explain.uses_component_indexes);
    REQUIRE(gte_explain.candidate_rows == 1);
    REQUIRE(gte_explain.steps[0].access == ecs::QueryAccessKind::index_seek);
}

TEST_CASE("predicate view explain keeps scans for non indexed members and not equal") {
    ecs::Registry registry(8);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 100});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 200});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 300});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();

    const ecs::QueryExplain y_eq_explain = tx.view<const IndexedPosition>().where_eq<&IndexedPosition::y>(200).explain();
    REQUIRE_FALSE(y_eq_explain.empty);
    REQUIRE_FALSE(y_eq_explain.uses_component_indexes);
    REQUIRE(y_eq_explain.candidate_rows == 3);
    REQUIRE(y_eq_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE_FALSE(y_eq_explain.steps[0].uses_component_index);

    const ecs::QueryExplain y_gt_explain = tx.view<const IndexedPosition>().where_gt<&IndexedPosition::y>(150).explain();
    REQUIRE_FALSE(y_gt_explain.empty);
    REQUIRE_FALSE(y_gt_explain.uses_component_indexes);
    REQUIRE(y_gt_explain.candidate_rows == 3);
    REQUIRE(y_gt_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);

    const ecs::QueryExplain ne_explain = tx.view<const IndexedPosition>().where_ne<&IndexedPosition::x>(10).explain();
    REQUIRE_FALSE(ne_explain.empty);
    REQUIRE_FALSE(ne_explain.uses_component_indexes);
    REQUIRE(ne_explain.candidate_rows == 3);
    REQUIRE(ne_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
}

TEST_CASE("predicate planner uses indexed seeds for mixed AND but scans for mixed OR") {
    ecs::Registry registry(8);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{20, 2});
        tx.write<IndexedPosition>(fourth, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();

    const ecs::QueryExplain and_explain =
        tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).where_eq<&IndexedPosition::y>(2).explain();
    REQUIRE_FALSE(and_explain.empty);
    REQUIRE(and_explain.uses_component_indexes);
    REQUIRE(and_explain.candidate_rows == 2);
    REQUIRE(and_explain.steps[0].access == ecs::QueryAccessKind::index_seek);

    std::vector<ecs::Entity> and_seen;
    tx.view<const IndexedPosition>()
        .where_eq<&IndexedPosition::x>(10)
        .where_eq<&IndexedPosition::y>(2)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            and_seen.push_back(entity);
        });
    REQUIRE(and_seen == std::vector<ecs::Entity>{second});

    const ecs::QueryExplain or_explain =
        tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).or_where_eq<&IndexedPosition::y>(2).explain();
    REQUIRE_FALSE(or_explain.empty);
    REQUIRE_FALSE(or_explain.uses_component_indexes);
    REQUIRE(or_explain.candidate_rows == 4);
    REQUIRE(or_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);

    std::vector<ecs::Entity> or_seen;
    tx.view<const IndexedPosition>()
        .where_eq<&IndexedPosition::x>(10)
        .or_where_eq<&IndexedPosition::y>(2)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            or_seen.push_back(entity);
        });
    REQUIRE(or_seen == std::vector<ecs::Entity>{first, second, third});
}

TEST_CASE("predicate planner unions overlapping OR seeds without duplicates and intersects overlapping AND seeds") {
    ecs::Registry registry(8);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    const ecs::Entity fifth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{40, 4});
        tx.write<IndexedPosition>(fifth, IndexedPosition{50, 5});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();

    std::vector<ecs::Entity> or_seen;
    tx.view<const IndexedPosition>()
        .where_gte<&IndexedPosition::x>(20)
        .or_where_lte<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            or_seen.push_back(entity);
        });
    REQUIRE(or_seen == std::vector<ecs::Entity>{first, second, third, fourth, fifth});

    const ecs::QueryExplain or_explain =
        tx.view<const IndexedPosition>().where_gte<&IndexedPosition::x>(20).or_where_lte<&IndexedPosition::x>(30).explain();
    REQUIRE_FALSE(or_explain.empty);
    REQUIRE_FALSE(or_explain.uses_component_indexes);
    REQUIRE(or_explain.candidate_rows == 5);
    REQUIRE(or_explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);

    std::vector<ecs::Entity> and_seen;
    tx.view<const IndexedPosition>()
        .where_gte<&IndexedPosition::x>(20)
        .where_lte<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            and_seen.push_back(entity);
        });
    REQUIRE(and_seen == std::vector<ecs::Entity>{second, third});

    const ecs::QueryExplain and_explain =
        tx.view<const IndexedPosition>().where_gte<&IndexedPosition::x>(20).where_lte<&IndexedPosition::x>(30).explain();
    REQUIRE_FALSE(and_explain.empty);
    REQUIRE(and_explain.uses_component_indexes);
    REQUIRE(and_explain.candidate_rows == 2);
    REQUIRE(and_explain.steps[0].access == ecs::QueryAccessKind::index_seek);
}

TEST_CASE("predicate planner uses index seeks for selective empty results") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain explain = tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(999).explain();

    REQUIRE(explain.empty);
    REQUIRE(explain.uses_component_indexes);
    REQUIRE(explain.candidate_rows == 0);
    REQUIRE(explain.estimated_entity_lookups == 0);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::index_seek);
    REQUIRE(explain.steps[0].uses_component_index);
}

TEST_CASE("predicate view explain marks index seek and sparse probes for multi component views") {
    ecs::Registry registry(8);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    const ecs::Entity fourth = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<Velocity>(first, Velocity{1, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<Velocity>(second, Velocity{2, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{10, 3});
        tx.write<IndexedPosition>(fourth, IndexedPosition{40, 4});
        tx.write<Velocity>(fourth, Velocity{4, 4});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();
    const ecs::QueryExplain explain =
        tx.view<const IndexedPosition, const Velocity>().where_eq<&IndexedPosition::x>(40).explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.uses_component_indexes);
    REQUIRE(explain.anchor_component == ecs::component_id<IndexedPosition>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 1);
    REQUIRE(explain.estimated_entity_lookups == 1);
    REQUIRE(explain.steps.size() == 2);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::index_seek);
    REQUIRE(explain.steps[0].uses_component_index);
    REQUIRE(explain.steps[1].access == ecs::QueryAccessKind::sparse_lookup);
    REQUIRE_FALSE(explain.steps[1].uses_component_index);

    std::vector<ecs::Entity> seen;
    tx.view<const IndexedPosition, const Velocity>()
        .where_eq<&IndexedPosition::x>(40)
        .forEach([&](ecs::Entity entity, const IndexedPosition&, const Velocity&) {
            seen.push_back(entity);
        });
    REQUIRE(seen == std::vector<ecs::Entity>{fourth});
}

TEST_CASE("predicate views keep staged updates consistent across predicate transitions") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<IndexedPosition, Velocity>();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{20, 2});
        tx.commit();
    }

    auto tx = registry.transaction<IndexedPosition, Velocity>();

    IndexedPosition* first_update = tx.write<IndexedPosition>(first);
    REQUIRE(first_update != nullptr);
    first_update->x = 15;
    first_update->y = 10;

    IndexedPosition* second_update = tx.write<IndexedPosition>(second);
    REQUIRE(second_update != nullptr);
    second_update->x = 30;
    second_update->y = 20;

    std::vector<ecs::Entity> eq_seen;
    tx.view<const IndexedPosition>()
        .where_eq<&IndexedPosition::x>(10)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            eq_seen.push_back(entity);
        });
    REQUIRE(eq_seen.empty());

    std::vector<ecs::Entity> range_seen;
    tx.view<const IndexedPosition>()
        .where_gte<&IndexedPosition::x>(15)
        .where_lte<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            range_seen.push_back(entity);
        });
    REQUIRE(range_seen == std::vector<ecs::Entity>{first, second});

    std::vector<ecs::Entity> or_seen;
    tx.view<const IndexedPosition>()
        .where_eq<&IndexedPosition::x>(10)
        .or_where_eq<&IndexedPosition::x>(30)
        .forEach([&](ecs::Entity entity, const IndexedPosition&) {
            or_seen.push_back(entity);
        });
    REQUIRE(or_seen == std::vector<ecs::Entity>{second});

    const ecs::QueryExplain or_explain =
        tx.view<const IndexedPosition>().where_eq<&IndexedPosition::x>(10).or_where_eq<&IndexedPosition::x>(30).explain();
    REQUIRE_FALSE(or_explain.empty);
    REQUIRE(or_explain.uses_component_indexes);
    REQUIRE(or_explain.candidate_rows == 1);
    REQUIRE(or_explain.steps[0].access == ecs::QueryAccessKind::index_seek);
}
