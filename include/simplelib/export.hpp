#pragma once

#if defined(SIMPLELIB_STATIC)
#define SIMPLELIB_API
#elif defined(_WIN32)
#if defined(SIMPLELIB_BUILDING_LIBRARY)
#define SIMPLELIB_API __declspec(dllexport)
#else
#define SIMPLELIB_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SIMPLELIB_API __attribute__((visibility("default")))
#else
#define SIMPLELIB_API
#endif
