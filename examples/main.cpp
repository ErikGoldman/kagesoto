#include <iostream>

#include "ecs/ecs.hpp"

namespace {

struct Position {
    float x;
    float y;
};

}  // namespace

int main() {
    ecs::Registry registry;

    const ecs::Entity player = registry.create();
    auto tx = registry.transaction();
    tx.write<Position>(player, Position{10.0f, 20.0f});
    tx.commit();

    auto read_tx = registry.transaction();
    const Position& position = read_tx.get<Position>(player);
    std::cout << "entity " << player << " -> (" << position.x << ", " << position.y << ")\n";

    return 0;
}
