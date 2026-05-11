#include "ashiato_test_support.hpp"

#include "ashiato/debug_server.hpp"

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void register_position_fields(ashiato::Registry& registry) {
    const ashiato::Entity position = registry.register_component<Position>("Position");
    REQUIRE(registry.set_component_fields(
        position,
        {
            ashiato::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ashiato::PrimitiveType::I32), 1},
            ashiato::ComponentField{"y", offsetof(Position, y), registry.primitive_type(ashiato::PrimitiveType::I32), 1},
        }));
}

void register_game_time_fields(ashiato::Registry& registry) {
    const ashiato::Entity game_time = registry.register_component<GameTime>("GameTime");
    REQUIRE(registry.set_component_fields(
        game_time,
        {ashiato::ComponentField{"tick", offsetof(GameTime, tick), registry.primitive_type(ashiato::PrimitiveType::I32), 1}}));
}

}  // namespace

#ifndef _WIN32
namespace {

std::string post_graphql(ashiato::DebugServer& server, const std::string& body) {
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    REQUIRE(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const int flags = fcntl(client, F_GETFL, 0);
    REQUIRE(flags >= 0);
    REQUIRE(fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0);

    const std::string request =
        "POST /graphql HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    REQUIRE(send(client, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));

    std::string response;
    char buffer[4096];
    for (int attempt = 0; attempt < 100 && response.find("\r\n\r\n") == std::string::npos; ++attempt) {
        server.poll();
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count > 0) {
            response.append(buffer, static_cast<std::size_t>(count));
        }
    }

    close(client);
    return response;
}

}  // namespace
#endif

TEST_CASE("zero component views enumerate alive entities and support runtime tag filters") {
    ashiato::Registry registry;
    registry.register_component<Active>("Active");

    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    const ashiato::Entity stale = registry.create();
    REQUIRE(registry.add<Active>(second));
    REQUIRE(registry.destroy(stale));

    std::vector<ashiato::Entity> entities;
    registry.view<>().each([&](ashiato::Entity entity) {
        entities.push_back(entity);
    });

    REQUIRE(std::find(entities.begin(), entities.end(), first) != entities.end());
    REQUIRE(std::find(entities.begin(), entities.end(), second) != entities.end());
    REQUIRE(std::find(entities.begin(), entities.end(), stale) == entities.end());

    std::vector<ashiato::Entity> active;
    registry.view<>().with_tags({registry.component<Active>()}).each([&](ashiato::Entity entity) {
        active.push_back(entity);
    });
    REQUIRE(active == std::vector<ashiato::Entity>{second});
}

TEST_CASE("jobs are discoverable through a job tag and expose read only metadata") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    const ashiato::Entity job = registry.job<const Position, Velocity>(7).name("Move test").max_threads(2).each(
        [](ashiato::Entity, const Position&, Velocity&) {});

    std::vector<ashiato::Entity> jobs;
    registry.view<>().with_tags({registry.job_tag()}).each([&](ashiato::Entity current) {
        jobs.push_back(current);
    });
    REQUIRE(jobs == std::vector<ashiato::Entity>{job});

    const std::optional<ashiato::JobInfo> info = registry.job_info(job);
    REQUIRE(info);
    REQUIRE(info->name == "Move test");
    REQUIRE(info->order == 7);
    REQUIRE(info->reads == std::vector<ashiato::Entity>{registry.component<Position>()});
    REQUIRE(info->writes == std::vector<ashiato::Entity>{registry.component<Velocity>()});

    const std::vector<ashiato::Entity> matches = registry.job_matching_entities(job);
    REQUIRE(matches.empty());
    REQUIRE(registry.add<Velocity>(entity, Velocity{}) != nullptr);
    REQUIRE(registry.job_matching_entities(job) == std::vector<ashiato::Entity>{entity});
}

TEST_CASE("registry exposes components present on an entity") {
    ashiato::Registry registry;
    register_position_fields(registry);
    registry.register_component<Active>("Active");
    registry.register_component<GameTime>("GameTime");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{3, 4}) != nullptr);
    REQUIRE(registry.add<Active>(entity));

    const std::vector<ashiato::EntityComponentInfo> components = registry.components(entity);
    const auto position = std::find_if(components.begin(), components.end(), [&](const ashiato::EntityComponentInfo& info) {
        return info.component == registry.component<Position>();
    });
    REQUIRE(position != components.end());
    REQUIRE(position->name == "Position");
    REQUIRE(position->debug_value == "Position{x=3, y=4}");

    const auto active = std::find_if(components.begin(), components.end(), [&](const ashiato::EntityComponentInfo& info) {
        return info.component == registry.component<Active>();
    });
    REQUIRE(active != components.end());
    REQUIRE(active->info.tag);

    const auto singleton = std::find_if(components.begin(), components.end(), [&](const ashiato::EntityComponentInfo& info) {
        return info.component == registry.component<GameTime>();
    });
    REQUIRE(singleton == components.end());

    const std::vector<ashiato::EntityComponentInfo> singletons = registry.singleton_components();
    const auto game_time = std::find_if(singletons.begin(), singletons.end(), [&](const ashiato::EntityComponentInfo& info) {
        return info.component == registry.component<GameTime>();
    });
    REQUIRE(game_time != singletons.end());
    REQUIRE(game_time->singleton);
    REQUIRE(game_time->debug_value == "GameTime{}");
}

#ifndef _WIN32
TEST_CASE("debug server responds to a loopback GraphQL request") {
    ashiato::Registry registry;
    register_position_fields(registry);
    register_game_time_fields(registry);
    registry.write<GameTime>().tick = 42;
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{5, 6}) != nullptr);

    ashiato::DebugServer server(registry, "client 1", ashiato::DebugServerOptions{true});
    INFO(server.last_error());
    REQUIRE(server.enabled());
    REQUIRE(server.port() != 0);
    REQUIRE(registry.add<ashiato::DebugName>(entity, ashiato::DebugName{"ball"}) != nullptr);
    const ashiato::Entity duplicate_name = registry.create();
    REQUIRE(registry.add<ashiato::DebugName>(duplicate_name, ashiato::DebugName{"ball"}) != nullptr);

    const std::string body =
        "{\"query\":\"query($id: ID!) { entity(id: $id) { id components { name fields { name type value } } } }\","
        "\"variables\":{\"id\":\"" +
        std::to_string(entity.value) + "\"}}";
    const std::string response = post_graphql(server, body);

    REQUIRE(response.find("200 OK") != std::string::npos);
    REQUIRE(response.find("\"entity\"") != std::string::npos);
    REQUIRE(response.find(std::to_string(entity.value)) != std::string::npos);
    REQUIRE(response.find("\"fields\"") != std::string::npos);
    REQUIRE(response.find("\"name\":\"x\"") != std::string::npos);
    REQUIRE(response.find("\"type\":\"i32\"") != std::string::npos);
    REQUIRE(response.find("\"value\":\"5\"") != std::string::npos);
    REQUIRE(response.find("\"name\":\"name\"") != std::string::npos);
    REQUIRE(response.find("\"type\":\"string\"") != std::string::npos);
    REQUIRE(response.find("\"value\":\"ball\"") != std::string::npos);

    const std::string duplicated_entities_response =
        post_graphql(server, "{\"query\":\"query { entities { id index version kind displayName } }\"}");
    REQUIRE(duplicated_entities_response.find("\"displayName\":\"ball 1\"") != std::string::npos);
    REQUIRE(duplicated_entities_response.find("\"displayName\":\"ball 2\"") != std::string::npos);

    const ashiato::Entity debug_name_component = registry.component<ashiato::DebugName>();
    const std::string rename_body =
        "{\"query\":\"mutation($entity: ID!, $component: ID!, $value: JSON!) { setComponent(entity: $entity, "
        "component: $component, value: $value) { name fields { name type value } } }\","
        "\"variables\":{\"entity\":\"" +
        std::to_string(entity.value) + "\",\"component\":\"" + std::to_string(debug_name_component.value) +
        "\",\"value\":{\"name\":\"hero\"}}}";
    const std::string rename_response = post_graphql(server, rename_body);
    REQUIRE(rename_response.find("200 OK") != std::string::npos);
    REQUIRE(rename_response.find("\"name\":\"DebugName\"") != std::string::npos);
    REQUIRE(rename_response.find("\"value\":\"hero\"") != std::string::npos);

    const std::string name_response = post_graphql(server, "{\"query\":\"query { serverName }\"}");
    REQUIRE(name_response.find("200 OK") != std::string::npos);
    REQUIRE(name_response.find("\"serverName\":\"client 1\"") != std::string::npos);

    const std::string entities_response =
        post_graphql(server, "{\"query\":\"query { entities { id index version kind displayName } }\"}");
    REQUIRE(entities_response.find("\"displayName\":\"hero\"") != std::string::npos);
    REQUIRE(entities_response.find("\"displayName\":\"ball\"") != std::string::npos);
    REQUIRE(entities_response.find("\"id\":\"" + std::to_string(registry.singleton_storage_entity().value) + "\"") ==
            std::string::npos);

    const std::string singletons_response = post_graphql(
        server,
        "{\"query\":\"query { singletons { entity { id index version kind } component { component name singleton "
        "debugValue fields { name type value } } } }\"}");
    REQUIRE(singletons_response.find("200 OK") != std::string::npos);
    REQUIRE(singletons_response.find("\"name\":\"GameTime\"") != std::string::npos);
    REQUIRE(singletons_response.find("\"singleton\":true") != std::string::npos);
    REQUIRE(singletons_response.find("\"debugValue\":\"GameTime{tick=42}\"") != std::string::npos);
    REQUIRE(singletons_response.find("\"value\":\"42\"") != std::string::npos);
}
#endif
