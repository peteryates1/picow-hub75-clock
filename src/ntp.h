#ifndef NTP_H
#define NTP_H

#include <stdbool.h>
#include <time.h>

// Minimal SNTP client over lwIP UDP (raw API).
//
// Resolves NTP_SERVER, sends one request, and waits for the reply. On success
// the UTC epoch is written to *out_epoch. Blocking with a timeout; intended to
// be called from core0 with cyw43_arch already initialised and connected.
//
// Returns true on success, false on DNS/timeout/error.
bool ntp_sync(time_t *out_epoch, uint32_t timeout_ms);

#endif // NTP_H
