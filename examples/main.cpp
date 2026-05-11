#include "ashiato/ashiato.hpp"

#include <iostream>

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

int main() {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();

    registry.add<Position>(entity, Position{1.0f, 2.0f});

    registry.write<Position>(entity).x += 3.0f;

    if (!registry.contains<Position>(entity)) {
        return 1;
    }

    const Position& position = registry.get<Position>(entity);
    std::cout << position.x << ", " << position.y << '\n';
    return 0;
}
