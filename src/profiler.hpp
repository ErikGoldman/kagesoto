#pragma once

#if defined(ECS_ENABLE_PROFILING) && ECS_ENABLE_PROFILING
#include <tracy/Tracy.hpp>

#define ECS_PROFILE_ZONE(name) ZoneScopedN(name)
#define ECS_PROFILE_FRAME(name) FrameMarkNamed(name)
#define ECS_PROFILE_TEXT(text) TracyMessageL(text)

#else

#define ECS_PROFILE_ZONE(name) ((void)0)
#define ECS_PROFILE_FRAME(name) ((void)0)
#define ECS_PROFILE_TEXT(text) ((void)0)

#endif
