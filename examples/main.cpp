#include "ecs/ecs.hpp"

#include <iostream>

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

int main() {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();

    registry.add<Position>(entity, Position{1.0f, 2.0f});

    registry.write<Position>(entity).x += 3.0f;

    if (!registry.contains<Position>(entity)) {
        return 1;
    }

    const Position& position = registry.get<Position>(entity);
    std::cout << position.x << ", " << position.y << '\n';
    return 0;
}
