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

    if (Position* position = registry.write<Position>(entity)) {
        position->x += 3.0f;
    }

    const Position* position = registry.get<Position>(entity);
    if (position == nullptr) {
        return 1;
    }

    std::cout << position->x << ", " << position->y << '\n';
    return 0;
}
