#pragma once
#include <stdint.h>

/* Framebuffer facts captured before the console paints over ABL's splash. */
void probe_fb_capture(void);

/* Print the live-FDT bring-up report for this board (no payload / loader
 * failure). Reads /memory, /reserved-memory, /cpus, /chosen and the MMIO
 * nodes the drivers need, so a first boot on a new board audits every
 * address the port was built with. */
void probe_report(uintptr_t dtb);
