// firmware/main/i2c_bus.h
//
// Lazily-initialized shared I2C master bus. On the fnk0104 the touch driver
// (FT6336G @ 0x38) and the audio codec (ES8311 @ 0x18) share the same SDA/SCL
// pair (GPIO 16 / GPIO 15). Either subsystem can request the bus handle — the
// first call initializes, subsequent calls return the cached handle.
//
// Guarded by BOARD_HAS_I2C_BUS so builds that don't need it (Waveshare C6,
// simulator) don't pay for the driver.

#pragma once

#include "board_config.h"

#if BOARD_HAS_I2C_BUS
#include "driver/i2c_master.h"

// Returns the shared master-bus handle, or NULL on allocation failure.
// Safe to call from any task; initialization is one-shot and idempotent.
i2c_master_bus_handle_t i2c_bus_get(void);

#endif
