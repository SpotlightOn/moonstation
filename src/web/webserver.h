#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdbool.h>

// Setup web server: an HTTPS page holding the runtime settings, started and
// stopped by the 5 s touch gesture. It runs in its own task, because the
// WiFi connect and the TLS handshake block for seconds that the touch task
// must not spend.

// Brings the radio up, waits for an address and opens the page. Starting it
// twice is a no-op. Safe to call from any task.
void web_start(void);

// Closes the page and takes the radio down again. Stopping it twice is a
// no-op. Safe to call from any task.
void web_stop(void);

// One call for the gesture: starts the server, or stops it when it already
// runs.
void web_toggle(void);

// True while the page is reachable.
bool web_is_running(void);

#endif // WEBSERVER_H
