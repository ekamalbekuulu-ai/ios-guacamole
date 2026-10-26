#pragma once
/*
 * Shim board constants. Select the device with
 *
 *     BOARD=guacamole shim/build.sh ../shim/out/payload.bin
 *
 * (default: sweet). Each boards/<name>.h defines the framebuffer the
 * bootloader leaves lit, the GIC and the SoC reset registers. Keep the
 * values in sync with tools/devices.py, which feeds the same numbers into
 * the Apple Device Tree and the xnu boot_args.
 */
#ifndef BOARD_HEADER
#define BOARD_HEADER "boards/sweet.h"
#endif
#include BOARD_HEADER
