#ifndef POWER_H
#define POWER_H

// Power profiles shared by every module.
//
// The ESP-IDF power manager already does the fine-grained work: it drops the
// CPU clock as soon as both cores idle and raises it again for a driver that
// holds an APB lock. What it does not decide is the ceiling. The setup page
// wants 240 MHz, everything else is fine with 80. Modules request a profile
// instead of calling esp_pm_configure() themselves, so two of them can no
// longer overwrite each other's configuration.
//
// Requests are refcounted: a profile is active while at least one holder has
// requested it, and the highest active profile wins. The idle floor and the
// light sleep switch stay compile-time values (PM_MIN_CPU_MHZ, PM_LIGHT_SLEEP
// in features.h) - they describe what the chip may do when nothing runs, not
// what a single module wants.

typedef enum {
    // Normal operation: 80 MHz ceiling, drops to PM_MIN_CPU_MHZ while the
    // UI waits for the next event.
    POWER_PROFILE_ACTIVE = 0,
    // Setup page over HTTPS: 240 MHz ceiling, held for as long as the page
    // runs. TLS is the only workload that gains noticeably from it.
    POWER_PROFILE_BOOST,
    POWER_PROFILE_COUNT
} power_profile_t;

// Applies the initial profile. Call once at boot, after settings_init().
void power_init(void);

// Adds or removes one holder of a profile. A release without a matching
// request is ignored, so a failure path cannot underflow the counter.
void power_request(power_profile_t profile);
void power_release(power_profile_t profile);

#endif // POWER_H
