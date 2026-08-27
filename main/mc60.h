#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Brings up the MC60 UART link. Call once before anything else in this file.
void mc60_init(void);

// Drains whatever the MC60 has sent since the last call into the rolling
// buffer, then parses at most one +CMT: URC out of it. Call once per main
// loop iteration -- mirrors "pump serial into mc60Buffer, then checkForSMS()"
// from the original Arduino loop().
void mc60_pump(void);

// Blocking AT helper. Sends `cmd` (unless empty) and then waits the FULL
// `timeout_ms`, regardless of whether a recognizable response has already
// arrived -- this is deliberately preserved from the original firmware, not
// a bug introduced here. Every byte received is appended to the rolling
// buffer (so an SMS URC arriving mid-transaction isn't lost) and, space
// permitting, copied into `out_resp` (pass NULL/0 to discard the response).
// Returns the number of bytes written into out_resp.
size_t mc60_send_command(const char *cmd, uint32_t timeout_ms, bool print,
                          char *out_resp, size_t out_resp_size);

// Last-10-digits match, so "+905551234567", "05551234567" and "5551234567"
// all match the same allowlist entry.
bool mc60_is_authorized(const char *number);

// Number of entries in the AUTHORIZED_NUMBERS allowlist (from credentials.h).
size_t mc60_authorized_count(void);

// Pops the oldest queued SMS command (enqueued by mc60_pump() from an
// authorized sender) into the caller's buffers. Returns false if the queue
// is empty.
bool mc60_dequeue_sms(char *out_sender, size_t sender_size,
                      char *out_text, size_t text_size);

// Sends a text SMS to `number`.
void mc60_send_sms(const char *number, const char *text);

// Replies to `reply_to` with the current GPS fix as a Google Maps link, or a
// "no fix" message if none is available.
void mc60_send_gps_via_sms(const char *reply_to);

// Reads the MC60's current NMEA GGA sentence and extracts lat/lon. Returns
// false if there's no valid fix.
bool mc60_get_gps_coordinates(float *lat, float *lon);

// Publishes the given coordinates to the configured Adafruit IO feed over
// the already-open MQTT connection. Returns false on failure.
bool mc60_publish_to_adafruit_io(float lat, float lon);
