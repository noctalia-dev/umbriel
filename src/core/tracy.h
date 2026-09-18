#pragma once

// Zones for the compositor's per-frame work, alongside umbrielfx's own zones
// for the render pass. No-ops without -Dtracy=enabled.
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define UMBRIEL_ZONE(name) ZoneScopedN(name)
#else
#define UMBRIEL_ZONE(name)
#endif
