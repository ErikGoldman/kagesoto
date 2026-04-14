#pragma once

#if defined(ECS_STATIC)
#define ECS_API
#elif defined(_WIN32)
#if defined(ECS_BUILDING_LIBRARY)
#define ECS_API __declspec(dllexport)
#else
#define ECS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ECS_API __attribute__((visibility("default")))
#else
#define ECS_API
#endif
