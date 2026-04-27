#include "ecs/ecs.hpp"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>

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
    unsigned char r = 255;
    unsigned char g = 255;
    unsigned char b = 255;
    unsigned char a = 255;
};

struct RewindFrame {
    std::uint64_t frame = 0;
    std::optional<ecs::Registry::Snapshot> checkpoint;
    std::optional<ecs::Registry::DeltaSnapshot> delta;
};

constexpr int screen_width = 1280;
constexpr int screen_height = 720;
constexpr int ball_count = 180;
constexpr int checkpoint_interval = 60;
constexpr std::size_t max_history_frames = 600;
constexpr float fixed_dt = 1.0f / 60.0f;
constexpr float world_half_size = 8.0f;

Color to_raylib_color(const BallColor& color) {
    return Color{color.r, color.g, color.b, color.a};
}

void register_components(ecs::Registry& registry) {
    registry.register_component<Position3>("Position3");
    registry.register_component<Velocity3>("Velocity3");
    registry.register_component<Radius>("Radius");
    registry.register_component<BallColor>("BallColor");
}

void populate_world(ecs::Registry& registry) {
    std::mt19937 rng(0x5EED);
    std::uniform_real_distribution<float> position_dist(-world_half_size + 0.7f, world_half_size - 0.7f);
    std::uniform_real_distribution<float> velocity_dist(-3.2f, 3.2f);
    std::uniform_real_distribution<float> radius_dist(0.14f, 0.34f);
    std::uniform_int_distribution<int> color_dist(80, 255);

    for (int i = 0; i < ball_count; ++i) {
        const ecs::Entity entity = registry.create();
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
                static_cast<unsigned char>(color_dist(rng)),
                static_cast<unsigned char>(color_dist(rng)),
                static_cast<unsigned char>(color_dist(rng)),
                255});
    }

    registry.clear_all_dirty<Position3>();
    registry.clear_all_dirty<Velocity3>();
    registry.clear_all_dirty<Radius>();
    registry.clear_all_dirty<BallColor>();
}

void simulate(ecs::Registry& registry, float dt) {
    registry.view<Position3, Velocity3, const Radius>().each(
        [dt](ecs::Entity, Position3& position, Velocity3& velocity, const Radius& radius) {
            const float min_bound = -world_half_size + radius.value;
            const float max_bound = world_half_size - radius.value;

            position.value = Vector3Add(position.value, Vector3Scale(velocity.value, dt));

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
}

class RewindTimeline {
public:
    void capture_checkpoint(ecs::Registry& registry, std::uint64_t frame) {
        RewindFrame record;
        record.frame = frame;
        record.checkpoint.emplace(registry.snapshot());
        frames_.push_back(std::move(record));
        trim();
    }

    void capture_delta(ecs::Registry& registry, const RewindFrame& baseline, std::uint64_t frame) {
        RewindFrame record;
        record.frame = frame;
        if (baseline.checkpoint.has_value()) {
            record.delta.emplace(registry.delta_snapshot(*baseline.checkpoint));
        } else {
            record.delta.emplace(registry.delta_snapshot(*baseline.delta));
        }
        frames_.push_back(std::move(record));
        trim();
    }

    void capture(ecs::Registry& registry, std::uint64_t frame) {
        if (frames_.empty() || frame % checkpoint_interval == 0) {
            capture_checkpoint(registry, frame);
            return;
        }

        capture_delta(registry, frames_.back(), frame);
    }

    bool restore(ecs::Registry& registry, std::size_t frame_index) const {
        if (frame_index >= frames_.size()) {
            return false;
        }

        std::size_t checkpoint_index = frame_index;
        while (checkpoint_index > 0 && !frames_[checkpoint_index].checkpoint.has_value()) {
            --checkpoint_index;
        }

        if (!frames_[checkpoint_index].checkpoint.has_value()) {
            return false;
        }

        registry.restore(*frames_[checkpoint_index].checkpoint);
        for (std::size_t i = checkpoint_index + 1; i <= frame_index; ++i) {
            if (frames_[i].delta.has_value()) {
                registry.restore(*frames_[i].delta);
            }
        }
        return true;
    }

    std::size_t newest_index() const {
        return frames_.empty() ? 0 : frames_.size() - 1;
    }

    std::uint64_t frame_number(std::size_t index) const {
        return index < frames_.size() ? frames_[index].frame : 0;
    }

    bool current_is_checkpoint(std::size_t index) const {
        return index < frames_.size() && frames_[index].checkpoint.has_value();
    }

    std::size_t size() const {
        return frames_.size();
    }

private:
    void trim() {
        while (frames_.size() > max_history_frames) {
            auto next_checkpoint = std::find_if(
                std::next(frames_.begin()),
                frames_.end(),
                [](const RewindFrame& frame) {
                    return frame.checkpoint.has_value();
                });

            if (next_checkpoint == frames_.end()) {
                break;
            }

            frames_.erase(frames_.begin(), next_checkpoint);
        }
    }

    std::deque<RewindFrame> frames_;
};

void draw_world(ecs::Registry& registry) {
    DrawCubeWires(Vector3{0.0f, 0.0f, 0.0f}, world_half_size * 2.0f, world_half_size * 2.0f, world_half_size * 2.0f, GRAY);

    registry.view<const Position3, const Radius, const BallColor>().each(
        [](ecs::Entity, const Position3& position, const Radius& radius, const BallColor& color) {
            DrawSphere(position.value, radius.value, to_raylib_color(color));
        });
}

void draw_overlay(
    bool paused,
    std::uint64_t generated_frame,
    std::size_t cursor,
    const RewindTimeline& timeline) {
    DrawRectangle(12, 12, 372, 132, Fade(BLACK, 0.72f));
    DrawText(paused ? "Paused" : "Running", 28, 26, 24, paused ? ORANGE : LIME);

    const std::uint64_t display_frame = timeline.frame_number(cursor);
    const char* frame_kind = timeline.current_is_checkpoint(cursor) ? "full checkpoint" : "delta";

    std::string frame_text = "display frame: " + std::to_string(display_frame) +
        " / generated: " + std::to_string(generated_frame);
    std::string history_text = "history: " + std::to_string(timeline.size()) +
        " frames, checkpoint every " + std::to_string(checkpoint_interval);
    std::string kind_text = std::string("current record: ") + frame_kind;

    DrawText(frame_text.c_str(), 28, 62, 16, RAYWHITE);
    DrawText(history_text.c_str(), 28, 84, 16, RAYWHITE);
    DrawText(kind_text.c_str(), 28, 106, 16, RAYWHITE);
    DrawText("Space pause/resume  Left/Right step  A/D scrub  R resume latest", 28, screen_height - 34, 18, RAYWHITE);
}

}  // namespace

int main() {
    ecs::Registry live;
    register_components(live);
    populate_world(live);

    ecs::Registry display;
    register_components(display);

    RewindTimeline timeline;
    std::uint64_t generated_frame = 0;
    timeline.capture(live, generated_frame);
    std::size_t cursor = timeline.newest_index();
    timeline.restore(display, cursor);

    InitWindow(screen_width, screen_height, "ECS 3D balls rewind debugger");
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
        UpdateCamera(&camera, CAMERA_ORBITAL);

        if (IsKeyPressed(KEY_SPACE)) {
            paused = !paused;
            if (!paused) {
                cursor = timeline.newest_index();
                timeline.restore(live, cursor);
                timeline.restore(display, cursor);
            }
        }

        if (IsKeyPressed(KEY_R)) {
            paused = false;
            cursor = timeline.newest_index();
            timeline.restore(live, cursor);
            timeline.restore(display, cursor);
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
                timeline.capture(live, generated_frame);
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

        draw_overlay(paused, generated_frame, cursor, timeline);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
