// reset_log.h -- /DEVICE/resets.log: why the device restarted, kept on the card.
//
// The reset reason used to appear only on the serial console -- exactly what
// is lost when the board drops off USB (2026-09-14: twice, under load). Each
// boot appends one line. The device also saves a "last alive" time every
// minute once its clock is synced, so the line written after an outage says
// roughly when the previous run ended:
//
//   boot=12 reset=brownout last_alive=1789390000
//
// "power-on" after an unplanned stop means the board lost power; "brownout",
// "panic" or a watchdog means it reset itself.

#pragma once

#include <stdint.h>

#include "hal/storage.h"

namespace net {

// Past this size the older half is dropped, so a board rebooting in a loop
// cannot fill the card with this file.
constexpr uint32_t kResetLogMaxBytes = 8192;
constexpr uint32_t kResetLogKeepBytes = 4096;

/** Appends one line for this boot. `last_alive_unix` is 0 when unknown. */
bool AppendResetRecord(hal::IStorage* storage, uint32_t boot_count, const char* reason,
                       uint32_t last_alive_unix);

/** /DEVICE/alive.bin: the last time (unix seconds) the device was known running. */
bool SaveAliveTime(hal::IStorage* storage, uint32_t unix_seconds);

/** 0 if the file is absent or damaged. */
uint32_t LoadAliveTime(hal::IStorage* storage);

}  // namespace net
