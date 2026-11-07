#pragma once

#include <stdbool.h>

// True when the OTA slot that holds the second firmware (xiaozhi-esp32)
// contains a bootable image. An empty slot is a valid state: the box ships
// with moonstation only.
bool bootswitch_xiaozhi_available(void);

// Selects the xiaozhi slot as the next boot partition. The caller restarts;
// false means the slot is empty or the bootloader refused the choice, and the
// box stays in this firmware.
bool bootswitch_to_xiaozhi(void);

// Watches the BOOT button (GPIO 0, unused by every other mode): a two second
// hold switches to xiaozhi. xiaozhi runs the same gesture in the other
// direction, so neither firmware can trap the box. Call once, after the audio
// and display are up; it starts a task and returns.
void bootswitch_start_button_monitor(void);
