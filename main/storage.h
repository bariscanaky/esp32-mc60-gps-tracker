#pragma once

#include <stdbool.h>
#include <stddef.h>

// Initializes NVS (erasing and reinitializing on a corrupt/incompatible
// partition -- standard ESP-IDF nvs_flash_init() boilerplate), opens this
// app's storage namespace, and records the current boot's reset reason into
// a running per-reason counter. Call once from app_main(), before anything
// else touches storage_*. If NVS can't be opened, every other storage_*
// call becomes a safe no-op/false for the rest of this boot -- persistence
// is a nice-to-have, never a reason to halt the tracker.
void storage_init(void);

// Persists the given fix so it survives a reboot. Overwrites any previous
// value. Call at most once per successful GPS read (each call is an NVS
// commit), not from a tight loop.
void storage_save_last_fix(float lat, float lon);

// Loads the most recently persisted fix (e.g. from a prior boot, or earlier
// this boot). Returns false if nothing has ever been saved.
bool storage_load_last_fix(float *lat, float *lon);

// Formats a one-line summary of accumulated reset-reason counts since NVS
// was first initialized, e.g. "BOR:2 PANIC:0 SW:1 OTHER:0". Truncates to
// fit buf_size.
void storage_format_reset_summary(char *buf, size_t buf_size);
