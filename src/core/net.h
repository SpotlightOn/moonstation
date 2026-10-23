#ifndef NET_H
#define NET_H

#include <stdbool.h>

// Radio and time sync. The WiFi, netif and SNTP stack is always linked,
// because the clock syncs on every boot; the setup web server reuses it
// instead of carrying its own copy of the bring-up.

// True when WiFi credentials are configured in settings. Checked at runtime
// on purpose: it keeps the whole stack referenced, so a firmware built
// without credentials still links the WiFi code and simply never uses it.
bool net_wifi_configured(void);

// Brings the station interface up and waits for an address. Returns false
// when no credentials are set or the access point does not answer, so the
// caller can skip a pointless wait.
bool net_wifi_up(void);

// Stops the station interface. A held link stays up, see net_wifi_hold().
void net_wifi_down(void);

// True while the station interface holds an address.
bool net_wifi_is_up(void);

// Keeps the radio up across net_wifi_down() for as long as a counter of
// holders is above zero (the setup server holds it while it listens).
void net_wifi_hold(void);
void net_wifi_release(void);

// Runs one SNTP poll and blocks until the system clock is set. Returns false
// when no answer arrives within the timeout.
bool net_ntp_sync(void);

#endif // NET_H
