#pragma once

#include <stdint.h>
#include <stdbool.h>

// Reads the interval this sensor was last assigned, if any. Returns
// false if it's never been provisioned (namespace/key doesn't exist yet).
bool provisioning_load(uint32_t *interval_sec_out);

// Persists the current interval so it survives a full power loss (deep
// sleep itself doesn't touch NVS, so this matters only for that case,
// plus keeping the very first provisioning result around at all).
void provisioning_save(uint32_t interval_sec);
