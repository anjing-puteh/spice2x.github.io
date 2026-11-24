#pragma once

#include <cstdint>

// touch2main: redirect touches on the touchscreen (the first non-primary monitor) to the
// primary display while the game runs, without remapping the digitizer in Windows' Tablet PC
// Settings. An invisible overlay covers the touchscreen, swallows its touches and replays
// them on the primary display through Windows touch injection.
namespace touch2main {

    // set by the -touch2main launcher option
    extern bool ENABLED;

    // maximum number of simultaneous contacts touch2main replays
    constexpr uint32_t MAX_CONTACTS = 10;

    // number of contacts Windows touch injection must be initialized for: spice2x's own
    // synthetic/mouse contact (pointer ID 0) plus touch2main's contacts (pointer IDs 1..10)
    uint32_t injection_contact_count();

    // start the overlay thread; does nothing if not enabled or already started
    void start();
}
