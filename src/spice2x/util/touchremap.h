#pragma once

#include <string>

/*
 * Touch monitor remap
 *
 * Reproduces what the "MultiDigiMon -touch" setup wizard does, without the wizard:
 *   1. snapshot the current HKCU\Software\Microsoft\Wisp\Pen\Digimon key ("normal" mapping)
 *   2. import a user supplied .reg file ("game" mapping)
 *   3. disable + re-enable the touch screen HID device so Windows re-reads the mapping
 * and undoes it again on shutdown.
 *
 * Only active in fullscreen mode (GRAPHICS_WINDOWED == false) and only when running
 * as administrator (disabling a device requires it).
 */
namespace touchremap {

    // if a previous session did not get to restore the normal mapping (crash, killed
    // process, ...), restore it now. safe to call every launch; does nothing when
    // there is nothing to recover.
    void recover_pending();

    // apply the game mapping.
    //   game_reg_path:   path to the .reg file exported while the wizard mapping was active
    //   normal_reg_path: optional .reg file to restore; empty snapshots the current mapping
    //   device_override: touch screen HID instance ID, e.g.
    //                    HID\VID_2A94&PID_564D&MI_00\9&2B0BEF74&0&0000
    //                    leave empty to auto-detect (works if exactly one touch screen exists)
    // must be called after GRAPHICS_WINDOWED is final and before touch is initialized.
    void apply(const std::string &game_reg_path, const std::string &normal_reg_path,
            const std::string &device_override);

    // restore the normal mapping. idempotent; safe to call multiple times.
    void restore();

    // time shutdown code should allow for restore() before force-killing the process;
    // 0 when no restore is pending.
    unsigned int restore_budget_ms();
}
