#pragma once

#include "ecs/ecs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#ifndef ECS_ENABLE_DEBUG_SERVER
#define ECS_ENABLE_DEBUG_SERVER 0
#endif

#if ECS_ENABLE_DEBUG_SERVER

namespace ecs {

struct DebugName {
    static constexpr std::size_t storage_size = 32;
    static constexpr std::size_t max_length = storage_size - 1;

    std::array<char, storage_size> bytes{};

    DebugName() = default;

    explicit DebugName(std::string_view name) {
        set(name);
    }

    void set(std::string_view name) {
        bytes.fill('\0');
        const std::size_t length = name.size() < max_length ? name.size() : max_length;
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned char ch = static_cast<unsigned char>(name[i]);
            bytes[i] = ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?';
        }
    }

    std::string str() const {
        const auto end = std::find(bytes.begin(), bytes.end(), '\0');
        return std::string(bytes.begin(), end);
    }
};

struct DebugServerOptions {
    bool enabled = false;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;
    std::size_t max_requests_per_poll = 16;
    std::string name;
};

class DebugServer {
public:
    DebugServer(Registry& registry, DebugServerOptions options);
    DebugServer(Registry& registry, std::string name, DebugServerOptions options);
    DebugServer(const DebugServer&) = delete;
    DebugServer& operator=(const DebugServer&) = delete;
    DebugServer(DebugServer&&) noexcept;
    DebugServer& operator=(DebugServer&&) noexcept;
    ~DebugServer();

    void poll();
    bool enabled() const noexcept;
    std::uint16_t port() const noexcept;
    const std::string& last_error() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ecs

#endif
