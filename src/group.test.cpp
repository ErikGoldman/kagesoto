#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

#include "ecs/ecs.hpp"

namespace {

struct Position {
    int x;
    int y;
};

struct Velocity {
    int dx;
    int dy;
};

struct Health {
    int value;
};

}  // namespace

TEST_CASE("owning groups keep existing storage and view APIs transparent") {
    ecs::Registry registry(4);

    const ecs::Entity grouped_with_health = registry.create();
    const ecs::Entity grouped = registry.create();
    const ecs::Entity position_only = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity, Health>();
        tx.write<Position>(grouped_with_health, Position{1, 2});
        tx.write<Velocity>(grouped_with_health, Velocity{3, 4});
        tx.write<Health>(grouped_with_health, Health{10});
        tx.write<Position>(grouped, Position{5, 6});
        tx.write<Velocity>(grouped, Velocity{7, 8});
        tx.write<Position>(position_only, Position{9, 10});
        tx.commit();
    }

    registry.group<Position, Velocity>();

    auto tx = registry.transaction<Position, Velocity, Health>();

    std::vector<ecs::Entity> positions;
    tx.storage<Position>().each([&](ecs::Entity entity, const Position&) {
        positions.push_back(entity);
    });
    std::sort(positions.begin(), positions.end());
    REQUIRE(positions == std::vector<ecs::Entity>{grouped_with_health, grouped, position_only});

    std::vector<ecs::Entity> grouped_view;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity entity, const Position&, const Velocity&) {
        grouped_view.push_back(entity);
    });
    std::sort(grouped_view.begin(), grouped_view.end());
    REQUIRE(grouped_view == std::vector<ecs::Entity>{grouped_with_health, grouped});

    const ecs::QueryExplain explain = tx.view<const Position, const Velocity, const Health>().explain();
    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.anchor_component == ecs::component_id<Health>());
    REQUIRE(explain.candidate_rows == 1);
    REQUIRE(explain.candidates.size() == 3);
    REQUIRE(std::count_if(explain.candidates.begin(), explain.candidates.end(), [](const ecs::QuerySourceCandidate& candidate) {
        return candidate.source == ecs::QuerySourceKind::owning_group;
    }) == 0);

    const std::string explain_text = tx.view<const Position, const Velocity, const Health>().explain_text();
    REQUIRE(explain_text.find("Source candidates:") != std::string::npos);
    REQUIRE(explain_text.find("group component[0] Position, rows=2, covers=2 (not chosen)") == std::string::npos);
    REQUIRE(explain_text.find("Anchor: component[2] Health via anchor scan") != std::string::npos);
}

TEST_CASE("owning groups are chosen as the view anchor when they are the cheapest source") {
    ecs::Registry registry(4);

    const ecs::Entity grouped = registry.create();
    {
        auto tx = registry.transaction<Position, Velocity, Health>();
        tx.write<Position>(grouped, Position{1, 2});
        tx.write<Velocity>(grouped, Velocity{3, 4});
        tx.commit();
    }

    registry.group<Position, Velocity>();

    auto tx = registry.transaction<Position, Velocity, Health>();
    const ecs::QueryExplain explain = tx.view<const Position, const Velocity>().explain();
    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.anchor_component == ecs::component_id<Position>());
    REQUIRE(explain.anchor_component_index == 0);
    REQUIRE(explain.candidate_rows == 1);
    REQUIRE(explain.estimated_entity_lookups == 0);
    REQUIRE(explain.steps.size() == 2);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE(explain.steps[1].access == ecs::QueryAccessKind::grouped_fetch);
    REQUIRE(std::count_if(explain.candidates.begin(), explain.candidates.end(), [](const ecs::QuerySourceCandidate& candidate) {
        return candidate.chosen && candidate.source == ecs::QuerySourceKind::owning_group;
    }) == 1);

    const std::string explain_text = tx.view<const Position, const Velocity>().explain_text();
    REQUIRE(explain_text.find("group component[0] Position, rows=1, covers=2 (chosen)") != std::string::npos);
    REQUIRE(explain_text.find("group row fetch") != std::string::npos);
}

TEST_CASE("owning groups promote committed MVCC writes into isolated group storage while staged queries still work") {
    ecs::Registry registry(4);

    const ecs::Entity grouped = registry.create();
    const ecs::Entity partial = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(grouped, Position{1, 2});
        tx.write<Velocity>(grouped, Velocity{3, 4});
        tx.write<Position>(partial, Position{5, 6});
        tx.commit();
    }

    registry.group<Position, Velocity>();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Velocity>(partial, Velocity{7, 8});

        std::vector<ecs::Entity> seen;
        tx.view<const Position, const Velocity>().forEach([&](ecs::Entity entity, const Position&, const Velocity&) {
            seen.push_back(entity);
        });
        std::sort(seen.begin(), seen.end());
        REQUIRE(seen == std::vector<ecs::Entity>{grouped, partial});

        tx.rollback();
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        std::vector<ecs::Entity> seen;
        tx.view<const Position, const Velocity>().forEach([&](ecs::Entity entity, const Position&, const Velocity&) {
            seen.push_back(entity);
        });
        REQUIRE(seen == std::vector<ecs::Entity>{grouped});
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Velocity>(partial, Velocity{7, 8});
        tx.commit();
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        std::vector<ecs::Entity> positions;
        tx.storage<Position>().each([&](ecs::Entity entity, const Position&) {
            positions.push_back(entity);
        });
        std::sort(positions.begin(), positions.end());
        REQUIRE(positions == std::vector<ecs::Entity>{grouped, partial});

        std::vector<ecs::Entity> seen;
        tx.view<const Position, const Velocity>().forEach([&](ecs::Entity entity, const Position&, const Velocity&) {
            seen.push_back(entity);
        });
        std::sort(seen.begin(), seen.end());
        REQUIRE(seen == std::vector<ecs::Entity>{grouped, partial});
    }

    REQUIRE(registry.remove<Velocity>(partial));

    {
        auto tx = registry.transaction<Position, Velocity>();
        REQUIRE(tx.has<Position>(partial));
        REQUIRE_FALSE(tx.has<Velocity>(partial));

        std::vector<ecs::Entity> positions;
        tx.storage<Position>().each([&](ecs::Entity entity, const Position&) {
            positions.push_back(entity);
        });
        std::sort(positions.begin(), positions.end());
        REQUIRE(positions == std::vector<ecs::Entity>{grouped, partial});

        std::vector<ecs::Entity> seen;
        tx.view<const Position, const Velocity>().forEach([&](ecs::Entity entity, const Position&, const Velocity&) {
            seen.push_back(entity);
        });
        REQUIRE(seen == std::vector<ecs::Entity>{grouped});
    }
}

TEST_CASE("owning groups enforce shared storage modes and exclusive ownership") {
    ecs::Registry mismatched(4);
    mismatched.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);
    REQUIRE_THROWS_AS((mismatched.group<Position, Velocity>()), std::logic_error);

    ecs::Registry registry(4);
    registry.group<Position, Velocity>();
    REQUIRE_THROWS_AS((registry.group<Velocity, Health>()), std::logic_error);
}

TEST_CASE("trace ondemand owning groups preserve history and rollback while group membership stays intact") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(8);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace_ondemand);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::trace_ondemand);
    registry.group<Position, Velocity>();

    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{7, 8});
        tx.commit();
    }

    std::vector<ecs::Timestamp> timestamps;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        timestamps.push_back(info.timestamp);
    });
    REQUIRE(timestamps == std::vector<ecs::Timestamp>{1, 3});

    registry.set_current_trace_time(4);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 1));

    auto tx = registry.transaction<Position, Velocity>();
    std::vector<ecs::Entity> seen;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity current, const Position& position, const Velocity& velocity) {
        seen.push_back(current);
        REQUIRE(position.x == 1);
        REQUIRE(position.y == 2);
        REQUIRE(velocity.dx == 3);
        REQUIRE(velocity.dy == 4);
    });
    REQUIRE(seen == std::vector<ecs::Entity>{entity});
}

TEST_CASE("trace ondemand owning groups demote correctly when committed removal breaks membership") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(8);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace_ondemand);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::trace_ondemand);
    registry.group<Position, Velocity>();

    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    REQUIRE(registry.remove<Velocity>(entity));

    auto tx = registry.transaction<Position, Velocity>();
    REQUIRE(tx.has<Position>(entity));
    REQUIRE_FALSE(tx.has<Velocity>(entity));

    std::vector<ecs::Entity> positions;
    tx.storage<Position>().each([&](ecs::Entity current, const Position&) {
        positions.push_back(current);
    });
    REQUIRE(positions == std::vector<ecs::Entity>{entity});

    std::vector<ecs::Entity> grouped;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity current, const Position&, const Velocity&) {
        grouped.push_back(current);
    });
    REQUIRE(grouped.empty());
}

TEST_CASE("trace preallocate owning groups expose retained frames and keep queries correct after rollback") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace_preallocate);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::trace_preallocate);
    registry.group<Position, Velocity>();

    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{10, 20});
        tx.write<Velocity>(entity, Velocity{30, 40});
        tx.commit();
    }

    std::vector<ecs::Timestamp> timestamps;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        timestamps.push_back(info.timestamp);
    });
    REQUIRE(timestamps == std::vector<ecs::Timestamp>{1});

    registry.set_current_trace_time(3);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 1));

    auto tx = registry.transaction<Position, Velocity>();
    std::vector<ecs::Entity> seen;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity current, const Position& position, const Velocity& velocity) {
        seen.push_back(current);
        REQUIRE(position.x == 1);
        REQUIRE(position.y == 2);
        REQUIRE(velocity.dx == 30);
        REQUIRE(velocity.dy == 40);
    });
    REQUIRE(seen == std::vector<ecs::Entity>{entity});
}

TEST_CASE("trace preallocate owning groups demote correctly when committed removal breaks membership") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace_preallocate);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::trace_preallocate);
    registry.group<Position, Velocity>();

    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    REQUIRE(registry.remove<Velocity>(entity));

    auto tx = registry.transaction<Position, Velocity>();
    REQUIRE(tx.has<Position>(entity));
    REQUIRE_FALSE(tx.has<Velocity>(entity));

    std::vector<ecs::Entity> positions;
    tx.storage<Position>().each([&](ecs::Entity current, const Position&) {
        positions.push_back(current);
    });
    REQUIRE(positions == std::vector<ecs::Entity>{entity});

    std::vector<ecs::Entity> grouped;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity current, const Position&, const Velocity&) {
        grouped.push_back(current);
    });
    REQUIRE(grouped.empty());
}
