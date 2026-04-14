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
    registry.emplace<Position>(player, Position{10.0f, 20.0f});

    const Position& position = registry.get<Position>(player);
    std::cout << "entity " << player << " -> (" << position.x << ", " << position.y << ")\n";

    return 0;
}
