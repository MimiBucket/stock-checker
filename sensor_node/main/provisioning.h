#pragma once

#include <stdint.h>
#include <stdbool.h>

// Reads the last-assigned interval. False if never provisioned.
bool provisioning_load(uint32_t *interval_sec_out);

// Persists the interval so it survives a full power loss.
void provisioning_save(uint32_t interval_sec);
