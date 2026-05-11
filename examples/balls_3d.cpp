#include "ashiato/ashiato.hpp"
#if ASHIATO_ENABLE_DEBUG_SERVER
#include "ashiato/debug_server.hpp"
#endif

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <random>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Position3 {
    Vector3 value{};
};

struct Velocity3 {
    Vector3 value{};
};

struct Radius {
    float value = 0.25f;
};

struct BallColor {
    std::uint32_t r = 255;
    std::uint32_t g = 255;
    std::uint32_t b = 255;
    std::uint32_t a = 255;
};

struct Glow {};

struct SimulationStep {
    float dt = 0.0f;
};

struct RewindFrame {
    std::uint64_t frame = 0;
    std::optional<ashiato::Registry::Snapshot> checkpoint;
    std::optional<ashiato::Registry::DeltaSnapshot> delta;
};

constexpr int screen_width = 1280;
constexpr int screen_height = 720;
constexpr int ball_count = 180;
constexpr int checkpoint_interval = 60;
constexpr std::size_t max_history_frames = 600;
constexpr float fixed_dt = 1.0f / 60.0f;
constexpr float world_half_size = 8.0f;
constexpr std::uint16_t default_debug_port = 8080;

}  // namespace

namespace ashiato {
template <>
struct is_singleton_component<SimulationStep> : std::true_type {};
}  // namespace ashiato

namespace {

struct ExampleOptions {
    bool debug_server = false;
    std::uint16_t debug_port = default_debug_port;
};

Color to_raylib_color(const BallColor& color) {
    const auto channel = [](std::uint32_t value) {
        return static_cast<unsigned char>(std::min(value, 255U));
    };
    return Color{channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
}

void register_components(ashiato::Registry& registry) {
    const ashiato::Entity f32 = registry.primitive_type(ashiato::PrimitiveType::F32);
    const ashiato::Entity u32 = registry.primitive_type(ashiato::PrimitiveType::U32);
    const ashiato::Entity position = registry.register_component<Position3>("Position3");
    const ashiato::Entity velocity = registry.register_component<Velocity3>("Velocity3");
    const ashiato::Entity radius = registry.register_component<Radius>("Radius");
    const ashiato::Entity ball_color = registry.register_component<BallColor>("BallColor");
    registry.register_component<Glow>("Glow");
    const ashiato::Entity simulation_step = registry.register_component<SimulationStep>("SimulationStep");

    const bool position_fields = registry.set_component_fields(
        position,
        {
            ashiato::ComponentField{"x", offsetof(Position3, value) + offsetof(Vector3, x), f32, 1},
            ashiato::ComponentField{"y", offsetof(Position3, value) + offsetof(Vector3, y), f32, 1},
            ashiato::ComponentField{"z", offsetof(Position3, value) + offsetof(Vector3, z), f32, 1},
        });
    const bool velocity_fields = registry.set_component_fields(
        velocity,
        {
            ashiato::ComponentField{"x", offsetof(Velocity3, value) + offsetof(Vector3, x), f32, 1},
            ashiato::ComponentField{"y", offsetof(Velocity3, value) + offsetof(Vector3, y), f32, 1},
            ashiato::ComponentField{"z", offsetof(Velocity3, value) + offsetof(Vector3, z), f32, 1},
        });
    const bool radius_fields =
        registry.set_component_fields(radius, {ashiato::ComponentField{"value", offsetof(Radius, value), f32, 1}});
    const bool color_fields = registry.set_component_fields(
        ball_color,
        {
            ashiato::ComponentField{"r", offsetof(BallColor, r), u32, 1},
            ashiato::ComponentField{"g", offsetof(BallColor, g), u32, 1},
            ashiato::ComponentField{"b", offsetof(BallColor, b), u32, 1},
            ashiato::ComponentField{"a", offsetof(BallColor, a), u32, 1},
        });
    const bool simulation_step_fields =
        registry.set_component_fields(simulation_step, {ashiato::ComponentField{"dt", offsetof(SimulationStep, dt), f32, 1}});
    if (!position_fields || !velocity_fields || !radius_fields || !color_fields || !simulation_step_fields) {
        throw std::runtime_error("failed to register balls debug component fields");
    }
}

void register_simulation_jobs(ashiato::Registry& registry) {
    registry.job<Position3, Velocity3, const Radius, const SimulationStep>(0).name("Move balls").each(
        [](ashiato::Entity, Position3& position, Velocity3& velocity, const Radius& radius, const SimulationStep& step) {
            const float min_bound = -world_half_size + radius.value;
            const float max_bound = world_half_size - radius.value;

            position.value = Vector3Add(position.value, Vector3Scale(velocity.value, step.dt));

            float* coordinates[] = {&position.value.x, &position.value.y, &position.value.z};
            float* speeds[] = {&velocity.value.x, &velocity.value.y, &velocity.value.z};

            for (int axis = 0; axis < 3; ++axis) {
                if (*coordinates[axis] < min_bound) {
                    *coordinates[axis] = min_bound;
                    *speeds[axis] = std::fabs(*speeds[axis]);
                } else if (*coordinates[axis] > max_bound) {
                    *coordinates[axis] = max_bound;
                    *speeds[axis] = -std::fabs(*speeds[axis]);
                }
            }
        });

    registry.job<BallColor, const SimulationStep>(1).name("Animate glow").with_tags<Glow>().each(
        [](ashiato::Entity, BallColor& color, const SimulationStep& step) {
            const auto shift = [dt = step.dt](std::uint32_t value, float rate) {
                return static_cast<std::uint32_t>(std::fmod(static_cast<float>(value) + rate * dt, 255.0f));
            };
            color.r = shift(color.r, 95.0f);
            color.g = shift(color.g, 145.0f);
            color.b = shift(color.b, 205.0f);
        });
}

void populate_world(ashiato::Registry& registry) {
    std::mt19937 rng(0x5EED);
    std::uniform_real_distribution<float> position_dist(-world_half_size + 0.7f, world_half_size - 0.7f);
    std::uniform_real_distribution<float> velocity_dist(-3.2f, 3.2f);
    std::uniform_real_distribution<float> radius_dist(0.14f, 0.34f);
    std::uniform_int_distribution<int> color_dist(80, 255);

    for (int i = 0; i < ball_count; ++i) {
        const ashiato::Entity entity = registry.create();
        const float radius = radius_dist(rng);
        Vector3 velocity{velocity_dist(rng), velocity_dist(rng), velocity_dist(rng)};
        if (Vector3LengthSqr(velocity) < 0.5f) {
            velocity.x += 1.4f;
            velocity.y += 0.8f;
        }

        registry.add<Position3>(entity, Position3{Vector3{position_dist(rng), position_dist(rng), position_dist(rng)}});
        registry.add<Velocity3>(entity, Velocity3{velocity});
        registry.add<Radius>(entity, Radius{radius});
        registry.add<BallColor>(
            entity,
            BallColor{
                static_cast<std::uint32_t>(color_dist(rng)),
                static_cast<std::uint32_t>(color_dist(rng)),
                static_cast<std::uint32_t>(color_dist(rng)),
                255});
        if (i % 13 == 0) {
            registry.add<Glow>(entity);
        }
    }

    registry.clear_all_dirty<Position3>();
    registry.clear_all_dirty<Velocity3>();
    registry.clear_all_dirty<Radius>();
    registry.clear_all_dirty<BallColor>();
}

void simulate(ashiato::Registry& registry, float dt) {
    registry.write<SimulationStep>().dt = dt;
    registry.run_jobs();
}

class RewindTimeline final : public ashiato::RegistryDirtyFrameBroadcastListener {
public:
    void set_next_capture_frame(std::uint64_t frame) {
        next_capture_frame_ = frame;
    }

    void on_registry_dirty_frame(const ashiato::RegistryDirtyFrame& frame) override {
        capture(frame.registry, next_capture_frame_);
    }

    void capture_checkpoint(ashiato::Registry& registry, std::uint64_t frame) {
        RewindFrame record;
        record.frame = frame;
        record.checkpoint.emplace(registry.create_snapshot());
        push(std::move(record));
    }

    void capture_delta(ashiato::Registry& registry, const RewindFrame& baseline, std::uint64_t frame) {
        RewindFrame record;
        record.frame = frame;
        if (baseline.checkpoint.has_value()) {
            record.delta.emplace(registry.create_delta_snapshot(*baseline.checkpoint));
        } else {
            record.delta.emplace(registry.create_delta_snapshot(*baseline.delta));
        }
        push(std::move(record));
    }

    void capture(ashiato::Registry& registry, std::uint64_t frame) {
        if (size_ == 0 || frame % checkpoint_interval == 0) {
            capture_checkpoint(registry, frame);
            return;
        }

        capture_delta(registry, at(size_ - 1), frame);
    }

    bool restore(ashiato::Registry& registry, std::size_t frame_index) const {
        if (frame_index >= size_) {
            return false;
        }

        std::size_t checkpoint_index = frame_index;
        while (checkpoint_index > 0 && !at(checkpoint_index).checkpoint.has_value()) {
            --checkpoint_index;
        }

        if (!at(checkpoint_index).checkpoint.has_value()) {
            return false;
        }

        registry.restore_snapshot(*at(checkpoint_index).checkpoint);
        for (std::size_t i = checkpoint_index + 1; i <= frame_index; ++i) {
            if (at(i).delta.has_value()) {
                registry.restore_delta_snapshot(*at(i).delta);
            }
        }
        return true;
    }

    std::size_t newest_index() const {
        return size_ == 0 ? 0 : size_ - 1;
    }

    std::uint64_t frame_number(std::size_t index) const {
        return index < size_ ? at(index).frame : 0;
    }

    bool current_is_checkpoint(std::size_t index) const {
        return index < size_ && at(index).checkpoint.has_value();
    }

    std::size_t size() const {
        return size_;
    }

    std::size_t capacity() const {
        return frames_.size();
    }

private:
    RewindFrame& at(std::size_t index) {
        return frames_[(start_ + index) % frames_.size()];
    }

    const RewindFrame& at(std::size_t index) const {
        return frames_[(start_ + index) % frames_.size()];
    }

    void push(RewindFrame record) {
        if (size_ < frames_.size()) {
            frames_[(start_ + size_) % frames_.size()] = std::move(record);
            ++size_;
        } else {
            frames_[start_] = std::move(record);
            start_ = (start_ + 1) % frames_.size();
        }
        discard_unrestorable_prefix();
    }

    void discard_unrestorable_prefix() {
        while (size_ > 0 && !at(0).checkpoint.has_value()) {
            start_ = (start_ + 1) % frames_.size();
            --size_;
        }
    }

    std::vector<RewindFrame> frames_{max_history_frames};
    std::size_t start_ = 0;
    std::size_t size_ = 0;
    std::uint64_t next_capture_frame_ = 0;
};

void sync_display_from_live(ashiato::Registry& live, ashiato::Registry& display) {
    display.restore_snapshot(live.create_snapshot());
}

void draw_world(ashiato::Registry& registry) {
    DrawCubeWires(Vector3{0.0f, 0.0f, 0.0f}, world_half_size * 2.0f, world_half_size * 2.0f, world_half_size * 2.0f, GRAY);

    registry.view<const Position3, const Radius, const BallColor>().each(
        [](ashiato::Entity, const Position3& position, const Radius& radius, const BallColor& color) {
            DrawSphere(position.value, radius.value, to_raylib_color(color));
        });
}

void draw_overlay(
    bool paused,
    std::uint64_t generated_frame,
    std::size_t cursor,
    const RewindTimeline& timeline,
    std::optional<std::uint16_t> debug_port) {
    DrawRectangle(12, 12, 420, debug_port.has_value() ? 156 : 132, Fade(BLACK, 0.72f));
    DrawText(paused ? "Paused" : "Running", 28, 26, 24, paused ? ORANGE : LIME);

    const std::uint64_t display_frame = timeline.frame_number(cursor);
    const char* frame_kind = timeline.current_is_checkpoint(cursor) ? "full checkpoint" : "delta";

    std::string frame_text = "display frame: " + std::to_string(display_frame) +
        " / generated: " + std::to_string(generated_frame);
    std::string history_text = "history: " + std::to_string(timeline.size()) +
        " / " + std::to_string(timeline.capacity()) +
        " frames, checkpoint every " + std::to_string(checkpoint_interval);
    std::string kind_text = std::string("current record: ") + frame_kind;

    DrawText(frame_text.c_str(), 28, 62, 16, RAYWHITE);
    DrawText(history_text.c_str(), 28, 84, 16, RAYWHITE);
    DrawText(kind_text.c_str(), 28, 106, 16, RAYWHITE);
    if (debug_port.has_value()) {
        std::string debug_text = "debug server: http://127.0.0.1:" + std::to_string(*debug_port) + "/graphql";
        DrawText(debug_text.c_str(), 28, 128, 16, SKYBLUE);
    }
    DrawText("Space pause/resume  Left/Right step  A/D scrub  R resume latest", 28, screen_height - 34, 18, RAYWHITE);
}

ExampleOptions parse_options(int argc, char** argv) {
    ExampleOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--debug-server") {
            options.debug_server = true;
            continue;
        }
        const std::string prefix = "--debug-server=";
        if (arg.rfind(prefix, 0) == 0) {
            options.debug_server = true;
            const int port = std::atoi(arg.substr(prefix.size()).c_str());
            if (port > 0 && port <= 65535) {
                options.debug_port = static_cast<std::uint16_t>(port);
            }
            continue;
        }
        const std::string port_prefix = "--debug-port=";
        if (arg.rfind(port_prefix, 0) == 0) {
            const int port = std::atoi(arg.substr(port_prefix.size()).c_str());
            if (port > 0 && port <= 65535) {
                options.debug_port = static_cast<std::uint16_t>(port);
            }
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const ExampleOptions options = parse_options(argc, argv);

    ashiato::Registry live;
    register_components(live);
    register_simulation_jobs(live);
    populate_world(live);

#if ASHIATO_ENABLE_DEBUG_SERVER
    ashiato::DebugServer debug_server(
        live,
        "balls 3d",
        ashiato::DebugServerOptions{
            options.debug_server,
            "127.0.0.1",
            options.debug_port,
            16});
    const std::optional<std::uint16_t> debug_port =
        debug_server.enabled() ? std::optional<std::uint16_t>{debug_server.port()} : std::nullopt;
    if (debug_server.enabled()) {
        std::vector<ashiato::Entity> debug_named_entities;
        live.view<>().each([&](ashiato::Entity entity) {
            if (live.is_user_entity(entity)) {
                debug_named_entities.push_back(entity);
            }
        });
        for (ashiato::Entity entity : debug_named_entities) {
            live.add<ashiato::DebugName>(entity, ashiato::DebugName{"ball"});
        }
    }
#else
    if (options.debug_server) {
        std::fprintf(stderr, "ashiato_balls_3d_example was built without ASHIATO_ENABLE_DEBUG_SERVER=ON\n");
    }
    const std::optional<std::uint16_t> debug_port = std::nullopt;
#endif

    ashiato::Registry display;
    register_components(display);

    RewindTimeline timeline;
    ashiato::RegistryDirtyFrameBroadcaster dirty_frame_broadcaster;
    auto timeline_dirty_subscription = dirty_frame_broadcaster.subscribe(timeline);
    std::uint64_t generated_frame = 0;
    timeline.capture(live, generated_frame);
    std::size_t cursor = timeline.newest_index();
    timeline.restore(display, cursor);

    InitWindow(screen_width, screen_height, "Ashiato ECS 3D balls rewind debugger");
    SetTargetFPS(60);

    Camera3D camera{};
    camera.position = Vector3{15.0f, 13.0f, 15.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    bool paused = false;
    float accumulator = 0.0f;

    while (!WindowShouldClose()) {
#if ASHIATO_ENABLE_DEBUG_SERVER
        debug_server.poll();
#endif
        UpdateCamera(&camera, CAMERA_ORBITAL);

        if (IsKeyPressed(KEY_SPACE)) {
            paused = !paused;
            if (!paused) {
                cursor = timeline.newest_index();
                sync_display_from_live(live, display);
            }
        }

        if (IsKeyPressed(KEY_R)) {
            paused = false;
            cursor = timeline.newest_index();
            sync_display_from_live(live, display);
        }

        if (paused && timeline.size() > 0) {
            int movement = 0;
            if (IsKeyPressed(KEY_LEFT)) {
                --movement;
            }
            if (IsKeyPressed(KEY_RIGHT)) {
                ++movement;
            }
            if (IsKeyDown(KEY_A)) {
                movement -= 3;
            }
            if (IsKeyDown(KEY_D)) {
                movement += 3;
            }

            if (movement != 0) {
                const int next = std::clamp(
                    static_cast<int>(cursor) + movement,
                    0,
                    static_cast<int>(timeline.newest_index()));
                cursor = static_cast<std::size_t>(next);
                timeline.restore(display, cursor);
            }
        }

        if (!paused) {
            accumulator += GetFrameTime();
            while (accumulator >= fixed_dt) {
                simulate(live, fixed_dt);
                ++generated_frame;
                timeline.set_next_capture_frame(generated_frame);
                dirty_frame_broadcaster.broadcast(live);
                cursor = timeline.newest_index();
                timeline.restore(display, cursor);
                accumulator -= fixed_dt;
            }
        }

        BeginDrawing();
        ClearBackground(Color{18, 20, 24, 255});

        BeginMode3D(camera);
        draw_world(display);
        EndMode3D();

        draw_overlay(paused, generated_frame, cursor, timeline, debug_port);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
