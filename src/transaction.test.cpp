#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <type_traits>

#include "ecs/ecs.hpp"

namespace {

struct Position {
    int x;
    int y;
};

}  // namespace

TEST_CASE("transactions stage copy-on-write updates and commit them atomically per component") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        Position* created = tx.write<Position>(entity, Position{1, 2});
        REQUIRE(created != nullptr);
        tx.commit();
    }

    auto tx = registry.transaction();
    const Position* original = tx.try_get<Position>(entity);
    REQUIRE(original != nullptr);
    REQUIRE(original->x == 1);

    Position* staged = tx.write<Position>(entity);
    REQUIRE(staged != nullptr);
    REQUIRE(staged != original);
    staged->x = 9;
    staged->y = 8;

    REQUIRE(tx.get<Position>(entity).x == 9);
    REQUIRE(tx.get<Position>(entity).y == 8);

    tx.commit();

    auto read_tx = registry.transaction();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 9);
    REQUIRE(committed->y == 8);
}

TEST_CASE("transactions roll back staged writes when not committed") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = 4;
        staged->y = 5;
        REQUIRE(tx.get<Position>(entity).x == 4);
    }

    auto read_tx = registry.transaction();
    REQUIRE_FALSE(read_tx.has<Position>(entity));
}

TEST_CASE("transactions read back changed staged data before commit") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    auto tx = registry.transaction();
    Position* staged = tx.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 41;
    staged->y = 99;

    const Position* observed = tx.try_get<Position>(entity);
    REQUIRE(observed != nullptr);
    REQUIRE(observed == staged);
    REQUIRE(observed->x == 41);
    REQUIRE(observed->y == 99);
    REQUIRE(tx.get<Position>(entity).x == 41);
    REQUIRE(tx.get<Position>(entity).y == 99);

    Position* reopened = tx.write<Position>(entity);
    REQUIRE(reopened == staged);
    REQUIRE(reopened->x == 41);
    REQUIRE(reopened->y == 99);
}

TEST_CASE("transactions discard changed staged data on rollback and preserve the committed value") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{7, 3});
        tx.commit();
    }

    {
        auto tx = registry.transaction();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = -5;
        staged->y = 12;

        REQUIRE(tx.get<Position>(entity).x == -5);
        REQUIRE(tx.get<Position>(entity).y == 12);
    }

    auto read_tx = registry.transaction();
    REQUIRE(read_tx.get<Position>(entity).x == 7);
    REQUIRE(read_tx.get<Position>(entity).y == 3);
}

TEST_CASE("transactions can stage a committed value even when revision value storage reallocates") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    for (int i = 0; i < 64; ++i) {
        auto tx = registry.transaction();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x += 1;
        staged->y += 2;
        tx.commit();
    }

    auto read_tx = registry.transaction();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 65);
    REQUIRE(committed->y == 130);
}

TEST_CASE("transactions reuse the same staged object for multiple writes to one entity component pair") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction();

    Position* first = tx.write<Position>(entity, Position{1, 2});
    REQUIRE(first != nullptr);
    REQUIRE(first->x == 1);
    REQUIRE(first->y == 2);

    Position* second = tx.write<Position>(entity);
    REQUIRE(second == first);
    second->x = 7;
    second->y = 8;

    Position* third = tx.write<Position>(entity, Position{9, 10});
    REQUIRE(third == first);
    REQUIRE(third->x == 9);
    REQUIRE(third->y == 10);
    REQUIRE(tx.get<Position>(entity).x == 9);
    REQUIRE(tx.get<Position>(entity).y == 10);

    tx.commit();

    auto read_tx = registry.transaction();
    REQUIRE(read_tx.get<Position>(entity).x == 9);
    REQUIRE(read_tx.get<Position>(entity).y == 10);
}

TEST_CASE("concurrent transactions are isolated from each others uncommitted writes") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto first = registry.transaction();
    auto second = registry.transaction();

    Position* first_write = first.write<Position>(entity);
    REQUIRE(first_write != nullptr);
    first_write->x = 10;
    first_write->y = 20;

    REQUIRE(first.get<Position>(entity).x == 10);
    REQUIRE(first.get<Position>(entity).y == 20);
    REQUIRE(second.get<Position>(entity).x == 1);
    REQUIRE(second.get<Position>(entity).y == 2);

    Position* second_write = second.write<Position>(entity);
    REQUIRE(second_write != nullptr);
    second_write->x = 30;
    second_write->y = 40;

    REQUIRE(first.get<Position>(entity).x == 10);
    REQUIRE(second.get<Position>(entity).x == 30);
}

TEST_CASE("transactions can commit repeated updates after revision overflow storage reallocates") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{0, 0});
        tx.commit();
    }

    for (int i = 1; i <= 64; ++i) {
        auto tx = registry.transaction();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = i;
        staged->y = i * 10;
        tx.commit();
    }

    auto read_tx = registry.transaction();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 64);
    REQUIRE(committed->y == 640);
}

TEST_CASE("later transactions only observe committed state from earlier transactions") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    {
        auto tx = registry.transaction();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = 50;
        staged->y = 60;
    }

    auto next_tx = registry.transaction();
    REQUIRE(next_tx.get<Position>(entity).x == 1);
    REQUIRE(next_tx.get<Position>(entity).y == 2);

    Position* committed = next_tx.write<Position>(entity);
    REQUIRE(committed != nullptr);
    committed->x = 70;
    committed->y = 80;
    next_tx.commit();

    auto final_tx = registry.transaction();
    REQUIRE(final_tx.get<Position>(entity).x == 70);
    REQUIRE(final_tx.get<Position>(entity).y == 80);
}

TEST_CASE("snapshots keep their original visibility after later commits") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{5, 6});
        tx.commit();
    }

    auto snapshot = registry.snapshot();
    auto writer = registry.transaction();
    Position* staged = writer.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 50;
    staged->y = 60;
    writer.commit();

    REQUIRE(snapshot.get<Position>(entity).x == 5);
    REQUIRE(snapshot.get<Position>(entity).y == 6);

    auto current = registry.transaction();
    REQUIRE(current.get<Position>(entity).x == 50);
    REQUIRE(current.get<Position>(entity).y == 60);
}

TEST_CASE("snapshots exclude transactions that were active when the snapshot opened") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto writer = registry.transaction();
    Position* staged = writer.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 10;
    staged->y = 20;

    auto snapshot = registry.snapshot();
    writer.commit();

    REQUIRE(snapshot.get<Position>(entity).x == 1);
    REQUIRE(snapshot.get<Position>(entity).y == 2);

    auto current = registry.transaction();
    REQUIRE(current.get<Position>(entity).x == 10);
    REQUIRE(current.get<Position>(entity).y == 20);
}

TEST_CASE("later commit wins even when its transaction tsn is older") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto older = registry.transaction();
    auto newer = registry.transaction();

    Position* older_value = older.write<Position>(entity);
    REQUIRE(older_value != nullptr);
    older_value->x = 20;
    older_value->y = 21;

    Position* newer_value = newer.write<Position>(entity);
    REQUIRE(newer_value != nullptr);
    newer_value->x = 30;
    newer_value->y = 31;
    newer.commit();

    auto middle_snapshot = registry.snapshot();
    older.commit();

    REQUIRE(middle_snapshot.get<Position>(entity).x == 30);
    REQUIRE(middle_snapshot.get<Position>(entity).y == 31);

    auto current = registry.transaction();
    REQUIRE(current.get<Position>(entity).x == 20);
    REQUIRE(current.get<Position>(entity).y == 21);
}

TEST_CASE("revision chains remain readable after overflowing inline and overflow storage") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{0, 0});
        tx.commit();
    }

    auto early_snapshot = registry.snapshot();

    for (int i = 1; i <= 8; ++i) {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{i, i * 10});
        tx.commit();
    }

    REQUIRE(early_snapshot.get<Position>(entity).x == 0);
    REQUIRE(early_snapshot.get<Position>(entity).y == 0);

    auto current = registry.transaction();
    REQUIRE(current.get<Position>(entity).x == 8);
    REQUIRE(current.get<Position>(entity).y == 80);
}

TEST_CASE("transactions become unusable after commit or rollback") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    auto committed = registry.transaction();
    committed.write<Position>(entity, Position{1, 2});
    committed.commit();
    REQUIRE_THROWS_AS(committed.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(committed.write<Position>(entity), std::logic_error);

    auto rolled_back = registry.transaction();
    rolled_back.write<Position>(entity, Position{3, 4});
    rolled_back.rollback();
    REQUIRE_THROWS_AS(rolled_back.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(rolled_back.write<Position>(entity), std::logic_error);
}

TEST_CASE("empty transactions release registry guards on commit and rollback") {
    ecs::Registry registry(4);

    {
        auto tx = registry.transaction();
        tx.commit();
    }

    const ecs::Entity created_after_commit = registry.create();
    REQUIRE(created_after_commit != ecs::null_entity);

    {
        auto tx = registry.transaction();
        tx.rollback();
    }

    const ecs::Entity created_after_rollback = registry.create();
    REQUIRE(created_after_rollback != ecs::null_entity);
}

TEST_CASE("many concurrent isolated revisions remain readable through overflow chains") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction();
        seed.write<Position>(entity, Position{0, 0});
        seed.commit();
    }

    std::array<std::unique_ptr<ecs::Transaction>, 9> transactions{};
    for (std::size_t i = 0; i < transactions.size(); ++i) {
        transactions[i] = std::make_unique<ecs::Transaction>(registry);
        Position* staged = transactions[i]->write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = static_cast<int>(i + 1);
        staged->y = static_cast<int>((i + 1) * 100);
    }

    auto snapshot = registry.snapshot();
    REQUIRE(snapshot.get<Position>(entity).x == 0);
    REQUIRE(snapshot.get<Position>(entity).y == 0);

    for (std::size_t i = 0; i < transactions.size(); ++i) {
        REQUIRE(transactions[i]->get<Position>(entity).x == static_cast<int>(i + 1));
        REQUIRE(transactions[i]->get<Position>(entity).y == static_cast<int>((i + 1) * 100));
    }

    for (std::size_t i = 0; i < transactions.size(); ++i) {
        transactions[i]->commit();
    }

    auto current = registry.transaction();
    REQUIRE(current.get<Position>(entity).x == 9);
    REQUIRE(current.get<Position>(entity).y == 900);
}

TEST_CASE("transaction read APIs are const-only") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{7, 3});
        tx.commit();
    }

    auto tx = registry.transaction();
    static_assert(std::is_same_v<decltype(tx.try_get<Position>(entity)), const Position*>);
    static_assert(std::is_same_v<decltype(tx.get<Position>(entity)), const Position&>);
    static_assert(std::is_same_v<decltype(tx.storage<Position>().get(entity)), const Position&>);
}
